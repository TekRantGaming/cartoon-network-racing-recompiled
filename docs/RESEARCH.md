# Cartoon Network Racing — PC Recompilation Research & Plan

Status: research complete, toolchain bring-up in progress. Last updated 2026-10-28.

## 1. Goal and scope

Produce a native Windows executable that plays Cartoon Network Racing (PS2) from the
user's own disc image, for both the USA (SLUS-21438) and EU (SLES-54306) releases.

Out of scope for now: launcher, settings UI, resolution/graphics options, control remapping,
user-facing game-file import. During development the ISOs are read from `game files/`.

## 2. The disc

| | USA | EU |
|---|---|---|
| Main ELF | `SLUS_214.38` (4,874,112 B) | `SLES_543.06` (4,874,240 B) |
| SYSTEM.CNF | `VER=1.01`, `VMODE=NTSC` | `VER=1.00`, `VMODE=PAL` |
| Build date | 2006-11-07 | 2006-10-28 |

Shared layout on both discs:

* `MODULES/` — Sony IOP modules (`LIBSD`, `SDRDRV`, `MCMAN`, `MCSERV`, `PADMAN`, `SIO2MAN`),
  replacement IOP kernel image `IOPRP310.IMG`, and the game's own IOP module `IOPMAIN.IRX`
  (CD streaming + SPU2 sound streaming: `SOUND_PlayStream`, `StartStream`, …).
  All IOP modules are byte-identical between regions.
* `ARCHIVE.AR` (~654 MB) + `CDFILES.DAT` (file table, `filP` magic) — the game's packed
  file system ("GFS" in engine strings).
* `*.PSS` — MPEG-2 FMVs (logos + 12 `NUGxx.PSS` cartoon clips), decoded on the EE via the IPU.

## 3. The executable

* ELF32 MIPS R5900, entry `0x100008`, **stripped** (no symbol table).
* `.text` 3.3 MB (~846k instructions) + `.textWriteDMA` 21 KB + `.vutext` 52 KB VU microcode,
  plus 22 `.DVP.overlay` VU1 microprogram overlays.
* C++ with RTTI (class names such as `RaceCarEngine`, `GSJetEngineEffect` survive) and
  exception tables — useful for naming functions via vtables.
* Sony SDK 3.1.0 libraries: libcdvd, libgraph, libipu, libkernl, libmc, libpad, libscf, libsdr.

### USA vs EU

`.text` is the same size at the same base address, but the two are separate compiles: function
start addresses drift by up to ±216 bytes across the binary, and data sections shift by
0x40–0x80 bytes. Consequences:

* Each region gets its own function map, TOML config and generated C++.
* Game-specific runtime overrides must not hard-code one region's addresses. They live in a
  shared table with a per-region address column (or are located by byte signature at load time).

## 4. Approaches considered

| Approach | Verdict |
|---|---|
| Ship an emulator (PCSX2 core) | Not a recompilation; GPL-3 constraints; rejected. |
| Full decompilation to matching C++ | Years of work for a 3.3 MB C++ binary; rejected for now. |
| **Static recompilation (MIPS → C++) + HLE runtime** | **Chosen.** Proven route (N64Recomp, XenonRecomp); several PS2 titles have reached playable in 2026 with it. |

## 5. Chosen toolchain

* **PS2Recomp** (github.com/ran-j/PS2Recomp, active, updated 2026-10-22) as the base:
  * `ps2xAnalyzer` — ELF analysis, SCE SDK signature database to name stripped SDK functions.
  * `ps2xRecomp` — R5900 → C++ translator (MMI and VU0 macro mode supported).
  * `ps2xRuntime` — guest memory, kernel syscalls, threads/scheduler, SIF/RPC, CD, memory card,
    pad, MPEG/IPU, GS (software backend), VIF1 and VU1 interpreter, raylib host window/input/audio.
  * `ps2xIOP` — runs the original IRX modules (R3000A) under a virtual IOP kernel with HLE fallbacks.
    This matters here because `IOPMAIN.IRX` is a custom module we would otherwise have to rewrite.
* **Ghidra 11.x + a PS2 loader extension** — for function boundaries; PS2Recomp ships
  `ExportPS2Functions.java`, which writes the CSV map + TOML the recompiler prefers.
* **MSVC 2022 Build Tools** (installed, v14.44) as the primary compiler, since PS2Recomp is
  tested mainly with MSVC. clang 22 (llvm-mingw) is also available.
* PCSX2 (not shipped, development use only) as a behaviour reference when debugging.

## 6. Project layout

```
game files/          user ISOs (git-ignored)
extracted/           files pulled off the ISOs for analysis (git-ignored)
generated/<region>/  recompiler output (git-ignored: it is the game's code)
config/<region>/     PS2Recomp TOML + Ghidra function map per region
game/                our game-specific runtime code: overrides, region address tables
scripts/             dev helper scripts (extraction, comparison)
tools/src/           third-party sources (PS2Recomp, wiki) (git-ignored)
docs/                this document and bring-up notes
```

## 7. Bring-up plan (USA first, then EU)

1. Build `ps2_analyzer`, `ps2_recomp`, `ps2EntryRunner`.
2. Ghidra: import `SLUS_214.38`, auto-analyse, run `ExportPS2Functions.java` → CSV + TOML.
3. Run the analyzer's SDK signature pass to name SDK functions; merge into the TOML stubs.
4. Recompile → `generated/usa`, build the runner, boot.
5. Boot debug loop (per the PS2Recomp walkthrough): unresolved functions → syscalls → SIF/IOP
   module loading → CD reads of `CDFILES.DAT`/`ARCHIVE.AR` → first GS frame.
6. Intro FMVs (IPU/MPEG path), front-end menu, input.
7. First race: VU1 microprograms (`.DVP.overlay`s) through the VU1 interpreter, GS rendering.
8. Audio: SPU2 via `IOPMAIN.IRX` streaming + `SDRDRV`/`LIBSD`.
9. Memory card saves.
10. Repeat 2–9 for `SLES_543.06`; factor shared overrides; PAL 50 Hz timing.

"Playable" = cold boot → menus → complete races in all modes with audio and saves, on both regions.

## 8. Known risks

* **VU1 / GS performance.** PS2Recomp's own README calls these "very bad". A racing game is
  VU1-heavy. Likely work: optimise or replace the software GS backend with a GPU renderer.
* **Custom IOP module.** `IOPMAIN.IRX` handles streaming; if the IOP emulator path misbehaves,
  we write an HLE replacement for its RPC interface.
* **Timing-sensitive code.** VSync/interrupt handlers and DMA completion waits are common hang points.
* **Stripped binary.** Function boundaries depend on Ghidra; bad splits show up as crashes in
  generated code and need manual entries in the function map.

## 9. Sources

* PS2Recomp — https://github.com/ran-j/PS2Recomp and its wiki
  (in particular "PS2Recomp Stripped Game Walkthrough").
* Community ports on PS2Recomp (God of War, Killzone, Urban Reign, Dragon Quest VIII, …) —
  reference for project structure and common runtime fixes.
* ps2-web-recomp (snuri00) — another EE → C++ recompiler with a WebGPU runtime; secondary reference.

## 10. Update 2026-10-28: base switched to the KillzoneRecomp stack

Upstream PS2Recomp's software GS and VU1 interpreter are too slow for a 3D racer. The most advanced
PS2Recomp port, KillzoneRecomp (github.com/drakolordx7/KillzoneRecomp, GPL-3.0), shows the working route:

* `ext/PS2Recomp` - drakolordx7's fork (`killzone` branch, d042263): upstream plus ~30 runtime/recompiler
  fixes needed to reach gameplay (FPU/MMI semantics, LQ/SQ alignment, VIF1 UNPACK, DMA chains, IOP
  semaphores/heap, SPU2 host audio, FMV audio, IOP thread, performance).
* `ext/pcsx2` - PCSX2 source pinned at 646df006a4 (verified by KillzoneRecomp), plus PCSX2's Windows
  dependency bundle in `ext/kzgs/deps`.
* `ext/kzgs` (PCSX2 hardware GS renderer: D3D11/D3D12/Vulkan), `ext/kzvu` (microVU),
  `ext/kzipu` (IPU, for the PSS movies), `ext/kzspu2` (SPU2 audio) - vendored from KillzoneRecomp.
  They are not Killzone-specific; the `kz` prefix is kept for now to make upstream syncing easy.

Licence consequence: the project is GPL-3.0 (PS2Recomp and PCSX2 both are).

Ghidra headless note: `analyzeHeadless.bat` breaks on the space in the project path, so
`scripts/ghidra_headless.sh` calls Ghidra's Java entry point directly.

USA Ghidra export: 14,978 functions + 50,906 code labels; kernel syscall wrappers at 0x42C360-0x42CBD0.
