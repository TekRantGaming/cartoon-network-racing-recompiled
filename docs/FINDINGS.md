# Bring-up findings (USA SLUS_214.38, EU SLES_543.06)

Reverse-engineering notes and the evidence behind each runtime change. Addresses are USA unless stated.

## Status (2026-10-30)

**Playable, USA and EU.** Cold boot -> movies -> memory card check -> title -> profile (new or loaded) -> menus ->
races, both regions. Rendering through PCSX2's hardware GS (D3D11), audio through PCSX2's SPU2 (WASAPI), SDL3 pad.

* Speed: 60 game frames/s (EU 50), one vblank per frame, in menus and races ("Frame pacing").
* 16:9 Hor+ by default, HUD in proportion at the screen edges; 2D-only screens pillarboxed 4:3 ("Widescreen").
* Saves: `saves/<USA|EU>/mc0` next to cnr.exe, new profile and reload verified ("Memory card saves").
* Fixed this phase: intermittent hang on file opens, sound banks/streams loaded with the wrong size (SPU errors), the
  garbage first output line/column (1-pixel presentation crop).
* Glitch sweep: attract-mode demos on six tracks (USA + EU), menus, profile flow, a full lap on Rocky Mountain Trail:
  no rendering faults seen.
* Everything is set in the TRG Launcher 2.0 (`launcher/project.cpp`, "The launcher" below).
* Frame rates above 60 (`CNR_FPS`): races start, run at full speed and no longer crash at 90, 120, 144 and 165 (USA) and
  60 and 120 (EU); 240 stays on the first Loading screen (see "Frame rates above 60"). World-anchored 2D markers
  sit slightly off in 16:9; a "WRONG WAY" sign was once captured without its text (not reproduced, may be its blink).

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

## Intermittent hang on a file open (fixed)

About one run in ten (more often at higher vblank rates) the game froze on a loading or menu transition: the EE main
thread waited forever in FUN_00232420 (`WaitSema` at 0x2324A4) for IOPMAIN's acknowledgement of an EE->IOP file
request (code 0x2C), with every IOP thread idle. IOP->EE completions (code 0x0A, handled by FUN_00231d70, which only
signals when the reply names the pending request at 0x4C1508) carry their data as SIF "extra" data in a ring of 256-byte
buffers in EE memory (0x5ED270...). The IOP emulator wrote that data into EE memory as soon as the IOP sent it, while
the packet itself waits for the EE thread (threaded IOP) and the EE handler runs later still, as a queued invocation.
IOPMAIN sends ~80 debug-text commands (0x0E) a second (the sound errors below), so the ring wrapped and overwrote a
pending acknowledgement before its handler ran. Now the extra data travels with the command
(`IopHost::sendSifCommandWithExtra`) and is written by the invocation's new `onStart` hook, right before its handler
runs. 10 of 10 runs at 120 Hz without a hang (before: hangs within two). `PS2X_SIF_LOG=1` logs every user SIF command
in both directions.

## Frame rates above 60 (experimental, CNR_FPS)

`CNR_FPS=<hz>` raises the guest vblank rate. The frame limiter FUN_0032e3e0 measures each frame in vblanks (counter
0x4C40D8) and multiplies by seconds-per-vblank (0x59B974, set with 1/refresh by the video mode setup FUN_0033c810, which
also stores the refresh rate at 0x59C220/0x59C214 and the frame time at 0x59BAD8). The main loop (around 0x10D250) then
runs fixed simulation steps: FUN_003831f8 accumulates the frame time and returns floor(acc / step), at least 1 and at
most 2; each step is seconds-per-vblank long (also as whole milliseconds at 0x4C566C, FUN_003831c0), and the physics
world takes that step at race load (0x177ED4).

* Without changes, a higher vblank rate runs the game faster (one 1/60 step per frame): 90 Hz measured 1.45x.
* With the rate globals set to the new rate (what `CNR_FPS` does, re-applied every frame from the `sceGsSyncV` hook),
  menus, loading and the race fly-by keep real-time speed.
* Race countdown (fixed): the race intro state (vtable 0x4EF4B0, update FUN_001cc3f8, state at +0x254) starts the
  countdown (virtual slot 46, FUN_0015e6c8) once FUN_00300260 reports every HUD widget's intro animation finished. The
  minimap's fade-in never finished: the HUD script loader FUN_00305340 stores a fade as a whole alpha step per simulation
  step, (int)(alphaMax / (duration / step)) at widget +0x36B, which is 1 at 60 Hz and truncates to 0 at 90/120 Hz, so the
  fade (FUN_00303848, EU 0x303780; alpha +0x368 towards +0x369) never moved. With `CNR_FPS` a zero step on an active fade
  becomes 1 (`fadeHook`): the minimap fades in up to 2x faster, the countdown starts.
* Speed (fixed): FUN_003831f8 (EU 0x383188) caps a frame at 2 simulation steps; frames longer than that lose the excess.
  At 60 Hz that is 2/60 s (the game clock 0x4C5658 still loses ~1.7% to occasional 3+-vblank hitches, as the original
  would); at 120 Hz the same cap lost ~4%. With `CNR_FPS` the function is replaced (`stepCount`) with the cap scaled to
  keep 2/60 s: game clock 10.009 s per 10 s at 120 Hz.
* Crash (fixed): races at 90/120 Hz crashed intermittently (about 1 race in 3 at 120 Hz) in the effect-node clone
  FUN_0033ffa0 (call to 0 at 0x340098): the node copy constructor's allocation (0x341A98, via the game allocator
  0x229E80) returned 0. The game heap was nearly full at every rate: 20-55 KB free at race start, and a slow leak of
  cloned "cratesmash" effects (crate smash handler FUN_001ab218 -> FUN_001ac028, ~200 B/s at 60 Hz) used it up; a
  140 KB allocation in the race HUD (caller 0x1613B0, the position ladder with driver portraits) failed even at 60 Hz,
  so that HUD element never appeared. Cause: the runtime's memory layout. The game's crt0 calls SetupThread(stack -1,
  size 0x80000) and SetupHeap(0x640644, -1); the PS2 kernel puts that stack at [32 MB - 512 KB, 32 MB) and the heap runs
  up to it (0x1F80000). The runtime put the stack pointer at 0x1F80000 (the stack below it) and ended "rest of RAM" heaps
  at 0x1F00000, where its own HLE pools live (RPC packets/servers, TLS, boot-mode table, 0x31000 bytes): the game lost
  512 KB. `ps2SetRuntimePools` now moves those pools (CNR: 0x20000, in the unused EE kernel area, `CNR_POOLS=0` keeps the
  old layout), and with them moved SetupThread/SetupHeap use the PS2 layout. Races now have ~300-350 KB free, the HUD
  position ladder appears, and 120 Hz races ran 6 of 6 crash-free (USA), plus 400 s runs at 90 Hz USA, 120 Hz EU, 60 Hz.
* Seen twice in ~50 boots (USA once, EU once, before and after these changes): the boot stays on its first "Loading"
  screen with the game otherwise running. Two instances run side by side also froze after the first logo movie twice.
  Not investigated yet.
* Debug: `CNR_HEAP_LOG=1` (USA) logs heap use/free once a second and every failed allocation with its caller;
  `CNR_HEAP_TRACK=1` adds live allocations by caller (leak hunting).
* Races through the launcher's game folder, 1080p, 2026-10-30: USA 144 and 165 held their rate in the race. EU 50 (two
  runs) and 60 reached a race; one earlier EU 60 run stayed on the pre-race Loading screen for two minutes, then stopped
  on the frame a `CNR_SHOT_DIR` screenshot was taken. Not reproduced.
* 240 Hz (USA): three of three runs stayed on the first Loading screen until the run ended, at a steady 240 frames/s, so
  the launcher does not offer it. Not investigated (the EE cycle scale is 0.25 there). "No limit" is not offered either: it
  would need a rate above that.
* Debug aids from this: `CNR_RAMDUMP=<s>,<s>` (EE RAM to build/runs/ram_<s>.bin), scripts/xref.py (code that builds an
  address), scripts/vtable.py.
* The runtime's automatic EE cycle scale (60 / vblank rate) is needed: with `PS2X_EE_CYCLE_SCALE=1` frames span
  several vblanks at 90 Hz.

## The launcher

`launcher/project.cpp` describes the game to the TRG Launcher 2.0: two games (USA, Europe), each with its own folder,
`games/<id>`, its own program and its own `<id>.toml`. The launcher itself needed PlayStation 2 discs and sections written
for a game that is not on ReXGlue (`patches/trg-launcher/0001-ps2-games.patch`).

* Install: the disc's serial comes from SYSTEM.CNF. Only about 5 MB of the disc is read as files (the ELF, CDFILES.DAT,
  MODULES/, SYSTEM.CNF); ARCHIVE.AR and the movies are read by sector. So the launcher keeps the image as
  `game/disc.iso` (a hard link on the same drive) and copies files up to 16 MB beside it.
* The launcher passes `--game_data_root=<dir>` and `--cnr_from_launcher=true`. `src/cnr_settings.cpp` reads
  `<exe dir>/<exe name>.toml`; the launcher writes every default before the first Play, so the game sees what it shows.
  Without the file the game uses its defaults and the `CNR_*` switches, which still win.
* Resolution: the window (or a borderless fullscreen window over the screen) and the GS upscale, which is the smallest
  multiple of 640 x 448 that covers the size both ways: 3x at 1080p, 4x at 1440p, 6x at 4K.
* The games are window programs (`/SUBSYSTEM:WINDOWS`), so no console opens; from the launcher their output goes to
  `logs/game.log` and `logs/game-errors.log`.
* Vibration: `scePadSetActAlign` and `scePadSetActDirect` are hooked and passed to `SDL_RumbleGamepad`. Not yet felt on a
  real controller.

## Memory card saves

The runtime's memory card emulation stores each card as a host folder (mc0 = slot 1, mc1 = slot 2). `cnr_main.cpp` puts
them next to the game: `<folder of cnr.exe>/saves/USA/mc0|mc1` and `saves/EU/...` (`CNR_SAVE_DIR=<dir>` overrides the
`saves` folder). Earlier builds saved into `extracted/<region>/mc0`; those saves are copied over once when the new folder
does not exist yet. Verified: new profile (name entry, autosave notice, "saving data") writes
`BASLUS-21438PPLAYER/{BASLUS-21438PPLAYER,icon.sys,iconlst.icn}`, and a later run loads it ("load successful").

## Sound banks loaded with the wrong file's size (fixed)

IOPMAIN printed `SOUND_CheckSPUMemory ... > SPU RAM size`, `StartStream ... SPU Memory Error`, `playing error` on every
boot. The SIF trace (`PS2X_SIF_LOG=1`) showed each EE answer to the IOP's file lookups (IOP->EE 0x0C with the name,
EE->IOP 0x2C sub-command 8 with sector and size) was right, but every `StartStream` used the previous lookup's size:
FrontEndSounds.snd started with musicarc.snd's 11.5 MB (into SPU RAM: the error), materials.snd with FrontEndSounds',
and so on; the first stream got an uninitialised 653 MB.

IOPMAIN's lookup (0x515AC) creates semaphore 172 (count 0, max 1), sends 0x0C and `WaitSema(172)`s; the IOP handler
for the answer signals it. The lookup runs inside the sound RPC (sid 0x12345), and the IOP emulator runs RPC server
functions synchronously, outside any IOP thread, where `WaitSema` (`waitSemaphoreOutsideThread`) runs the IOP's
threads until the semaphore is signalled, giving up after 1 s of IOP time. Only the EE could signal it, and with all
IOP threads idle the loop skipped that second of IOP time at once: `WaitSema(172) outside a thread ... TIMED OUT`. The
semaphore was not taken, the answer arriving ~36 ms later left it at 1, and from then on every lookup's wait returned at
once, with the previous answer.

Fix (ps2xIOP): while the IOP is idle in that wait, idle steps are kept short (0.5 ms of IOP time) and the IOP worker
thread releases the IOP lock for 0.2 ms (`IopEmulator::setExternalWait`) so the EE can deliver its answer; queued RPCs
are not started meanwhile. Gives up after 2 s wall time. Result: no timeouts, no SPU/stream errors; music streams with
per-track seeks into musicarc.snd; audio output measured in menus and races.
