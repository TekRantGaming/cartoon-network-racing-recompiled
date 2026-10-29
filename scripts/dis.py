"""Disassemble guest code from the region ELF: python -I scripts/dis.py <usa|eu> <hex addr> [count] [--func]
--func starts at the containing function (Ghidra CSV) instead of the address."""
import bisect, csv, struct, sys, tomllib
from pathlib import Path
import capstone

ROOT = Path(__file__).resolve().parents[1]
region, addr = sys.argv[1], int(sys.argv[2], 16)
count = int(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else 40
cfg = tomllib.loads((ROOT / "config" / region / f"cnr_{region}.toml").read_text())["general"]
d = (ROOT / cfg["input"]).read_bytes()
rows = sorted((int(r["Start"], 16), int(r["End"], 16), r["Name"]) for r in csv.DictReader((ROOT / cfg["ghidra_output"]).open())
              if not r["Name"].startswith("entry_"))
i = bisect.bisect_right([r[0] for r in rows], addr) - 1
if i >= 0:
    print(f"; in {rows[i][2]} 0x{rows[i][0]:08X}-0x{rows[i][1]:08X}")
    if "--func" in sys.argv:
        addr = rows[i][0]
        count = max(count, (rows[i][1] - rows[i][0]) // 4)
phoff, = struct.unpack_from("<I", d, 0x1C)
phnum, = struct.unpack_from("<H", d, 0x2C)
for p in range(phnum):
    _, off, vaddr, _, filesz, _, _, _ = struct.unpack_from("<8I", d, phoff + p * 32)
    if vaddr <= addr < vaddr + filesz:
        fo = off + addr - vaddr
        break
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 + capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata = True
for ins in md.disasm(d[fo:fo + count * 4], addr):
    print(f"  {ins.address:08x}  {ins.mnemonic:8} {ins.op_str}")
