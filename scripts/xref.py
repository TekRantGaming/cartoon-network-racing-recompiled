"""Find code that builds a data address with lui/addiu (or lui/ori, or a lui-relative load/store):
python -I scripts/xref.py <usa|eu> <address hex> [...]"""
import struct
import sys

ELF = {"usa": "extracted/usa/SLUS_214.38", "eu": "extracted/eu/SLES_543.06"}


def main():
    d = open(ELF[sys.argv[1]], "rb").read()
    targets = {int(a, 16) for a in sys.argv[2:]}
    phoff, = struct.unpack_from("<I", d, 0x1C)
    phnum, = struct.unpack_from("<H", d, 0x2C)
    for p in range(phnum):
        ptype, off, vaddr, _, filesz, _, flags, _ = struct.unpack_from("<8I", d, phoff + p * 32)
        if ptype != 1 or not (flags & 1):
            continue
        words = struct.unpack_from(f"<{filesz // 4}I", d, off)
        his = {}  # reg -> (hi value, index)
        for i, w in enumerate(words):
            op = w >> 26
            rs, rt = (w >> 21) & 31, (w >> 16) & 31
            imm = w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 0x0F:  # lui
                his[rt] = (imm << 16, i)
                continue
            if rs in his and i - his[rs][1] < 24:
                base = his[rs][0]
                if op in (0x09, 0x0D) or op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x28, 0x29, 0x2B, 0x31, 0x37, 0x39, 0x3F, 0x1E, 0x1F):
                    addr = (base | imm) if op == 0x0D else (base + simm) & 0xFFFFFFFF
                    if addr in targets:
                        print(f"{vaddr + i * 4:08x}: ref {addr:08x} (op {op:#x})")


if __name__ == "__main__":
    main()
