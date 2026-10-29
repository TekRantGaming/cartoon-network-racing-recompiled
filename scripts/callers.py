"""List call sites (jal) of a guest function in the region ELF, with the containing function from the Ghidra CSV.
Usage: python -I scripts/callers.py <usa|eu> <hex address> [...]"""
import bisect, csv, struct, sys, tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
region = sys.argv[1]
cfg = tomllib.loads((ROOT / "config" / region / f"cnr_{region}.toml").read_text())["general"]
d = (ROOT / cfg["input"]).read_bytes()
shoff, = struct.unpack_from("<I", d, 0x20)
text_addr, text_off, text_size = struct.unpack_from("<III", d, shoff + 40 + 12)  # section 1 = .text
rows = sorted((int(r["Start"], 16), r["Name"]) for r in csv.DictReader((ROOT / cfg["ghidra_output"]).open()) if not r["Name"].startswith("entry_"))
starts = [r[0] for r in rows]
words = struct.unpack_from(f"<{text_size // 4}I", d, text_off)
for t in (int(a, 16) for a in sys.argv[2:]):
    enc = 0x0C000000 | ((t >> 2) & 0x03FFFFFF)
    sites = [text_addr + i * 4 for i, w in enumerate(words) if w == enc]
    print(f"0x{t:08X}: {len(sites)} call sites")
    for s in sites:
        i = bisect.bisect_right(starts, s) - 1
        print(f"  0x{s:08X} in {rows[i][1]} (0x{rows[i][0]:08X})")
