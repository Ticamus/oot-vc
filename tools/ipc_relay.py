#!/usr/bin/env python3
r"""Bridge an OoTMM multiplayer client to a Wii VC emulator instance.

The Wii cannot expose the IPC channels expected by the OoTMM client, so comboNet.c
connects over TCP and this relay republishes the stream as either a Windows named
pipe or a Unix socket.

One relay handles one game. The IPC channel uses the TCP port as its name, allowing
multiple emulator instances to run side by side:

    tools/ipc_relay.py -p 14237  # -> pj64em-ipc.14237 / wii-14237.sock

The relay also translates framing: TCP uses a 4-byte little-endian length prefix,
while the IPC channels send one message per read/write. Payload fields are unchanged.

The relay is single-threaded because synchronous Windows named pipes serialize
operations on a handle: a thread blocked in ReadFile would prevent WriteFile and
deadlock the initial HELLO. PeekNamedPipe lets us poll both directions instead.

Finally, the TCP listener is only opened after the client attaches. Otherwise the
game could connect before the relay is ready, queueing multiple HELLOs; OoTMM rejects
a session that sends HELLO twice. Refusing the connection instead makes the game
retry after COMBO_IPC_RETRY_US.
"""

import argparse
import os
import select
import socket
import struct
import sys
import tempfile
import time

DEFAULT_PORT = 14237
MAX_MESSAGE = 4096
TICK = 0.01


class Closed(Exception):
    """Either end went away; the session is over."""


def recv_exactly(sock, count: int) -> bytes:
    if count == 0 or count > MAX_MESSAGE:
        raise Closed

    data = b""

    while len(data) < count:
        try:
            chunk = sock.recv(count - len(data))
        except OSError as error:
            raise Closed from error

        if not chunk:
            raise Closed

        data += chunk

    return data


# --------------------------------------------------------------------------------------------------
# The client side, one implementation per channel. Both speak whole messages, so the pump above them
# does not care which one it is talking to.
# --------------------------------------------------------------------------------------------------


class UnixChannel:
    """Unix socket, length-prefixed like client/internal/ipc/unixsock.go."""

    def __init__(self, path: str) -> None:
        self.path = path
        os.makedirs(os.path.dirname(path), exist_ok=True)

        # A leftover socket file from a previous run would make the client dial into nothing.
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass

        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(path)
        self.listener.listen(1)
        self.conn = None

    def describe(self) -> str:
        return self.path

    def accept(self) -> None:
        self.conn, _ = self.listener.accept()

    def readable(self, timeout: float) -> bool:
        return bool(select.select([self.conn], [], [], timeout)[0])

    def recv(self) -> bytes:
        header = recv_exactly(self.conn, 4)
        return recv_exactly(self.conn, struct.unpack("<I", header)[0])

    def send(self, message: bytes) -> None:
        try:
            self.conn.sendall(struct.pack("<I", len(message)) + message)
        except OSError as error:
            raise Closed from error

    def close_session(self) -> None:
        if self.conn is not None:
            try:
                self.conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

            self.conn.close()
            self.conn = None

    def close(self) -> None:
        self.close_session()
        self.listener.close()

        try:
            os.unlink(self.path)
        except OSError:
            pass


class PipeChannel:
    """Windows message-mode named pipe, as Project64-EM offers it.

    PIPE_TYPE_MESSAGE is not optional: the client calls SetNamedPipeHandleState(PIPE_READMODE_MESSAGE)
    on the handle it opens, and that fails outright on a byte-type pipe. One ReadFile then yields
    exactly one message, which is why nothing here deals with lengths.
    """

    PIPE_ACCESS_DUPLEX = 0x00000003
    PIPE_TYPE_MESSAGE = 0x00000004
    PIPE_READMODE_MESSAGE = 0x00000002
    PIPE_WAIT = 0x00000000
    PIPE_UNLIMITED_INSTANCES = 255
    ERROR_MORE_DATA = 234
    ERROR_PIPE_CONNECTED = 535

    def __init__(self, name: str) -> None:
        import ctypes
        from ctypes import wintypes

        self.ctypes = ctypes
        self.wintypes = wintypes
        self.invalid_handle = ctypes.cast(-1, wintypes.HANDLE).value

        dll = ctypes.WinDLL("kernel32", use_last_error=True)
        dword_p = ctypes.POINTER(wintypes.DWORD)

        dll.CreateNamedPipeW.restype = wintypes.HANDLE
        dll.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                         wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                         wintypes.DWORD, wintypes.LPVOID]
        dll.ConnectNamedPipe.restype = wintypes.BOOL
        dll.ConnectNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID]
        dll.DisconnectNamedPipe.restype = wintypes.BOOL
        dll.DisconnectNamedPipe.argtypes = [wintypes.HANDLE]
        dll.PeekNamedPipe.restype = wintypes.BOOL
        dll.PeekNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                                      dword_p, dword_p, dword_p]
        dll.ReadFile.restype = wintypes.BOOL
        dll.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, dword_p,
                                 wintypes.LPVOID]
        dll.WriteFile.restype = wintypes.BOOL
        dll.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, dword_p,
                                  wintypes.LPVOID]
        dll.CloseHandle.restype = wintypes.BOOL
        dll.CloseHandle.argtypes = [wintypes.HANDLE]

        self.dll = dll
        self.name = name
        self.handle = None

    def _fail(self, what: str) -> OSError:
        return OSError(0, what, None, self.ctypes.get_last_error())

    def describe(self) -> str:
        return self.name

    def accept(self) -> None:
        self.handle = self.dll.CreateNamedPipeW(
            self.name, self.PIPE_ACCESS_DUPLEX,
            self.PIPE_TYPE_MESSAGE | self.PIPE_READMODE_MESSAGE | self.PIPE_WAIT,
            self.PIPE_UNLIMITED_INSTANCES, MAX_MESSAGE, MAX_MESSAGE, 0, None)

        if self.handle is None or self.handle == self.invalid_handle:
            self.handle = None
            raise self._fail("CreateNamedPipeW")

        if not self.dll.ConnectNamedPipe(self.handle, None):
            # A client that got in between Create and Connect is a success, not an error.
            if self.ctypes.get_last_error() != self.ERROR_PIPE_CONNECTED:
                raise self._fail("ConnectNamedPipe")

    def readable(self, timeout: float) -> bool:
        """True when a message is already buffered, so the ReadFile below cannot block.

        Never leave a read in flight: on a synchronous handle it would block the writes too.
        """
        if self.handle is None:
            raise Closed

        available = self.wintypes.DWORD(0)

        if not self.dll.PeekNamedPipe(self.handle, None, 0, None, self.ctypes.byref(available), None):
            raise Closed

        if available.value:
            return True

        time.sleep(timeout)
        return False

    def recv(self) -> bytes:
        buffer = self.ctypes.create_string_buffer(MAX_MESSAGE)
        read = self.wintypes.DWORD(0)
        message = b""

        while True:
            if self.handle is None:
                raise Closed

            ok = self.dll.ReadFile(self.handle, self.ctypes.cast(buffer, self.wintypes.LPVOID),
                                   MAX_MESSAGE, self.ctypes.byref(read), None)
            message += buffer.raw[:read.value]

            if ok:
                return message

            # ERROR_MORE_DATA only means the message was longer than the buffer.
            if self.ctypes.get_last_error() != self.ERROR_MORE_DATA:
                raise Closed

    def send(self, message: bytes) -> None:
        if self.handle is None:
            raise Closed

        written = self.wintypes.DWORD(0)
        data = self.ctypes.cast(self.ctypes.c_char_p(message), self.wintypes.LPVOID)

        if not self.dll.WriteFile(self.handle, data, len(message), self.ctypes.byref(written), None):
            raise Closed

    def close_session(self) -> None:
        if self.handle is not None:
            self.dll.DisconnectNamedPipe(self.handle)
            self.dll.CloseHandle(self.handle)
            self.handle = None

    def close(self) -> None:
        self.close_session()


def default_socket_path(port: int) -> str:
    """Named after the port, so relays for different instances never share a socket file."""
    base = os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir()
    return os.path.join(base, "n64-ipc", f"wii-{port}.sock")


def default_pipe_name(port: int) -> str:
    """Same idea, and it also makes the client's dial order (by name) follow the port numbers."""
    return rf"\\.\pipe\pj64em-ipc.{port}"


def open_channel(args: argparse.Namespace):
    if args.socket:
        return UnixChannel(args.socket)

    if hasattr(socket, "AF_UNIX"):
        return UnixChannel(default_socket_path(args.port))

    # Windows: no socket.AF_UNIX in CPython, and the pipe is what the client looks for first.
    return PipeChannel(args.pipe or default_pipe_name(args.port))


def listen_tcp(port: int) -> socket.socket:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", port))
    listener.listen(1)
    return listener


def pump(channel, game: socket.socket) -> None:
    """Move whole messages both ways until either end goes."""
    try:
        while True:
            if select.select([game], [], [], 0)[0]:
                header = recv_exactly(game, 4)
                channel.send(recv_exactly(game, struct.unpack("<I", header)[0]))
                continue

            if channel.readable(TICK):
                message = channel.recv()
                game.sendall(struct.pack("<I", len(message)) + message)
    except (Closed, OSError):
        pass
    finally:
        try:
            game.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

        game.close()
        channel.close_session()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--port", type=int, default=DEFAULT_PORT,
                        help=f"TCP port the game dials (COMBO_IPC_PORT, default {DEFAULT_PORT}); the "
                             "client-side channel is named after it")
    parser.add_argument("-s", "--socket", default=None,
                        help="serve a unix socket at this path instead of the port-derived default")
    parser.add_argument("--pipe", default=None,
                        help=r"Windows pipe name; must start with \\.\pipe\pj64em-ipc.")
    args = parser.parse_args()

    channel = open_channel(args)
    print(f"relay: waiting for the OoTMM client on {channel.describe()}")

    try:
        while True:
            channel.accept()
            print(f"relay: client attached, opening port {args.port} for the game")
            tcp_listener = listen_tcp(args.port)

            try:
                game, address = tcp_listener.accept()
            finally:
                # Closed before pumping so a second console cannot join mid-session, and so a game
                # that reconnects is refused rather than silently queued.
                tcp_listener.close()

            print(f"relay: game connected from {address[0]}")
            game.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            pump(channel, game)
            print("relay: session closed")
    except KeyboardInterrupt:
        pass
    finally:
        channel.close()


if __name__ == "__main__":
    main()
