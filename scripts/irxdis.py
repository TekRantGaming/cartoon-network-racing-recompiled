"""Disassemble an IRX module as loaded at a base address:
python -I scripts/irxdis.py <irx> <load base hex> <iop address hex> [count]"""
import struct, sys
import capstone

d = open(sys.argv[1], "rb").read()
base, addr = int(sys.argv[2], 16), int(sys.argv[3], 16)
count = int(sys.argv[4]) if len(sys.argv) > 4 else 60
phoff, = struct.unpack_from("<I", d, 0x1C)
phnum, = struct.unpack_from("<H", d, 0x2C)
for p in range(phnum):
    ptype, off, vaddr, _, filesz, _, _, _ = struct.unpack_from("<8I", d, phoff + p * 32)
    if ptype == 1 and vaddr <= addr - base < vaddr + filesz:  # PT_LOAD
        fo = off + (addr - base - vaddr)
        break
else:
    sys.exit("address not in a loaded segment")
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata = True
for ins in md.disasm(d[fo:fo + count * 4], addr):
    print(f"  {ins.address:08x}  {ins.mnemonic:8} {ins.op_str}")
