#!/usr/bin/env python3
"""Writes assets/icon.ico, the placeholder application icon of VetusPoeta.exe, with the standard
library only (no Pillow, no browser). Original artwork: a cream leaf with a vein on a terracotta
rounded square, the UI's own leaf symbol (gui/ui/index.html, #vp-i-leaf) and its colour tokens
(--accent #B3452A, --bg #FBF7F0). The brand task replaces it with tools/make_icons.py output.

    python3 gui/shell/tools/make_placeholder_icon.py [--out assets/icon.ico]

Layers 16, 24, 32, 48, 64, 128, 256 px, PNG-compressed (Windows Vista and later). The output is
deterministic: shapes are filled by an exact scanline rasteriser with 4x4 supersampling.
"""
import argparse
import os
import struct
import zlib

SIZES = [16, 24, 32, 48, 64, 128, 256]
SS = 4  # supersampling per axis
ACCENT = (0xB3, 0x45, 0x2A)
CREAM = (0xFB, 0xF7, 0xF0)


def cubic(p0, p1, p2, p3, n=48):
    pts = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        x = u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0]
        y = u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]
        pts.append((x, y))
    return pts


def rounded_square(x0, y0, x1, y1, r, n=16):
    import math
    pts = []
    corners = [(x1 - r, y0 + r, -90), (x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180)]
    for cx, cy, start in corners:
        for i in range(n + 1):
            a = math.radians(start + 90 * i / n)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def leaf():
    # The UI's 16x16 leaf: M2.5 13.5 C2.5 7.5 7 3 13.5 2.5 C13 9 8.5 13.5 2.5 13.5 Z, scaled into the
    # rounded square with a margin.
    pts = [(2.5, 13.5)]
    pts += cubic((2.5, 13.5), (2.5, 7.5), (7, 3), (13.5, 2.5))
    pts += cubic((13.5, 2.5), (13, 9), (8.5, 13.5), (2.5, 13.5))
    return pts


def vein(width):
    # A straight vein from the stem towards the tip, as a thin quadrilateral.
    (ax, ay), (bx, by) = (3.4, 12.6), (10.6, 5.4)
    dx, dy = bx - ax, by - ay
    length = (dx * dx + dy * dy) ** 0.5
    nx, ny = -dy / length * width / 2, dx / length * width / 2
    return [(ax + nx, ay + ny), (bx + nx, by + ny), (bx - nx, by - ny), (ax - nx, ay - ny)]


def coverage(poly, size, scale, offset):
    """Fraction of each pixel inside `poly` (16-unit design space mapped by scale/offset)."""
    grid = size * SS
    pts = [((x * scale + offset) * SS, (y * scale + offset) * SS) for x, y in poly]
    cov = [0] * (size * size)
    edges = list(zip(pts, pts[1:] + pts[:1]))
    for row in range(grid):
        y = row + 0.5
        xs = []
        for (x0, y0), (x1, y1) in edges:
            if (y0 <= y < y1) or (y1 <= y < y0):
                xs.append(x0 + (y - y0) * (x1 - x0) / (y1 - y0))
        xs.sort()
        prow = (row // SS) * size
        for a, b in zip(xs[0::2], xs[1::2]):
            c0 = max(0, int(a + 0.5))
            c1 = min(grid, int(b + 0.5))
            for col in range(c0, c1):
                cov[prow + col // SS] += 1
    full = SS * SS
    return [c / full for c in cov]


def render(size):
    # The design space is 16 units; small sizes get a slightly fuller square.
    margin = 0.5 if size <= 24 else 1.0
    scale = size / 16.0
    sq = coverage(rounded_square(margin, margin, 16 - margin, 16 - margin, 3.2), size, scale, 0)
    leaf_scale = scale * 0.78
    leaf_off = size * 0.11
    lf = coverage(leaf(), size, leaf_scale, leaf_off)
    vn = coverage(vein(0.9 if size <= 32 else 0.6), size, leaf_scale, leaf_off)
    rgba = bytearray()
    for i in range(size * size):
        a_sq = sq[i]
        if a_sq <= 0:
            rgba += b'\x00\x00\x00\x00'
            continue
        # Composite inside the square: accent, then cream leaf, then accent vein on the leaf.
        r, g, b = ACCENT
        t = lf[i]
        r, g, b = (r + (CREAM[0] - r) * t, g + (CREAM[1] - g) * t, b + (CREAM[2] - b) * t)
        v = vn[i] * t
        r, g, b = (r + (ACCENT[0] - r) * v, g + (ACCENT[1] - g) * v, b + (ACCENT[2] - b) * v)
        rgba += bytes((int(r + 0.5), int(g + 0.5), int(b + 0.5), int(a_sq * 255 + 0.5)))
    return bytes(rgba)


def png(size, rgba):
    raw = b''.join(b'\x00' + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def ico(images):
    head = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    dirs, blobs = b'', b''
    for size, data in images:
        dim = 0 if size >= 256 else size
        dirs += struct.pack('<BBBBHHII', dim, dim, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    return head + dirs + blobs


def main():
    repo = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', default=os.path.join(repo, 'assets', 'icon.ico'))
    args = ap.parse_args()
    images = [(s, png(s, render(s))) for s in SIZES]
    data = ico(images)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'wb') as f:
        f.write(data)
    print('wrote %s (%d bytes, sizes %s)' % (args.out, len(data), ', '.join(str(s) for s in SIZES)))


if __name__ == '__main__':
    main()
