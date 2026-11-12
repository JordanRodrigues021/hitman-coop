"""Read or write the game's encrypted text files (Retail/thumbs.dat, Runtime/packagedefinition.txt).

  python tools/gamecfg.py show <file>
  python tools/gamecfg.py decrypt <file> <out.txt>
  python tools/gamecfg.py encrypt <in.txt> <file>

The format: a 16 byte magic, the CRC32 of the plain text, then the text in XTEA blocks of 8 bytes
(zero padded). The setup (installer/setup/GameFiles.cs) does the same in C#.
"""
import struct, sys, zlib

MAGIC = bytes([0x22, 0x3D, 0x6F, 0x9A, 0xB3, 0xF8, 0xFE, 0xB6, 0x61, 0xD9, 0xCC, 0x1C, 0x62, 0xDE, 0x83, 0x41])
KEY = [0x30F95282, 0x1F48C419, 0x295F8548, 0x2A78366D]
DELTA = 0x61C88647
M = 0xFFFFFFFF


def _dec(v0, v1):
    s = 0xC6EF3720
    for _ in range(32):
        v1 = (v1 - ((((v0 << 4) ^ (v0 >> 5)) + v0) ^ (s + KEY[(s >> 11) & 3]))) & M
        s = (s + DELTA) & M
        v0 = (v0 - ((((v1 << 4) ^ (v1 >> 5)) + v1) ^ (s + KEY[s & 3]))) & M
    return v0, v1


def _enc(v0, v1):
    s = 0
    for _ in range(32):
        v0 = (v0 + ((((v1 << 4) ^ (v1 >> 5)) + v1) ^ (s + KEY[s & 3]))) & M
        s = (s - DELTA) & M
        v1 = (v1 + ((((v0 << 4) ^ (v0 >> 5)) + v0) ^ (s + KEY[(s >> 11) & 3]))) & M
    return v0, v1


def decrypt(data):
    if data[:16] != MAGIC:
        raise ValueError("not an encrypted game text file")
    crc = struct.unpack_from("<I", data, 16)[0]
    body = data[20:]
    out = bytearray()
    for i in range(0, len(body) - 7, 8):
        out += struct.pack("<II", *_dec(*struct.unpack_from("<II", body, i)))
    text = bytes(out).rstrip(b"\0")
    return text, zlib.crc32(text) & M == crc


def encrypt(text):
    padded = text + b"\0" * (-len(text) % 8)
    out = bytearray(MAGIC + struct.pack("<I", zlib.crc32(text) & M))
    for i in range(0, len(padded), 8):
        out += struct.pack("<II", *_enc(*struct.unpack_from("<II", padded, i)))
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    if sys.argv[1] in ("show", "decrypt"):
        text, ok = decrypt(open(sys.argv[2], "rb").read())
        if sys.argv[1] == "show":
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
            print(text.decode("utf-8", "replace"))
            print("# checksum %s, %d bytes" % ("ok" if ok else "WRONG", len(text)), file=sys.stderr)
        else:
            open(sys.argv[3], "wb").write(text)
        return 0 if ok else 1
    if sys.argv[1] == "encrypt":
        open(sys.argv[3], "wb").write(encrypt(open(sys.argv[2], "rb").read()))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
