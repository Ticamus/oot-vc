#!/usr/bin/env python3
r"""Bridge an OoTMM multiplayer client to a Wii VC emulator.

The Wii can't provide the IPC channels OoTMM expects, so comboNet.c connects to this
relay over TCP. The relay exposes it as a Windows named pipe or Unix socket.

One relay handles one game, with the TCP port used to identify the IPC channel:

    tools/ipc_relay.py -p 14237  # -> pj64em-ipc.14237 / wii-14237.sock

The relay also converts between the client's message framing and the TCP/game framing
(see frame_to_game).

It handles OoTMM's short-lived logical sessions too. The game can end and restart a
session several times a minute without closing the connection, so the relay handles
repeat HELLOs and manages its own sequence numbers instead of forwarding them directly.

The relay is single-threaded because blocking ReadFile on a Windows named pipe would
prevent writes. PeekNamedPipe is used to poll both directions.

The TCP listener starts when the client attaches and stays open so the game can reconnect.
"""

import argparse
import os
import select
import socket
import struct
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
    listener.listen(2)
    return listener


OP_HELLO = 0x01
HELLO_MAGIC = b"OoTMM\x7f\x01\x00"

# Greeting bodies, header excluded: magic + sessionId + secret + playerId + playerName + worldId
# from the game, magic + seqGame + seqNet from the client.
HELLO_IN_SIZE = 57
HELLO_OUT_SIZE = 16

# Frames moved per direction per turn of the loop, so neither side starves the other.
BATCH = 32

# Framing towards the game: sync word, true length, check, payload, padding to a multiple of four.
#
#   4F 4D A5 5A   magic "OM\xa5Z"
#   LL LL         the real message length, u16 LE (1..256)
#   CK CK         u16 LE check: the payload bytes summed, plus the length
#   <payload>     LL bytes
#   <padding>     0..3 bytes
GAME_SYNC = b"OM\xa5\x5a"
GAME_HEADER = len(GAME_SYNC) + 4


def frame_to_game(body: bytes) -> bytes:
    """Wraps one message in the sync word, its length and check, and padding to a multiple of four."""
    check = (sum(body) + len(body)) & 0xFFFF
    return (GAME_SYNC + struct.pack("<HH", len(body), check) + body + bytes((-len(body)) & 3))


class GameGone(Exception):
    """The game's socket ended. The client keeps its session and waits for the next one."""


def game_identity(payload: bytes) -> bytes:
    """The part of a greeting that says which game this is.
    """
    return payload[8:48] + payload[56:57]


class Session:
    """Everything between one client attaching and that client going away.
    """

    def __init__(self, channel, port: int) -> None:
        self.channel = channel
        self.port = port
        self.listener = listen_tcp(port)
        self.game = None
        self.hello = None       # identity from the greeting that was forwarded to the client
        self.ready = False      # the client has answered it, so the counters below are real
        self.seq_game = 0       # next number to stamp on a game -> client message
        self.seq_net = 0        # next number to stamp on a client -> game message
        self.to_client = 0
        self.to_game = 0
        self.answered = 0
        self.discarded = 0
        self.links = 0

    def attach_game(self) -> None:
        game, address = self.listener.accept()
        game.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        # A console that rebooted dials again while the old socket is still half-open here.
        if self.game is not None:
            print("relay: a second game connected, dropping the first")
            self.close_game()

        self.game = game
        self.links += 1
        print(f"relay: game connected from {address[0]}"
              f"{' (link #%d)' % self.links if self.links > 1 else ''}")

    def close_game(self) -> None:
        if self.game is None:
            return

        try:
            self.game.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

        self.game.close()
        self.game = None

    def recv_frame(self) -> bytes:
        try:
            header = recv_exactly(self.game, 4)
        except Closed as error:
            raise GameGone("it closed its socket") from error

        size = struct.unpack("<I", header)[0]

        if size == 0 or size > MAX_MESSAGE:
            raise GameGone(f"it announced a bogus length ({size})")

        try:
            return recv_exactly(self.game, size)
        except Closed as error:
            raise GameGone(f"it cut a message short (wanted {size})") from error

    def send_game(self, body: bytes) -> None:
        wire = frame_to_game(body)

        try:
            self.game.sendall(wire)
        except OSError as error:
            raise GameGone("it stopped accepting data") from error

        # The exact bytes on the wire for the first few frames, to compare against what the emulator
        # reports receiving (`f<n>` on its overlay).
        if self.to_game < 3:
            print(f"relay: -> game #{self.to_game}: {len(body)} bytes, "
                  f"wire {wire[:36].hex()}")

        self.to_game += 1

    def answer_hello(self) -> None:
        """Replies to a greeting ourselves, with the counters the client is actually at."""
        body = (struct.pack(">IB", 0, OP_HELLO) + HELLO_MAGIC +
                struct.pack(">II", self.seq_game, self.seq_net))
        self.send_game(body)
        self.answered += 1

        # Loud for the first few, then occasional: this is normal and fires several times a minute,
        # but a count stuck at 0 while sessions churn would mean the greeting is not recognised.
        if self.answered <= 3 or self.answered % 50 == 0:
            print(f"relay: answered a re-greeting ({self.answered}) with "
                  f"seqGame={self.seq_game} seqNet={self.seq_net}")

    def handle_from_game(self, body: bytes) -> None:
        if len(body) < 5:
            raise GameGone(f"it sent {len(body)} bytes, too few for a header")

        if body[4] == OP_HELLO:
            payload = body[5:]

            if len(payload) < HELLO_IN_SIZE:
                raise GameGone(f"its greeting was {len(payload)} bytes, want {HELLO_IN_SIZE}")

            identity = game_identity(payload)

            if self.hello is None:
                self.hello = identity
                self.channel.send(body)
                self.to_client += 1
                print(f"relay: greeting forwarded to the client ({len(body)} bytes)")
                return

            if identity != self.hello:
                raise Closed("the game greeted with a different session")

            if not self.ready:
                return

            self.answer_hello()
            return

        if self.hello is None:
            # The client is still blocked in its own hello(), which accepts nothing but a greeting.
            self.discarded += 1
            return

        self.channel.send(struct.pack(">I", self.seq_game) + body[4:])
        self.seq_game = (self.seq_game + 1) & 0xFFFFFFFF
        self.to_client += 1

    def handle_from_client(self, body: bytes) -> None:
        if len(body) < 5:
            print(f"relay: WARNING dropped a {len(body)}-byte message from the client, no header")
            return

        if body[4] == OP_HELLO:
            payload = body[5:]

            if len(payload) < HELLO_OUT_SIZE:
                raise Closed(f"its greeting was {len(payload)} bytes, want {HELLO_OUT_SIZE}")

            self.seq_game = int.from_bytes(payload[8:12], "big")
            self.seq_net = int.from_bytes(payload[12:16], "big")
            self.ready = True
            print(f"relay: client answered the greeting, seqGame={self.seq_game} "
                  f"seqNet={self.seq_net}")

            if self.game is not None:
                self.send_game(body)

            return

        if self.game is None:
            self.discarded += 1
            return

        self.send_game(struct.pack(">I", self.seq_net) + body[4:])
        self.seq_net = (self.seq_net + 1) & 0xFFFFFFFF

    def pump_game(self) -> bool:
        moved = 0

        while moved < BATCH and self.game is not None:
            if not select.select([self.game], [], [], 0)[0]:
                break

            self.handle_from_game(self.recv_frame())
            moved += 1

        return moved != 0

    def pump_client(self) -> bool:
        moved = 0

        while moved < BATCH and self.channel.readable(0):
            message = self.channel.recv()

            if not message:
                print("relay: WARNING dropped an empty message from the client")
                continue

            self.handle_from_client(message)
            moved += 1

        return moved != 0

    def drop_link(self, gone: GameGone) -> None:
        print(f"relay: game link #{self.links} ended: {gone} "
              f"({self.to_client} message(s) to the client, {self.to_game} to the game so far)")
        self.close_game()

    def run(self) -> None:
        """Runs until the client goes away. A game that goes away is only an interruption."""
        reason = "unknown"

        try:
            while True:
                busy = False

                if select.select([self.listener], [], [], 0)[0]:
                    self.attach_game()
                    busy = True

                try:
                    if self.game is not None:
                        busy = self.pump_game() or busy

                    busy = self.pump_client() or busy
                except GameGone as gone:
                    self.drop_link(gone)
                    busy = True

                if not busy:
                    time.sleep(TICK)
        except Closed as why:
            reason = str(why) or "the client hung up"
        except OSError as error:
            reason = f"the client channel failed ({error})"
        finally:
            self.close_game()
            self.listener.close()
            self.channel.close_session()
            print(f"relay: session over after {self.to_client} message(s) to the client, "
                  f"{self.to_game} to the game, {self.answered} re-greeting(s) answered, "
                  f"{self.links} game link(s), {self.discarded} message(s) discarded "
                  f"with no game attached: {reason}")


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
            Session(channel, args.port).run()
            print(f"relay: waiting for the OoTMM client on {channel.describe()}")
    except KeyboardInterrupt:
        pass
    finally:
        channel.close()


if __name__ == "__main__":
    main()
