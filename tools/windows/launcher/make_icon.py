#!/usr/bin/env python3
"""Generate totk.ico (stylized triforce on a dark rounded square), PNG-compressed sizes 16-256. Standard library only."""
import struct, sys, zlib
from pathlib import Path

def tri(p, a, b, c):
    def s(p1, p2, p3): return (p1[0]-p3[0])*(p2[1]-p3[1]) - (p2[0]-p3[0])*(p1[1]-p3[1])
    d1, d2, d3 = s(p, a, b), s(p, b, c), s(p, c, a)
    return not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0))

def pixel(x, y):   # x,y in 0..1
    cx, cy = x-.5, y-.5
    r = .16
    ax, ay = abs(cx)-(.5-r), abs(cy)-(.5-r)
    if max(ax, 0)**2 + max(ay, 0)**2 > r*r: return (0, 0, 0, 0)
    bg = (int(18+30*y), int(30+40*y), int(48+40*y), 255)
    h, w = .26, .30   # three triangles
    top = ((.5, .16), (.5-w/2, .16+h), (.5+w/2, .16+h))
    bl = ((.5-w/2, .16+h), (.5-w, .16+2*h), (.5, .16+2*h))
    br = ((.5+w/2, .16+h), (.5, .16+2*h), (.5+w, .16+2*h))
    # a gap between pieces comes from shrinking each toward its centroid
    for t in (top, bl, br):
        gx, gy = sum(v[0] for v in t)/3, sum(v[1] for v in t)/3
        k = .88
        s = tuple((gx+(v[0]-gx)*k, gy+(v[1]-gy)*k) for v in t)
        if tri((x, y), *s):
            g = int(200+55*(1-y))
            return (g, int(g*.82), 40, 255)
    return bg

def png(size):
    ss = 3 if size < 64 else 2
    rows = []
    for j in range(size):
        row = bytearray([0])
        for i in range(size):
            acc = [0, 0, 0, 0]
            for sj in range(ss):
                for si in range(ss):
                    p = pixel((i+(si+.5)/ss)/size, (j+(sj+.5)/ss)/size)
                    for k in range(3): acc[k] += p[k]*p[3]
                    acc[3] += p[3]
            n = ss*ss
            a = acc[3]
            row += bytes([acc[k]//a if a else 0 for k in range(3)] + [a//n])
        rows.append(bytes(row))
    def chunk(t, d): c = struct.pack('>I', len(d))+t+d; return c+struct.pack('>I', zlib.crc32(t+d))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b''))

def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).with_name('totk.ico')
    sizes = (16, 24, 32, 48, 64, 128, 256)
    imgs = [png(s) for s in sizes]
    data = struct.pack('<HHH', 0, 1, len(sizes))
    off = 6+16*len(sizes)
    for s, im in zip(sizes, imgs):
        data += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(im), off)
        off += len(im)
    out.write_bytes(data + b''.join(imgs))
    print('wrote', out)

if __name__ == '__main__':
    main()
