#!/usr/bin/env python3
"""PNG previews of the screen from what the host test recorded (frames.json): every line and filled polygon the
screen drew, in order.
    python3 render_frames.py tyr8.svg frames.json OUTDIR
(tyr8.svg is accepted for the old command line; it is empty since 0.9.6.) A plain software rasteriser (standard
library only): good for checking layout and motion, not the OP-1's pixels."""
import json, struct, sys, zlib

W, H, Z = 320, 160, 3                       # Z: preview zoom


def line(img, x0, y0, x1, y1, w, rgb):
    x0, y0, x1, y1, r = x0 * Z, y0 * Z, x1 * Z, y1 * Z, max(w * Z / 2, 0.6)
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    for py in range(max(0, int(min(y0, y1) - r - 1)), min(H * Z, int(max(y0, y1) + r + 2))):
        for px in range(max(0, int(min(x0, x1) - r - 1)), min(W * Z, int(max(x0, x1) + r + 2))):
            t = 0 if L2 == 0 else max(0, min(1, ((px - x0) * dx + (py - y0) * dy) / L2))
            ex, ey = px - (x0 + t * dx), py - (y0 + t * dy)
            if ex * ex + ey * ey <= r * r:
                img[py][px] = rgb


def poly(img, pts, rgb):
    pts = [(x * Z, y * Z) for x, y in pts]
    ys = [p[1] for p in pts]
    for py in range(max(0, int(min(ys))), min(H * Z, int(max(ys)) + 1)):
        yc = py + 0.5
        xs = []
        for i in range(len(pts)):
            (ax, ay), (bx, by) = pts[i - 1], pts[i]
            if (ay <= yc) != (by <= yc):
                xs.append(ax + (yc - ay) * (bx - ax) / (by - ay))
        xs.sort()
        for a, b in zip(xs[0::2], xs[1::2]):
            for px in range(max(0, int(a + 0.5)), min(W * Z, int(b + 0.5))):
                img[py][px] = rgb


def png(path, img):
    raw = b''.join(b'\x00' + bytes(c for px in row for c in px) for row in img)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W * Z, H * Z, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    frames = json.load(open(sys.argv[2]))
    out, seen, n = sys.argv[3], set(), 0
    for fr in frames:
        if fr['label'] in seen:
            continue
        seen.add(fr['label'])
        img = [[(0, 0, 0)] * (W * Z) for _ in range(H * Z)]
        for op in fr['ops']:
            if op[0] == 'L':
                _, x0, y0, x1, y1, w, r, g, b = op
                line(img, x0 / 1024, y0 / 1024, x1 / 1024, y1 / 1024, w / 1024, (r, g, b))
            else:
                _, p, r, g, b = op
                poly(img, [(p[i] / 1024, p[i + 1] / 1024) for i in range(0, len(p), 2)], (r, g, b))
        path = f'{out}/screen_{n:02d}.png'
        png(path, img)
        print(path, '-', fr['label'])
        n += 1


main()
