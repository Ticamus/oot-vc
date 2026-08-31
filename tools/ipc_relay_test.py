#!/usr/bin/env python3
"""End-to-end test for tools/ipc_relay.py, with a fake game and a fake client.

Runs the real relay as a subprocess and drives both of its ends, so the cases that only ever showed
up on hardware can be checked here: a game that greets again on a live connection, a game that loses
its socket and dials back in, and a game whose sequence numbers have a hole in them.

    python3 tools/ipc_relay_test.py
"""

import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RELAY = os.path.join(HERE, "ipc_relay.py")

OP_HELLO = 0x01
OP_WAL = 0x02
OP_WAL_QUERY = 0x03
OP_POSITION = 0x05

MAGIC = b"OoTMM\x7f\x01\x00"

# Framing of the relay -> game direction, mirroring GAME_SYNC / frame_to_game in ipc_relay.py and
# IPC_SYNC_* / comboIpcPeekRx in comboIpc.c.
GAME_SYNC = b"OM\xa5\x5a"

SEQ_GAME = 0x11110000
SEQ_NET = 0x22220000

TIMEOUT = 5.0

failures = []


def check(name, got, want):
    if got == want:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name}: got {got!r}, want {want!r}")
        failures.append(name)


def message(seq, op, payload=b""):
    return struct.pack(">IB", seq, op) + payload


def hello_in(session=b"S" * 16, secret=b"E" * 8, player=b"P" * 16, name=b"LINK\0\0\0\0", world=0):
    return message(0, OP_HELLO, MAGIC + session + secret + player + name + bytes([world]))


def hello_out(seq_game, seq_net):
    return message(0, OP_HELLO, MAGIC + struct.pack(">II", seq_game, seq_net))


def parse(body):
    return struct.unpack(">I", body[0:4])[0], body[4], body[5:]


class Peer:
    """One end of the relay. Both send with a 4-byte little-endian length prefix.

    Receiving differs: the client's channel is length-prefixed, while the game's direction carries the
    sync word and padding, so `sync` picks which parse to use.
    """

    def __init__(self, sock, sync=False):
        self.sock = sock
        self.sync = sync
        self.buffer = b""
        self.wire = b""

    def send(self, body):
        self.sock.sendall(struct.pack("<I", len(body)) + body)

    def _fill(self, count, deadline):
        while len(self.buffer) < count:
            self.sock.settimeout(max(0.01, deadline - time.time()))
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("peer closed")
            self.buffer += chunk
            self.wire += chunk

    def recv(self, timeout=TIMEOUT):
        deadline = time.time() + timeout
        header = 8 if self.sync else 4

        self._fill(header, deadline)

        if self.sync:
            if self.buffer[:4] != GAME_SYNC:
                raise AssertionError(f"no sync word: {self.buffer[:8].hex()}")
            size, check = struct.unpack("<HH", self.buffer[4:8])
            total = header + ((size + 3) & ~3)
        else:
            size = struct.unpack("<I", self.buffer[:4])[0]
            total = header + size

        self._fill(total, deadline)
        body = self.buffer[header:header + size]
        self.buffer = self.buffer[total:]

        if self.sync and (sum(body) + size) & 0xFFFF != check:
            raise AssertionError(f"bad check on a {size}-byte frame")

        return parse(body)

    def quiet(self, seconds=0.4):
        """Nothing arrives within `seconds`. Used to prove a message was absorbed, not forwarded."""
        try:
            self.sock.settimeout(seconds)
            data = self.sock.recv(4096)
        except socket.timeout:
            return None
        except OSError as error:
            return f"error {error}"

        if not data:
            return "closed"

        self.buffer += data
        return f"{len(data)} bytes"

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()


def frame(body):
    """frame_to_game() in ipc_relay.py: sync word, length, check, payload, padding to a multiple of 4."""
    check = (sum(body) + len(body)) & 0xFFFF
    return GAME_SYNC + struct.pack("<HH", len(body), check) + body + bytes((-len(body)) & 3)


def guest_rx(stream):
    """Returns (messages delivered, resynchronise count) for a byte stream, as comboIpcRead sees it."""
    queue = bytearray(stream)
    out = []
    resyncs = 0

    while True:
        if len(queue) >= 8 and bytes(queue[:4]) == GAME_SYNC:
            size, check = struct.unpack("<HH", queue[4:8])
            total = 8 + ((size + 3) & ~3)

            if size == 0 or size > 256:
                resyncs += 1
                queue = resync(queue)
                continue

            if len(queue) < total:
                return out, resyncs                 # the rest is still on its way

            body = bytes(queue[8:8 + size])

            # A frame that does not add up lost bytes inside its payload, and would otherwise reach
            # the guest with a valid sequence number and a garbled body.
            if (sum(body) + size) & 0xFFFF != check:
                resyncs += 1
                queue = resync(queue)
                continue

            out.append(body)
            del queue[:total]
            continue

        if len(queue) >= 4:
            resyncs += 1
            queue = resync(queue)
            continue

        return out, resyncs


def resync(queue):
    """comboIpcResync: to the next sync word, else keep the last three bytes as a partial one."""
    for offset in range(1, len(queue) - 3):
        if bytes(queue[offset:offset + 4]) == GAME_SYNC:
            return queue[offset:]

    return queue[-3:] if len(queue) > 3 else queue


def free_port():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def dial_game(port):
    deadline = time.time() + TIMEOUT

    while time.time() < deadline:
        sock = socket.socket()
        try:
            sock.connect(("127.0.0.1", port))
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            return Peer(sock, sync=True)
        except OSError:
            sock.close()
            time.sleep(0.05)

    raise AssertionError("the relay never opened its TCP port")


def dial_client(path):
    deadline = time.time() + TIMEOUT

    while time.time() < deadline:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(path)
            return Peer(sock)
        except OSError:
            sock.close()
            time.sleep(0.05)

    raise AssertionError("the relay never served its unix socket")


def main():
    port = free_port()
    path = os.path.join(tempfile.mkdtemp(prefix="ipc-relay-test."), "wii.sock")

    # -u so the relay's own log interleaves with the checks instead of being lost when it is killed.
    relay = subprocess.Popen([sys.executable, "-u", RELAY, "-p", str(port), "-s", path],
                             stdout=sys.stdout, stderr=sys.stderr)

    client = None
    game = None

    try:
        client = dial_client(path)
        game = dial_game(port)

        print("\n-- the first greeting reaches the client, and its answer reaches the game")
        game.send(hello_in())
        check("client saw the greeting", client.recv(), (0, OP_HELLO, hello_in()[5:]))
        client.send(hello_out(SEQ_GAME, SEQ_NET))
        check("game saw the answer", game.recv(), (0, OP_HELLO, MAGIC + struct.pack(">II", SEQ_GAME, SEQ_NET)))

        print("\n-- traffic is numbered from the counters the client chose")
        game.send(message(SEQ_GAME, OP_WAL_QUERY, b"\0\0\0\1"))
        check("first query", client.recv(), (SEQ_GAME, OP_WAL_QUERY, b"\0\0\0\1"))
        client.send(message(SEQ_NET, OP_WAL, b"body"))
        check("first WAL down", game.recv(), (SEQ_NET, OP_WAL, b"body"))

        print("\n-- a re-greeting is answered by the relay and never shown to the client")
        game.send(hello_in())
        check("game got a fresh answer", game.recv(),
              (0, OP_HELLO, MAGIC + struct.pack(">II", SEQ_GAME + 1, SEQ_NET + 1)))
        check("client saw nothing", client.quiet(), None)

        print("\n-- and the stream carries on from there")
        game.send(message(SEQ_GAME + 1, OP_WAL_QUERY, b"\0\0\0\2"))
        check("query after re-greeting", client.recv(), (SEQ_GAME + 1, OP_WAL_QUERY, b"\0\0\0\2"))
        client.send(message(SEQ_NET + 1, OP_POSITION, b"pos"))
        check("position after re-greeting", game.recv(), (SEQ_NET + 1, OP_POSITION, b"pos"))

        print("\n-- a hole in the game's numbering is closed on the way out")
        # What comboIpcMakeRoom() does when the send queue fills: the POSITION frame numbered
        # SEQ_GAME + 2 never leaves the emulator, so the game's next message is numbered + 3.
        game.send(message(SEQ_GAME + 3, OP_WAL_QUERY, b"\0\0\0\3"))
        check("renumbered, no gap", client.recv(), (SEQ_GAME + 2, OP_WAL_QUERY, b"\0\0\0\3"))

        print("\n-- the game can lose its socket and dial back in; the client keeps its session")
        game.close()
        time.sleep(0.2)                                        # let the relay notice, TICK is 0.01
        client.send(message(SEQ_NET + 2, OP_WAL, b"lost"))     # discarded, no game attached
        time.sleep(0.2)
        game = dial_game(port)
        game.send(hello_in())
        check("answer on the new link", game.recv(),
              (0, OP_HELLO, MAGIC + struct.pack(">II", SEQ_GAME + 3, SEQ_NET + 2)))
        check("client still there", client.quiet(), None)
        game.send(message(SEQ_GAME + 3, OP_WAL_QUERY, b"\0\0\0\4"))
        check("query on the new link", client.recv(), (SEQ_GAME + 3, OP_WAL_QUERY, b"\0\0\0\4"))
        client.send(message(SEQ_NET + 3, OP_WAL, b"again"))
        check("WAL down on the new link", game.recv(), (SEQ_NET + 2, OP_WAL, b"again"))

        print("\n-- every frame towards the game is word-aligned and carries the sync word")
        # game.wire holds every byte the relay has put on the socket so far.
        offset = 0
        frames = 0
        while offset < len(game.wire):
            check(f"frame {frames} sync word", game.wire[offset:offset + 4], GAME_SYNC)
            size, _ = struct.unpack("<HH", game.wire[offset + 4:offset + 8])
            total = 8 + ((size + 3) & ~3)
            check(f"frame {frames} length is a multiple of 4", total % 4, 0)
            offset += total
            frames += 1
        check("frames accounted for exactly", offset, len(game.wire))
        print(f"  ({frames} frames, {len(game.wire)} bytes, none misaligned)")

        print("\n-- the relay's own output needs no resynchronising")
        want, resyncs = guest_rx(game.wire)
        check("clean stream needs no resynchronise", resyncs, 0)
        check("clean stream delivers every frame", len(want), frames)

        print("\n-- the guest model resynchronises through a hole instead of dying")
        for name, at in (("on a frame boundary", 0), ("inside a payload", 11), ("in a length field", 5)):
            # Four frames of a size OoTMM really uses, so a damaged one always has a successor to land
            # on. Three bytes is the worst case of the hole IOS leaves, and the one seen on hardware.
            bodies = [bytes([0x30 + i]) * 29 for i in range(4)]
            clean = b"".join(frame(b) for b in bodies)
            offset = len(frame(bodies[0])) + at
            holed = clean[:offset] + clean[offset + 3:]

            got, resyncs = guest_rx(holed)
            check(f"a 3-byte hole {name} loses only its own frame", got,
                  [bodies[0], bodies[2], bodies[3]])
            check(f"a 3-byte hole {name} needs at most two resynchronises", resyncs <= 2, True)

        print("\n-- a greeting from another seed ends the session instead of being answered")
        game.send(hello_in(session=b"X" * 16))
        # A unix socket shut down at both ends answers with ECONNRESET rather than end of file, so
        # either shape counts as gone.
        gone = client.quiet(1.0)
        check("client channel closed", gone == "closed" or "error" in str(gone), True)
    finally:
        for peer in (game, client):
            if peer is not None:
                peer.close()

        relay.terminate()
        relay.wait(timeout=5)

    print()

    if failures:
        print(f"FAILED: {len(failures)} check(s): {', '.join(failures)}")
        return 1

    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
