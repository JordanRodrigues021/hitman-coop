#!/usr/bin/env python3
"""HITMAN Co-op relay server. Created by Jordan Rodrigues.

Lets two players connect with a short join code instead of an IP address, through any router or
NAT, without port forwarding or a VPN. The host's game registers and gets a code; the friend's game
joins with the code; the server then forwards their packets both ways. It never looks inside the
game traffic, keeps nothing on disk, and only ever sends to addresses that contacted it first.

    python3 relay.py [--bind 0.0.0.0] [--port 47100]

Needs Python 3.8+ and nothing else. Open UDP port 47100 (or --port) to the internet.

Wire format (big-endian): b"HCR1", type (1 byte), body. See core/src/RelaySocket.cpp.
"""

import argparse
import asyncio
import logging
import random
import secrets
import struct
import time

MAGIC = b"HCR1"
PROTOCOL = 1

# Message types
HOST_REGISTER, HOST_OK, JOIN, JOIN_OK, DATA, ERROR, PING, PONG, PEER_LEFT = range(1, 10)

# Error codes sent to clients
ERR_NO_ROOM, ERR_FULL, ERR_RATE, ERR_CLOSED, ERR_SERVER_FULL, ERR_BAD, ERR_VERSION = range(1, 8)

CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"  # no 0/O or 1/I
CODE_LENGTH = 6

# Requests are padded to at least this size and replies are smaller, so the server can't be
# used to amplify traffic towards a spoofed address.
MIN_REQUEST = 64
MAX_PACKET = 1400

PEER_TIMEOUT = 20.0       # seconds without any packet before a host or guest is dropped
MAX_ROOMS = 5000
MAX_GUESTS = 3            # the game only plays two, but extra guests get a proper "full" reply from it
ROOM_BYTES_PER_SEC = 512 * 1024
REGISTERS_PER_MIN = 10    # per source IP
JOINS_PER_MIN = 30        # per source IP

log = logging.getLogger("relay")

# For testing only (--lag, --jitter, --loss): forwarded game packets are held back and dropped like
# on a long internet route. Off by default.
NET_LAG = 0.0
NET_JITTER = 0.0
NET_LOSS = 0.0


class Bucket:
    """Token bucket: `rate` tokens per `per` seconds, bursting up to `rate`."""

    __slots__ = ("rate", "per", "tokens", "last")

    def __init__(self, rate, per):
        self.rate, self.per, self.tokens, self.last = rate, per, float(rate), time.monotonic()

    def take(self, n=1.0):
        now = time.monotonic()
        self.tokens = min(self.rate, self.tokens + (now - self.last) * self.rate / self.per)
        self.last = now
        if self.tokens < n:
            return False
        self.tokens -= n
        return True


class Room:
    __slots__ = ("code", "host", "host_seen", "guests", "next_guest", "budget")

    def __init__(self, code, host):
        self.code = code
        self.host = host
        self.host_seen = time.monotonic()
        self.guests = {}        # addr -> [guest id, last seen]
        self.next_guest = 1
        self.budget = Bucket(ROOM_BYTES_PER_SEC, 1.0)


class Relay(asyncio.DatagramProtocol):
    def __init__(self):
        self.transport = None
        self.rooms = {}         # code -> Room
        self.by_addr = {}       # addr -> Room (as host or guest)
        self.limits = {}        # (ip, kind) -> Bucket
        self.stats = {"rooms": 0, "joins": 0, "bytes": 0, "dropped": 0}

    # --- plumbing -------------------------------------------------------------------------

    def connection_made(self, transport):
        self.transport = transport

    def error_received(self, exc):
        # ICMP "port unreachable" from a client that went away (Windows reports it on the socket).
        pass

    def send(self, addr, kind, body=b""):
        self.transport.sendto(MAGIC + bytes([kind]) + body, addr)

    def forward(self, addr, body):
        if NET_LOSS and random.random() < NET_LOSS:
            return
        delay = NET_LAG + (random.uniform(0.0, NET_JITTER) if NET_JITTER else 0.0)
        if delay <= 0.0:
            self.send(addr, DATA, body)
        else:
            asyncio.get_running_loop().call_later(delay, self.send, addr, DATA, body)

    def error(self, addr, code, text):
        self.send(addr, ERROR, bytes([code]) + text.encode()[:100])

    def allowed(self, ip, kind, rate):
        bucket = self.limits.get((ip, kind))
        if bucket is None:
            if len(self.limits) > 100000:
                self.limits.clear()
            bucket = self.limits[(ip, kind)] = Bucket(rate, 60.0)
        return bucket.take()

    def new_code(self):
        for _ in range(100):
            code = "".join(secrets.choice(CODE_ALPHABET) for _ in range(CODE_LENGTH))
            if code not in self.rooms:
                return code
        return None

    def forget(self, addr):
        """Removes addr from whatever room it is in, closing the room if it was the host."""
        room = self.by_addr.pop(addr, None)
        if room is None:
            return
        if room.host == addr:
            for guest in list(room.guests):
                self.by_addr.pop(guest, None)
                self.error(guest, ERR_CLOSED, "The host closed the session.")
            self.rooms.pop(room.code, None)
        elif addr in room.guests:
            guest_id = room.guests.pop(addr)[0]
            self.send(room.host, PEER_LEFT, struct.pack(">I", guest_id))

    # --- packets --------------------------------------------------------------------------

    def datagram_received(self, data, addr):
        if len(data) < 5 or len(data) > MAX_PACKET or data[:4] != MAGIC:
            return

        kind, body = data[4], data[5:]

        if kind == DATA:
            self.on_data(addr, body)
        elif kind == PING:
            room = self.by_addr.get(addr)
            if room is not None:
                self.touch(room, addr)
                self.send(addr, PONG)
        elif kind == HOST_REGISTER:
            self.on_register(addr, data, body)
        elif kind == JOIN:
            self.on_join(addr, data, body)

    def touch(self, room, addr):
        if room.host == addr:
            room.host_seen = time.monotonic()
        elif addr in room.guests:
            room.guests[addr][1] = time.monotonic()

    def on_register(self, addr, data, body):
        if len(data) < MIN_REQUEST or len(body) < 5:
            return

        version, nonce = body[0], body[1:5]

        if version != PROTOCOL:
            self.error(addr, ERR_VERSION, "This co-op server speaks a different version. Update the mod.")
            return

        room = self.by_addr.get(addr)

        if room is not None and room.host == addr:
            room.host_seen = time.monotonic()      # a retry: same code again
            self.send(addr, HOST_OK, nonce + room.code.encode())
            return

        if not self.allowed(addr[0], "register", REGISTERS_PER_MIN):
            self.error(addr, ERR_RATE, "Too many sessions started from your address. Wait a minute.")
            return

        if len(self.rooms) >= MAX_ROOMS:
            self.error(addr, ERR_SERVER_FULL, "The co-op server is full right now. Try again later.")
            return

        code = self.new_code()
        if code is None:
            self.error(addr, ERR_SERVER_FULL, "The co-op server is full right now. Try again later.")
            return

        self.forget(addr)
        room = Room(code, addr)
        self.rooms[code] = room
        self.by_addr[addr] = room
        self.stats["rooms"] += 1
        self.send(addr, HOST_OK, nonce + code.encode())

    def on_join(self, addr, data, body):
        if len(data) < MIN_REQUEST or len(body) < 1 + CODE_LENGTH:
            return

        version = body[0]
        code = body[1:1 + CODE_LENGTH].decode("ascii", "replace").upper()

        if version != PROTOCOL:
            self.error(addr, ERR_VERSION, "This co-op server speaks a different version. Update the mod.")
            return

        current = self.by_addr.get(addr)
        if current is not None and current.code == code and addr in current.guests:
            self.touch(current, addr)
            self.send(addr, JOIN_OK)            # a retry
            return

        if not self.allowed(addr[0], "join", JOINS_PER_MIN):
            self.error(addr, ERR_RATE, "Too many join attempts. Wait a minute.")
            return

        room = self.rooms.get(code)
        if room is None:
            self.error(addr, ERR_NO_ROOM, "No session with that code. Check it with the host.")
            return

        if room.host == addr:
            self.error(addr, ERR_BAD, "You can't join your own session.")
            return

        if len(room.guests) >= MAX_GUESTS:
            self.error(addr, ERR_FULL, "That session is full.")
            return

        self.forget(addr)
        room.guests[addr] = [room.next_guest, time.monotonic()]
        room.next_guest += 1
        self.by_addr[addr] = room
        self.stats["joins"] += 1
        self.send(addr, JOIN_OK)

    def on_data(self, addr, body):
        room = self.by_addr.get(addr)

        if room is None or len(body) < 4:
            return

        if not room.budget.take(len(body)):
            self.stats["dropped"] += 1
            return

        peer, payload = struct.unpack(">I", body[:4])[0], body[4:]

        if room.host == addr:
            room.host_seen = time.monotonic()
            for guest, (guest_id, _) in room.guests.items():
                if guest_id == peer:
                    self.forward(guest, struct.pack(">I", 0) + payload)
                    self.stats["bytes"] += len(payload)
                    return
        elif addr in room.guests:
            entry = room.guests[addr]
            entry[1] = time.monotonic()
            self.forward(room.host, struct.pack(">I", entry[0]) + payload)
            self.stats["bytes"] += len(payload)

    # --- housekeeping ---------------------------------------------------------------------

    def sweep(self):
        now = time.monotonic()
        for room in list(self.rooms.values()):
            if now - room.host_seen > PEER_TIMEOUT:
                self.forget(room.host)
                continue
            for guest, (_, seen) in list(room.guests.items()):
                if now - seen > PEER_TIMEOUT:
                    self.forget(guest)


async def main():
    parser = argparse.ArgumentParser(description="HITMAN Co-op relay server")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=47100)
    parser.add_argument("--stats-every", type=float, default=300.0, help="seconds between stats log lines")
    parser.add_argument("--lag", type=float, default=0.0, help=argparse.SUPPRESS)     # testing: ms each way
    parser.add_argument("--jitter", type=float, default=0.0, help=argparse.SUPPRESS)  # testing: extra 0..ms
    parser.add_argument("--loss", type=float, default=0.0, help=argparse.SUPPRESS)    # testing: percent dropped
    args = parser.parse_args()
    global NET_LAG, NET_JITTER, NET_LOSS
    NET_LAG, NET_JITTER, NET_LOSS = args.lag / 1000.0, args.jitter / 1000.0, args.loss / 100.0

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    loop = asyncio.get_running_loop()
    transport, relay = await loop.create_datagram_endpoint(Relay, local_addr=(args.bind, args.port))
    log.info("HITMAN Co-op relay server (created by Jordan Rodrigues) listening on udp %s:%d", args.bind, args.port)

    next_stats = time.monotonic() + args.stats_every
    try:
        while True:
            await asyncio.sleep(2.0)
            relay.sweep()
            if time.monotonic() >= next_stats:
                next_stats += args.stats_every
                s = relay.stats
                log.info("open rooms %d | since start: rooms %d, joins %d, relayed %.1f MB, dropped %d",
                         len(relay.rooms), s["rooms"], s["joins"], s["bytes"] / 1e6, s["dropped"])
    finally:
        transport.close()


if __name__ == "__main__":
    import sys
    # The default Proactor loop adds milliseconds of latency to every UDP packet on Windows.
    loop_factory = asyncio.SelectorEventLoop if sys.platform == "win32" else None
    try:
        if sys.version_info >= (3, 12):
            asyncio.run(main(), loop_factory=loop_factory)
        else:
            if loop_factory:
                asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
            asyncio.run(main())
    except KeyboardInterrupt:
        pass
