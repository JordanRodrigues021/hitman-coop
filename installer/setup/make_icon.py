"""Writes the setup's icon (setup.ico): a red crosshair on a dark rounded tile.

Pure Python, no imaging libraries: draws 16/32/48/256 px images and stores them as 32-bit
BMP entries in one .ico. Run: python make_icon.py setup.ico
"""
import math
import struct
import sys


def pixel(x, y, n):
    """RGBA for pixel (x, y) of an n x n icon."""
    c = (n - 1) / 2.0
    dx, dy = x - c, y - c
    r = math.hypot(dx, dy) / (n / 2.0)

    # Rounded dark tile
    corner = n * 0.18
    qx, qy = max(abs(dx) - (c - corner), 0), max(abs(dy) - (c - corner), 0)
    if math.hypot(qx, qy) > corner:
        return (0, 0, 0, 0)

    red = (219, 20, 36, 255)
    gold = (255, 199, 64, 255)
    base = (int(18 + 14 * (1 - r)), 14, 16, 255)

    width = max(n * 0.07, 1.2)
    ring = abs(r - 0.62) * (n / 2.0) < width * 0.6
    cross = (abs(dx) < width / 2 or abs(dy) < width / 2) and 0.18 < r < 0.86
    dot = r < 0.1

    if dot:
        return gold
    if ring or cross:
        return red
    return base


def bmp_entry(n):
    rows = []
    for y in range(n - 1, -1, -1):  # BMP rows go bottom-up
        row = b""
        for x in range(n):
            r, g, b, a = pixel(x, y, n)
            row += bytes((b, g, r, a))
        rows.append(row)
    xor = b"".join(rows)
    mask_row = ((n + 31) // 32) * 4
    and_mask = b"\x00" * (mask_row * n)
    header = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, len(xor) + len(and_mask), 0, 0, 0, 0)
    return header + xor + and_mask


def main(out):
    sizes = [16, 32, 48, 256]
    images = [bmp_entry(n) for n in sizes]
    data = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    for n, img in zip(sizes, images):
        data += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    data += b"".join(images)
    open(out, "wb").write(data)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "setup.ico")
