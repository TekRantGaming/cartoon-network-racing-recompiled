"""Summarise a GIF frame dump (CNR_GS_DUMP, build/runs/gsdump.bin): one line per run of draws with the same path,
primitive type and texture, with vertex count and screen-space bounding box (pixels, XYOFFSET removed).

usage: python -I scripts/gsdump.py [build/runs/gsdump.bin]
"""
import struct
import sys

PRIMS = ["POINT", "LINE", "LSTRIP", "TRI", "TSTRIP", "TFAN", "SPRITE", "?"]


def main():
    data = open(sys.argv[1] if len(sys.argv) > 1 else "build/runs/gsdump.bin", "rb").read()
    st = {"prim": 0, "ofx": [0, 0], "ofy": [0, 0], "tex0": [0, 0], "ctxt": 0}
    runs = []
    cur = None

    def vertex(path, x, y):
        nonlocal cur
        c = (st["prim"] >> 9) & 1
        px = x / 16.0 - st["ofx"][c] / 16.0
        py = y / 16.0 - st["ofy"][c] / 16.0
        key = (path, st["prim"] & 7, (st["prim"] >> 4) & 1, st["tex0"][c] & 0x3FFF, (st["prim"] >> 8) & 1)
        if cur is None or cur["key"] != key:
            cur = {"key": key, "n": 0, "x0": 1e9, "x1": -1e9, "y0": 1e9, "y1": -1e9}
            runs.append(cur)
        cur["n"] += 1
        cur["x0"] = min(cur["x0"], px); cur["x1"] = max(cur["x1"], px)
        cur["y0"] = min(cur["y0"], py); cur["y1"] = max(cur["y1"], py)

    def ad(path, addr, val):
        if addr == 0x00:
            st["prim"] = val & 0x7FF
        elif addr in (0x18, 0x19):
            st["ofx"][addr - 0x18] = val & 0xFFFF
            st["ofy"][addr - 0x18] = (val >> 32) & 0xFFFF
        elif addr in (0x06, 0x07):
            st["tex0"][addr - 0x06] = val
        elif addr in (0x05, 0x0D, 0x04, 0x0C):
            vertex(path, val & 0xFFFF, (val >> 16) & 0xFFFF)

    off = 0
    while off + 8 <= len(data):
        path, size = struct.unpack_from("<II", data, off)
        off += 8
        pkt = data[off:off + size]
        off += size
        p = 0
        while p + 16 <= len(pkt):
            lo, hi = struct.unpack_from("<QQ", pkt, p)
            p += 16
            nloop = lo & 0x7FFF
            if (lo >> 46) & 1:
                st["prim"] = (lo >> 47) & 0x7FF
            flg = (lo >> 58) & 3
            nreg = (lo >> 60) & 0xF or 16
            regs = [(hi >> (4 * r)) & 0xF for r in range(nreg)]
            if flg == 0:
                for _ in range(nloop):
                    for r in regs:
                        if p + 16 > len(pkt):
                            break
                        a, b = struct.unpack_from("<QQ", pkt, p)
                        p += 16
                        if r == 0xE:
                            ad(path, b & 0xFF, a)
                        elif r == 0x0:
                            st["prim"] = a & 0x7FF
                        elif r in (0x4, 0x5):  # XYZF2 / XYZ2 packed
                            vertex(path, a & 0xFFFF, (a >> 32) & 0xFFFF)
                        elif r in (0xC, 0xD):
                            vertex(path, a & 0xFFFF, (a >> 32) & 0xFFFF)
            elif flg == 1:
                words = []
                n = nloop * nreg
                for i in range(n):
                    words.append(struct.unpack_from("<Q", pkt, p + 8 * i)[0])
                p += ((n + 1) // 2) * 16
                for i, w in enumerate(words):
                    r = regs[i % nreg]
                    if r == 0x0:
                        st["prim"] = w & 0x7FF
                    elif r in (0x4, 0x5, 0xC, 0xD):
                        vertex(path, w & 0xFFFF, (w >> 16) & 0xFFFF)
                    elif r == 0x6:
                        st["tex0"][0] = w
            else:
                p += nloop * 16
    for r in runs:
        path, prim, tme, tbp, ctxt = r["key"]
        print(f"path{path} {PRIMS[prim]:6} tme{tme} ctx{ctxt} tbp{tbp:5} n{r['n']:5}  x {r['x0']:7.1f}..{r['x1']:7.1f}  y {r['y0']:7.1f}..{r['y1']:7.1f}")


if __name__ == "__main__":
    main()
