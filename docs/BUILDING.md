# Building (development)

Windows x64, Visual Studio 2022 Build Tools (MSVC 14.44), CMake 3.21+, Ninja, Python 3.11+, 7-Zip, Git.
Game files are user supplied; during development they live in `game files/` and `extracted/` (both git-ignored).

## One-time setup

1. Third-party sources (git-ignored, fetched by hand):
   * `ext/PS2Recomp`: `git clone -b killzone --recursive https://github.com/drakolordx7/PS2Recomp.git ext/PS2Recomp`
     (verified at d042263), then `git -C ext/PS2Recomp apply ../../patches/ps2recomp/0001-cnr-runtime.patch`
   * `ext/pcsx2`: `git clone https://github.com/PCSX2/pcsx2.git ext/pcsx2`, then `git -C ext/pcsx2 checkout 646df006a4`
   * `ext/kzgs/deps`: PCSX2's `pcsx2-windows-dependencies.7z` (latest-windows-dependencies release), extracted there
   * `ext/sdl3`: `SDL3-devel-3.4.16-VC.zip`, extracted there
   * `ext/trg-launcher`: `git clone https://github.com/TekRantGaming/trg-launcher.git ext/trg-launcher` (verified at
     8896944), then `git -C ext/trg-launcher apply ../../patches/trg-launcher/0001-ps2-games.patch`
2. Extract the boot files from the disc image: `SLUS_214.38`, `SYSTEM.CNF`, `CDFILES.DAT` and `MODULES/` into
   `extracted/usa/` (7-Zip can open the ISO).
3. Ghidra function map (only needed when the map changes; the result is committed in `config/usa/`):
   Ghidra 12.1.4 + ghidra-emotionengine-reloaded 2.1.38 + JDK 21 in `tools/bin/`, then
   `bash scripts/ghidra_headless.sh ...` with `ExportPS2Functions.java` (see docs/RESEARCH.md).

## Recompiler

```bash
cmake -S ext/PS2Recomp -B build/ps2recomp -G "Visual Studio 17 2022" -A x64 -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF
cmake --build build/ps2recomp --config RelWithDebInfo --target ps2_recomp ps2_analyzer -- -m
```

## Generate C++ from the ELF

```bash
python -I scripts/regen.py usa
python -I scripts/regen.py eu
```

Writes `generated/usa/` and `generated/eu/` (~18,700 files each). Only changed files are rewritten, so rebuilds stay
incremental. The EU boot files go in `extracted/eu/` (same set as USA, with `SLES_543.06`).

## Build the game

```bash
cmd //c "scripts\\vsenv.bat cmake -S . -B build\\cnr -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo"
cmd //c "scripts\\vsenv.bat cmake --build build\\cnr -j 14"
```

The EU executable is a second build of the same project against the EU code:

```bash
cmd //c "scripts\vsenv.bat cmake -S . -B build\cnr_eu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCNR_GENERATED_DIR=generated/eu"
cmd //c "scripts\vsenv.bat cmake --build build\cnr_eu -j 14"
```

## The launcher

Players start the game from the TRG Launcher 2.0. `launcher/project.cpp` holds everything about this game, including
every setting. It is built on its own (it fetches SDL2, Dear ImGui and stb) and staged with both games:

```bash
cmd //c "scripts\vsenv.bat cmake -S launcher -B build\launcher -G Ninja -DCMAKE_BUILD_TYPE=Release"
cmd //c "scripts\vsenv.bat cmake --build build\launcher"
python scripts/stage_launcher.py
```

`dist/CartoonNetworkRacing/cnr_launcher.exe` is the portable result: `games/cnr_usa` and `games/cnr_eu` hold the programs,
and the launcher's install step puts the player's disc in `game/` beside them. Saves stay in each game's `saves/` folder.
Staging again replaces the programs and keeps the disc, settings, saves and shader cache.

## Make a release

Set `VERSION`, build both games, then:

```bash
python scripts/make_release.py
python scripts/make_release.py --earlier 1.0.0
```

The first line is for the first release; later ones name the version before them, so updates can remove what is gone. It
writes `release/v<version>/`: the players' zip (GitHub tag `v<version>`) and `packages/` (tag `packages-v<version>`), which
is what the launcher downloads. Publish the packages release first, so the players' release is the newest on the page.
The repository must be public, as the launcher asks GitHub without signing in. Put every zip through VirusTotal first, and
keep every published zip: the next release needs them. The rules are in `ext/trg-launcher/docs/UPDATER.md`.

## Run

From the project root: `build/cnr/cnr.exe` (defaults to `extracted/usa/SLUS_214.38` and the USA ISO in `game files/`),
or `build/cnr_eu/cnr.exe --eu` for the EU disc (PAL, 50 Hz).
See the header of `src/cnr_main.cpp` for the `CNR_*` development switches (headless runs, screenshots, renderer).

## Automated test run (headless)

```bash
CNR_HEADLESS=1 CNR_SECONDS=240 CNR_SHOT_DIR=build/runs/test CNR_SHOT_INTERVAL=5 CNR_AUTOPRESS="66:start,70:cross,74:cross,78:cross,82:cross,86:cross,90:cross,94:cross,98:cross,102:cross,106:cross,110:cross,114:cross,118:cross,122:cross,126:cross,130:cross,134:cross,138:cross,142:cross,146:cross,150:cross,155:cross/80" build/cnr/cnr.exe
```

Boots, picks the defaults through the menus and drives the first race with the accelerator held.
