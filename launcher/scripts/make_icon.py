#!/usr/bin/env python3
# Regenerate the placeholder icon: python3 scripts/make_icon.py src-tauri/icons/app-icon.png
# then: npx tauri icon src-tauri/icons/app-icon.png
# An original placeholder (no game art, no logo): a pale green crescent moon
# with two small stars on a dark night-blue-green rounded square. Standard
# library only.
import math, struct, sys, zlib

N = 1024
C = N / 2

def rr(px, py, r=200, m=40):
    x0, y0, x1, y1 = m, m, N - m, N - m
    cx = min(max(px, x0 + r), x1 - r)
    cy = min(max(py, y0 + r), y1 - r)
    return (px - cx) ** 2 + (py - cy) ** 2 <= r * r and x0 <= px <= x1 and y0 <= py <= y1

def reticle(px, py):
    """1 where the mark is drawn: a crescent (a disc minus an offset disc)
    and two small stars (dots) in the crescent's opening."""
    if math.hypot(px - 470, py - 530) <= 300 and math.hypot(px - 590, py - 430) > 270:
        return 1
    if math.hypot(px - 690, py - 330) <= 26:
        return 1
    if math.hypot(px - 770, py - 470) <= 16:
        return 1
    return 0

S = [(0.25, 0.25), (0.75, 0.25), (0.25, 0.75), (0.75, 0.75)]
rows = []
for y in range(N):
    row = bytearray([0])
    for x in range(N):
        a = b = 0
        for sx, sy in S:
            px, py = x + sx, y + sy
            if rr(px, py):
                a += 1
                b += reticle(px, py)
        if a == 0:
            row += b"\0\0\0\0"
            continue
        t = y / N
        bg = (int(22 - 12 * t), int(36 - 20 * t), int(40 - 22 * t))     # night blue-green, darker below
        # a faint vertical light stripe through the middle
        glow = max(0.0, 1 - abs(x - C) / 420) * 10
        bg = tuple(min(255, int(c + glow)) for c in bg)
        fg = (108, 198, 168)                                            # styles.css --accent
        f = b / a
        col = [int(bg[i] * (1 - f) + fg[i] * f) for i in range(3)]
        row += bytes(col + [int(255 * a / 4)])
    rows.append(bytes(row))

def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
open(sys.argv[1], "wb").write(png)
