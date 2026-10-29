"""Find the vtable of a class from its RTTI name (GCC 3 ABI: vtable[-1] = type_info, type_info+4 = name):
python -I scripts/vtable.py <usa|eu> <mangled class name, e.g. 18GSRaceIntroState_c>"""
import struct
import sys

ELF = {"usa": "extracted/usa/SLUS_214.38", "eu": "extracted/eu/SLES_543.06"}


def main():
    d = open(ELF[sys.argv[1]], "rb").read()
    phoff, = struct.unpack_from("<I", d, 0x1C)
    phnum, = struct.unpack_from("<H", d, 0x2C)
    segs = [struct.unpack_from("<8I", d, phoff + p * 32) for p in range(phnum)]

    def va(fo):
        for t, off, v, _, fs, _, _, _ in segs:
            if t == 1 and off <= fo < off + fs:
                return v + fo - off

    def words():
        for t, off, v, _, fs, _, _, _ in segs:
            if t == 1:
                for i in range(0, fs - 3, 4):
                    yield v + i, struct.unpack_from("<I", d, off + i)[0]

    def rd(a):
        for t, off, v, _, fs, _, _, _ in segs:
            if t == 1 and v <= a < v + fs:
                return struct.unpack_from("<I", d, off + a - v)[0]

    name = sys.argv[2].encode() + b"\0"
    fo = d.find(name)
    while fo >= 0 and d[fo - 1] != 0:
        fo = d.find(name, fo + 1)
    nva = va(fo)
    print("name", hex(nva))
    tis = [a - 4 for a, w in words() if w == nva]
    for ti in tis:
        print("type_info", hex(ti))
        for a, w in words():
            if w == ti:
                vt = a + 4
                print("  vtable", hex(vt), [hex(rd(vt + 4 * k)) for k in range(12)])


if __name__ == "__main__":
    main()
