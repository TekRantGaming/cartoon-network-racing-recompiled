# Bring-up findings (USA SLUS_214.38, EU SLES_543.06)

Reverse-engineering notes and the evidence behind each runtime change. Addresses are USA unless stated.

## Status (2026-10-29)

**Playable (USA).** Cold boot → intro movies → memory card check → title → profile/name entry → Select Show → Driver
Select → races (tested several minutes: AI opponents, positions, laps, pickups, HUD). Rendering through PCSX2's hardware
GS (D3D11, 3x), audio through PCSX2's SPU2 (WASAPI), pad via SDL3 (XInput controller detected). Races run at full
speed: 60 game frames/s, one vblank per frame (see "Frame pacing").

**Playable (EU)** as well: `build/cnr_eu/cnr.exe --eu` runs SLES_543.06 as PAL (640x512, 50 Hz vblank, PAL CRTC
fallbacks), through the menus into races at 50 game frames/s. The SDK library code sits at the same addresses in both
builds, so both overrides apply unchanged; everything else is a separate recompile (`config/eu/`).

Not done / not verified: memory card save/load round trip; audio has been measured (levels per scene) but
not listened to.

## IOP side (IOPMAIN.IRX, the game's own module)

* Polls `cdvdman` export 9 (`sceCdGetToc`) at start-up → implemented with PCSX2's single-layer DVD TOC.
* Reports "IOP ready" with SIF_CMD_SET_SREG (cid 0x80000001) sreg 31 = 0x7F914 (its EE→IOP request buffer). The EE
  polls `sceSifGetSreg(31)` in `FUN_00226180`. The runtime now keeps sregs for that system command, and the game's
  `sceSifGetSreg` (0x42F030, missed by the signature DB) is bound to the runtime handler (`src/cnr_overrides.cpp`).
* The EE also sets an IOP sreg with SET_SREG; IOPMAIN polls IOP-side `sceSifGetSreg` (sifcmd #6) until it changes.
  The IOP emulator now implements sifcmd #6/#7 and the SET_SREG system command.
* Request protocol (both directions): user SIF command cid 0, header word 3 = command code, a 256-byte argument block
  as SIF "extra" data. Each side dispatches `table[code](extraDest)`:
  * EE → IOP code 0x2C (IOP handler 0x61C34, table at module +0x29F5C, BSS). Sub-command in block word 0:
    1 open (by name at +0x20, or by sector: +0x1C index, +0x20 byte offset, +0x24 size), 2 read, 4 ?, 8 lookup answer,
    0xB register ARCHIVE.AR, 0xD ?.
  * IOP → EE codes: 0x0A completions (EE `FUN_00231d70`, types 5/6), 0x0C file lookup request (EE `FUN_0021ff70`,
    answers with 0x2C sub 8 via `isceSifSendCmd`), 0x0E debug text, 0x0D ?.
* Runtime fixes this needed: EE `sceSifSendCmd` now delivers the packet to the IOP handler (it only copied the extra
  data, into the wrong memory); arguments 5-6 (dest, size) come from `$t0/$t1` (EE ABI), not the stack; the IOP side
  records `sceSifAddCmdHandler` / `sceSifSetCmdBuffer`.
* `isceSifSendCmd` (0x42F6B8) bound to the runtime handler too (`src/cnr_overrides.cpp`); unbound, the EE's answer to
  the IOP's ARCHIVE.AR lookup never arrived and the IOP file worker blocked on semaphore 172.

## Movies (.PSS)

* The player calls `sceMpegGetPictureRAW8(mp, rp, mbcount)` (not the RGB32 `sceMpegGetPicture`), then libmpeg's own CSC
  routine (0x3C3540: DMAC ch3 handler 0x3C3BD8, IPU_CMD CSC, IPU_TO via 0x3C2700).
* Runtime: `sceMpegGetPictureRAW8` shares `sceMpegGetPicture`'s pacing/callbacks and writes FFmpeg's YUV420P planes as
  384-byte macroblocks, column-major (the order the RGB32 path uses).
* The IPU is PCSX2's (`ext/kzipu`, `src/cnr_ipu.cpp`). libipu (`sceIpuInit/Sync/StopDMA/RestartDMA`) stays recompiled
  (`config/usa/cnr_usa.toml` `[cnr] keep_recompiled`), since the runtime's HLE versions return without touching the IPU.
* The EE scheduler polls the host IPU every 64 checkpoints: libmpeg's CSC waits on a counter its DMA handler updates
  without touching IPU registers, and kzipu only advances on register access.

## Recompiler inputs

* Ghidra misses frameless leaf functions placed right after another function (e.g. 0x3849E8, called from 0x383FA8;
  the boot stopped at the memory card screen). `scripts/find_code_pointers.py` now adds every `jal` target (25 found).

## Debug switches added

* `PS2X_IOP_IMPORT_TRACE=<n>`: log the first n IOP import calls (library:ordinal, caller, args).
* `PS2X_IOP_SYNC_TRACE=2` now also logs EE→IOP commands with their argument blocks and IOP→EE command codes.
* `CNR_IPU_STATS=1`: per-second IPU command and DMA counts.
* `CNR_AUTOPRESS="<s>:<button>[+...][/<hold>],..."`: scripted pad input for automated runs.
* `CNR_GS_FORMATS=1`: texture upload / TEX0 format histogram every 5 s.
* `CNR_AUDIO_WAV=<file>`: record the SPU2 output.

## Textures (the big one)

* All textures go over PATH3 (GIF DMA ch2) as PSMCT32 uploads and are sampled as PSMT8/PSMT4 with CT32 CLUTs
  (`CNR_GS_FORMATS=1` histogram). The game kicks a frame's texture chain while VIF1 holds PATH3 masked, then opens a
  short MSKPATH3 window before each draw batch, streaming textures through a small VRAM area.
* The runtime released *every* queued PATH3 packet at the first unmask, so all batches drew with the last texture
  (garbled menus; movies showed only their first 128-px slice). Now each unmask releases up to the end of the next
  complete GIF packet (EOP), as the GIF does (`PS2Memory::flushMaskedPath3Packets(..., onePacket)`).
  `PS2X_PATH3_ALL_AT_ONCE=1` restores the old behaviour.

## Recompiler MMI fixes

* `PROT3W` rotated all four words; the EE rotates the low three (`rd = {w1, w2, w0, w3}`); 74 uses in this game.
* `PINTH`/`PINTEH` macros interleaved the wrong halfwords; the `PEXEW` macro disagreed with the translator; halfword
  shifts (`PSLLH/PSRLH/PSRAH`) now mask the count to 4 bits. (Not the texture bug, but real.)

## Movies, part 2

* RAW8 macroblocks are column-major with a 448-line column: the player converts width*height/256 macroblocks and
  draws 448-line columns, so shorter streams (the 640x360 logo) are presented as 448-line frames with black bars
  (`raw8OutputHeight`).

## Heap

* The game claims all remaining heap at start-up (`FUN_0022a860` probes malloc from 128 MB down) and manages it itself.
  The runtime's own guest buffers (SIF packets, MPEG callbacks) and interrupt stacks used to come from the same region
  and corrupted it (a race crashed after ~8 s when an allocation returned 0). They now live at 0x80000-0x100000
  (`ps2SetRuntimeArena` in `src/cnr_main.cpp`), and `SetupHeap`/`EndOfHeap` keep reporting the game's own heap.

## More missed functions

* Functions that start with nops (float code with hazard padding), reached only through vtables (0x408A70):
  `find_code_pointers.py` used to treat a zero first word as padding.

All runtime/recompiler changes are in `patches/ps2recomp/0001-cnr-runtime.patch` (apply on drakolordx7/PS2Recomp
`killzone` @ d042263).

## Frame pacing

`CNR_FRAME_STATS=1` wraps `sceGsSyncV` (USA 0x3B9DF8, EU 0x3B9DE0) and prints game frames/s and vblanks per frame.
Races first measured 36-46 frames/s: vblanks 15.3 ms apart with periodic 30 ms gaps, the EE idle in between. The
scheduler's idle wait (`EeScheduler::waitForEvent`) overslept by a whole 15.6 ms Windows timer tick although
`timeBeginPeriod(1)` was set: Windows 11 ignores timer-resolution requests from a process whose window is not visible
(the hidden `CNR_HEADLESS` window here; equally a minimised or covered game window). `cnr_main.cpp` opts out with
`SetProcessInformation(ProcessPowerThrottling, PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION)`. After that: USA
60.0 frames/s (300/300 frames take one vblank), EU 50.0.

Two scheduler safeguards from the investigation stay in (no measurable effect once the timer was fixed): the idle wait
wakes for the earliest event by host time (`PS2X_IDLE_WAKE_BY_CYCLE=1` reverts), and a vblank's cycle deadline is
clamped to the cycle the wall-clock floor reaches at its host deadline (`PS2X_VBLANK_CYCLE_CLAMP=0` reverts).

## Widescreen (16:9 Hor+)

On by default; `CNR_WIDESCREEN=0` restores 4:3. Starting point: the PCSX2 patch database's widescreen patch for this game
(PCSX2/pcsx2_patches `SLUS-21438_EAE72CFE.pnach` / `SLES-54306_CB4B48AF.pnach`, by Arapapa), which patches the camera
setup FUN_0033ecf8 (EU FUN_0033ec58). Patching code bytes does nothing in a recompile, so the same place was worked out
instead:

* FUN_0033ecf8 builds the projection from fov and a constant table K (USA 0x4C4C58, EU 0x4C4C68; values 1, 1, 4/3):
  x scale = K[1] / tan(fov/2) (0x33F0B4..0x33F0E4), y scale from K[0], K[2] and the viewport size. Only this function
  reads K[1]. `src/cnr_overrides.cpp` sets K[1] = 0.75 at load: horizontal view x4/3, vertical unchanged (true Hor+).
  The function runs every frame in movies, menus and races alike, so it cannot tell races from menus.
* Presentation (`src/cnr_gs.cpp`): a frame is "wide" when it drew 3D through PATH1 (VU1). Wide frames are presented at
  16:9 (kzgs wide ratio), all others (movies, title, loading screens, 2D-only menus) at 4:3 pillarboxed.
* 2D: the game draws its overlay through PATH2 after the 3D, one GIF packet per element or glyph, in 640-pixel screen
  coordinates (XYOFFSET 1728). In wide frames each such packet's X coordinates are narrowed by 3/4: elements whose centre
  is in the outer quarter of the screen around that edge (HUD to the corners; `CNR_HUD_CENTRE=1`: everything around
  the centre), the rest around the centre. Packets at least 600 px wide (screen fills, fades, frame copies) are left
  alone, and vertices on the screen's left/right edge stay there, so the menu transition's iris mask still covers
  the whole screen. PATH2 before the 3D (frame-start copies) is not touched.
* Known limitation: 2D markers drawn over world positions (the "REVENGE" tag above an opponent) move a little off
  their target, like any HUD element of their quarter.
* `CNR_GS_DUMP=<seconds>` writes one frame's GIF packets to `build/runs/gsdump.bin`; `scripts/gsdump.py` lists the
  draws (path, primitive, texture, vertex count, screen box).
