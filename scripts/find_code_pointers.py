"""Find function starts Ghidra missed, for regen.py to pass to ps2_recomp as entry_points.

Three sources:
  1. data pointers: aligned words in .data/.rodata/.sdata pointing into .text at a non-function address
     (vtables, callback tables, jump tables);
  2. prologues after a return: `addiu sp,sp,-N` that follows `jr ra` + delay slot (+ nop / `addiu v0,v0,0` padding).
     Ghidra sometimes extends a tiny function over the next one (e.g. entry_0043ffd0 swallowing the static
     constructor at 0x43FFE8, reached only via a runtime-built list), so these starts never appear as pointers.
  3. direct call targets: every `jal` into .text. Cartoon Network Racing has frameless leaf functions (no prologue)
     that Ghidra merged into the previous function, e.g. 0x3849E8 called from 0x383FA8.
Writes build/work/<region>/code_pointers.txt (one 0x... per line).

Usage: called by scripts/regen.py (paths come from CNR_* env vars)
"""
import csv, os, struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Adapted from KillzoneRecomp (GPL-3.0). CNR_ELF / CNR_FUNCS_CSV / CNR_CODE_POINTERS / CNR_CODE_POINTERS_REJECT pick the region.
ELF = Path(os.environ["CNR_ELF"])
CSV = Path(os.environ["CNR_FUNCS_CSV"])
OUT = Path(os.environ["CNR_CODE_POINTERS"])
# Candidates whose recompiled body hit undecodable instructions (VU microcode / tables embedded in .text);
# written by regen.py after each recompile and excluded here.
REJECT = Path(os.environ["CNR_CODE_POINTERS_REJECT"])


def sections(d):
    shoff, = struct.unpack_from("<I", d, 0x20)
    shnum, shstrndx = struct.unpack_from("<HH", d, 0x30)
    sh = [struct.unpack_from("<10I", d, shoff + i * 40) for i in range(shnum)]
    stro = sh[shstrndx][4]
    for s in sh:
        name = d[stro + s[0]:d.index(b"\0", stro + s[0])].decode()
        yield name, s[1], s[3], s[4], s[5]  # name, type, addr, offset, size


def main():
    d = ELF.read_bytes()
    secs = {n: (t, a, o, sz) for n, t, a, o, sz in sections(d)}
    _, tstart, toff, tsize = secs[".text"]
    tend = tstart + tsize
    starts = set()
    with CSV.open() as f:
        for row in csv.DictReader(f):
            starts.add(int(row["Start"], 16))
    found = {}
    for name in (".data", ".rodata", ".sdata"):
        stype, addr, off, size = secs[name]
        for i in range(0, size - 3, 4):
            v, = struct.unpack_from("<I", d, off + i)
            if tstart <= v < tend and v % 4 == 0 and v not in starts:
                # A target that starts with nops is still code when real instructions follow (e.g. the float method
                # at 0x408A70, reached only through a vtable, opens with two nops); pure padding runs longer.
                head = struct.unpack_from("<4I", d, toff + (v - tstart)) if v + 16 <= tend else (0,)
                if not any(head):
                    continue  # padding, not code
                found.setdefault(v, name)
    JR_RA = 0x03E00008
    PADDING = {0x00000000, 0x24420000}
    words = struct.unpack_from(f"<{tsize // 4}I", d, toff)
    for i, w in enumerate(words):
        if (w >> 16) != 0x27BD or not (w & 0x8000):
            continue  # not addiu sp,sp,-N
        a = tstart + i * 4
        if a in starts:
            continue
        j = i - 1
        while j >= 0 and words[j] in PADDING:
            j -= 1
        # words[j] is the delay slot (non-padding) or jr ra itself when the delay slot is a nop
        if (j >= 0 and words[j] == JR_RA) or (j >= 1 and words[j - 1] == JR_RA):
            found.setdefault(a, "prologue")
    for i, w in enumerate(words):
        if (w >> 26) != 3:
            continue  # not jal
        t = ((tstart + i * 4 + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
        if tstart <= t < tend and t not in starts and words[(t - tstart) // 4] != 0:
            found.setdefault(t, "jal")
    rejected = set()
    if REJECT.exists():
        rejected = {int(l.split()[0], 16) for l in REJECT.read_text().splitlines() if l.strip() and not l.startswith("#")}
    for v in rejected:
        found.pop(v, None)
    by = {}
    for v, n in found.items():
        by[n] = by.get(n, 0) + 1
    OUT.write_text("".join(f"0x{v:08X}\n" for v in sorted(found)))
    print(f"[code-pointers] {len(found)} targets not in function map {by} ({len(rejected)} rejected as data) -> {OUT}")


if __name__ == "__main__":
    main()
