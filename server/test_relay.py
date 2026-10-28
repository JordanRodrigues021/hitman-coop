"""Tests for relay.py's logic, without sockets. Run: python -I test_relay.py"""
import struct
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import relay  # noqa: E402

M = relay.MAGIC


class FakeTransport:
    def __init__(self):
        self.sent = []  # (addr, kind, body)

    def sendto(self, data, addr):
        assert data[:4] == M
        self.sent.append((addr, data[4], data[5:]))


def register(nonce=1):
    return (M + bytes([relay.HOST_REGISTER, relay.PROTOCOL]) + struct.pack(">I", nonce)).ljust(64, b"\0")


def join(code):
    return (M + bytes([relay.JOIN, relay.PROTOCOL]) + code.encode()).ljust(64, b"\0")


def data(peer, payload):
    return M + bytes([relay.DATA]) + struct.pack(">I", peer) + payload


HOST, GUEST, OTHER = ("1.1.1.1", 1000), ("2.2.2.2", 2000), ("3.3.3.3", 3000)


class RelayTests(unittest.TestCase):
    def setUp(self):
        self.r = relay.Relay()
        self.t = FakeTransport()
        self.r.connection_made(self.t)

    def last(self, addr):
        return [s for s in self.t.sent if s[0] == addr][-1]

    def host_code(self, addr=HOST, nonce=1):
        self.r.datagram_received(register(nonce), addr)
        to, kind, body = self.last(addr)
        self.assertEqual(kind, relay.HOST_OK)
        self.assertEqual(struct.unpack(">I", body[:4])[0], nonce)
        return body[4:10].decode()

    def test_register_gives_code_and_retry_gives_same_code(self):
        code = self.host_code()
        self.assertEqual(len(code), 6)
        self.assertTrue(all(c in relay.CODE_ALPHABET for c in code))
        self.assertEqual(self.host_code(), code)
        self.assertEqual(len(self.r.rooms), 1)

    def test_join_and_forward_both_ways(self):
        code = self.host_code()
        self.r.datagram_received(join(code.lower()), GUEST)
        self.assertEqual(self.last(GUEST)[1], relay.JOIN_OK)

        self.r.datagram_received(data(0, b"hello host"), GUEST)
        to, kind, body = self.last(HOST)
        self.assertEqual(kind, relay.DATA)
        guest_id = struct.unpack(">I", body[:4])[0]
        self.assertEqual(body[4:], b"hello host")

        self.r.datagram_received(data(guest_id, b"hello guest"), HOST)
        to, kind, body = self.last(GUEST)
        self.assertEqual((kind, body), (relay.DATA, struct.pack(">I", 0) + b"hello guest"))

    def test_wrong_code_and_short_requests(self):
        self.host_code()
        self.r.datagram_received(join("ZZZZZZ"), GUEST)
        to, kind, body = self.last(GUEST)
        self.assertEqual((kind, body[0]), (relay.ERROR, relay.ERR_NO_ROOM))

        # Unpadded requests are ignored, so replies can never be bigger than requests.
        before = len(self.t.sent)
        self.r.datagram_received(register()[:20], OTHER)
        self.r.datagram_received(join("ABCDEF")[:12], OTHER)
        self.assertEqual(len(self.t.sent), before)

    def test_replies_never_larger_than_requests(self):
        code = self.host_code()
        self.r.datagram_received(join("ZZZZZZ"), GUEST)
        for addr, kind, body in self.t.sent:
            if kind in (relay.HOST_OK, relay.ERROR, relay.JOIN_OK):
                self.assertLessEqual(5 + len(body), relay.MIN_REQUEST)

    def test_strangers_and_unknown_peers_get_nothing(self):
        code = self.host_code()
        self.r.datagram_received(join(code), GUEST)
        before = len(self.t.sent)
        self.r.datagram_received(data(0, b"x"), OTHER)          # not in any room
        self.r.datagram_received(data(999, b"x"), HOST)         # no such guest
        self.r.datagram_received(M + bytes([relay.PING]), OTHER)
        self.r.datagram_received(b"junk", OTHER)
        self.r.datagram_received(M + b"\x05" + b"x" * 2000, GUEST)  # oversized
        self.assertEqual(len(self.t.sent), before)

    def test_room_full(self):
        code = self.host_code()
        for i in range(relay.MAX_GUESTS):
            self.r.datagram_received(join(code), ("9.9.9.%d" % i, 1))
        self.r.datagram_received(join(code), OTHER)
        to, kind, body = self.last(OTHER)
        self.assertEqual((kind, body[0]), (relay.ERROR, relay.ERR_FULL))

    def test_cant_join_own_room(self):
        code = self.host_code()
        self.r.datagram_received(join(code), HOST)
        to, kind, body = self.last(HOST)
        self.assertEqual((kind, body[0]), (relay.ERROR, relay.ERR_BAD))

    def test_register_rate_limit(self):
        for port in range(relay.REGISTERS_PER_MIN):
            self.host_code(("5.5.5.5", 4000 + port))
        self.r.datagram_received(register(), ("5.5.5.5", 5999))
        to, kind, body = self.last(("5.5.5.5", 5999))
        self.assertEqual((kind, body[0]), (relay.ERROR, relay.ERR_RATE))

    def test_version_mismatch(self):
        packet = (M + bytes([relay.HOST_REGISTER, 99]) + struct.pack(">I", 1)).ljust(64, b"\0")
        self.r.datagram_received(packet, HOST)
        self.assertEqual(self.last(HOST)[2][0], relay.ERR_VERSION)

    def test_expiry_closes_room_and_tells_guest(self):
        code = self.host_code()
        self.r.datagram_received(join(code), GUEST)
        self.r.rooms[code].host_seen -= relay.PEER_TIMEOUT + 1
        self.r.sweep()
        self.assertEqual(self.r.rooms, {})
        self.assertEqual(self.r.by_addr, {})
        to, kind, body = self.last(GUEST)
        self.assertEqual((kind, body[0]), (relay.ERROR, relay.ERR_CLOSED))

    def test_guest_timeout_tells_host(self):
        code = self.host_code()
        self.r.datagram_received(join(code), GUEST)
        self.r.rooms[code].guests[GUEST][1] -= relay.PEER_TIMEOUT + 1
        self.r.sweep()
        self.assertEqual(self.last(HOST)[1], relay.PEER_LEFT)
        self.assertIn(code, self.r.rooms)

    def test_host_registering_again_elsewhere_replaces_nothing_else(self):
        a = self.host_code(HOST)
        b = self.host_code(OTHER)
        self.assertNotEqual(a, b)
        self.r.datagram_received(join(a), GUEST)
        self.r.datagram_received(data(0, b"to a"), GUEST)
        self.assertEqual(self.last(HOST)[2][4:], b"to a")
        self.assertFalse(any(s[0] == OTHER and s[1] == relay.DATA for s in self.t.sent))

    def test_bandwidth_cap(self):
        code = self.host_code()
        self.r.datagram_received(join(code), GUEST)
        chunk = b"x" * 1300
        for _ in range(int(relay.ROOM_BYTES_PER_SEC / 1300) + 50):
            self.r.datagram_received(data(0, chunk), GUEST)
        self.assertGreater(self.r.stats["dropped"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=1)
