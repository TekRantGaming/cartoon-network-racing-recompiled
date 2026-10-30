#!/usr/bin/env python3
"""Lays out the portable launcher folder: dist/CartoonNetworkRacing with cnr_launcher.exe and the two games.

    python scripts/stage_launcher.py

Program files come from build/cnr, build/cnr_eu and build/launcher. What the player owns or made stays as it is: the
installed disc (game/), the settings (<id>.toml), saves/, logs/ and the shader cache/. Saves from the development builds
(build/<game>/saves) are copied once, while the game has none. The launcher's Install step adds the disc.
"""
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / 'dist' / 'CartoonNetworkRacing'
GAMES = (('cnr_usa', 'cnr'), ('cnr_eu', 'cnr_eu'))


def stage_game(game, build):
    src = ROOT / 'build' / build
    if not (src / 'cnr.exe').exists():
        sys.exit(f'{src / "cnr.exe"} is missing: build the game first')
    out = DIST / 'games' / game
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / 'cnr.exe', out / f'{game}.exe')
    for dll in src.glob('*.dll'):
        shutil.copy2(dll, out / dll.name)
    shutil.copytree(src / 'resources', out / 'resources', dirs_exist_ok=True)
    if (src / 'saves').is_dir() and not (out / 'saves').exists():
        shutil.copytree(src / 'saves', out / 'saves')
        print(f'{game}: copied the saves from {src / "saves"}')


def main():
    launcher = ROOT / 'build' / 'launcher' / 'cnr_launcher.exe'
    if not launcher.exists():
        sys.exit(f'{launcher} is missing: build it first (launcher/CMakeLists.txt)')
    DIST.mkdir(parents=True, exist_ok=True)
    shutil.copy2(launcher, DIST / 'cnr_launcher.exe')
    for game, build in GAMES:
        stage_game(game, build)
    print(f'staged {DIST}')


if __name__ == '__main__':
    main()
