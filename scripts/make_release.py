#!/usr/bin/env python3
"""Makes one version of the port, laid out as its two GitHub releases serve it (see ext/trg-launcher/launcher2/README.md).

    python scripts/make_release.py                      the version in VERSION
    python scripts/make_release.py --earlier 1.0.0      a later version: its update removes what 1.0.0 shipped and it does not

Writes release/v<version>/:
    CartoonNetworkRacing-v<version>-windows-x64.zip     the release players see (tag v<version>): the portable launcher with
                                                        both games in its games folder
    packages/                                           the packages release (tag packages-v<version>), what the launcher
                                                        downloads: its own update, each game, and SHA256SUMS.txt

Build the games first (build/cnr, build/cnr_eu); the launcher is rebuilt here. Everything comes from a clean stage folder,
so nothing of the player's goes in: no saves, settings, logs, shader cache or disc. The script checks that as well, by
name and by comparing every file with the game's own files when extracted/ is there. Keep every zip you publish.
"""
import argparse
import glob
import hashlib
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / 'ext' / 'trg-launcher' / 'tools' / 'make_update_files.py'
CRT_GLOB = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT'
CRT_FILES = ('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')

# What the game loads, worked out with dumpbin from the executable down, plus shaderc (Vulkan loads it by name). The build
# folder holds more: the rest come along with PCSX2's and FFmpeg's bundles and nothing uses them.
GAME_DLLS = ('avcodec-61.dll', 'avutil-59.dll', 'brotlicommon.dll', 'brotlidec.dll', 'brotlienc.dll', 'dxcompiler.dll',
             'jpeg62.dll', 'jxl.dll', 'jxl_cms.dll', 'jxl_threads.dll', 'libpng16.dll', 'libsharpyuv.dll', 'libwebp.dll',
             'libwebpmux.dll', 'SDL3.dll', 'shaderc_shared.dll', 'swresample-5.dll', 'swscale-8.dll', 'z.dll', 'zlib1.dll',
             'zstd.dll')

LAUNCHER = {'folder': 'CartoonNetworkRacing',
            'protect': ('games', 'save-backups', 'cache', 'cnr.txt', 'test-userdata')}
GAMES = ({'folder': 'CartoonNetworkRacingUSA', 'id': 'cnr_usa', 'build': 'cnr'},
         {'folder': 'CartoonNetworkRacingEU', 'id': 'cnr_eu', 'build': 'cnr_eu'})
GAME_PROTECT = ('game', 'logs', 'saves', 'cache', 'textures', 'fpe.log')

# Never in a release, whatever the path: the player's data or the game's own files.
FORBIDDEN_NAMES = ('disc.iso', 'iconlst.icn', 'icon.sys', 'system.cnf', 'cdfiles.dat', 'archive.ar', 'iopmain.irx')
FORBIDDEN_PARTS = ('saves', 'logs', 'cache', 'textures', 'game', 'mc0', 'mc1', 'modules', 'test-userdata')
FORBIDDEN_ENDINGS = ('.toml', '.pdb', '.log', '.iso', '.pss', '.irx', '.mcd', '.old', '.new', '.part', '.ilk', '.lib')

README = '''Cartoon Network Racing Recompiled {version}

Run cnr_launcher.exe. The first time you press Play, it asks for a disc image you made from your own USA or European
PlayStation 2 disc. No game files are included. Your discs, saves and settings all stay in this folder, so you can move it.

Windows 10 or 11 (64 bit), a processor with AVX2 and a graphics card with Direct3D 11.

Source code and help: https://github.com/TekRantGaming/cartoon-network-racing-recompiled
This program is free software under the GNU General Public License 3.0 (LICENSE.txt).
Third-party licences: THIRD-PARTY-NOTICES.txt.
'''


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def notices():
    """The bundled parts' licences, one after another, from docs/third-party-licenses."""
    folder = ROOT / 'docs' / 'third-party-licenses'
    text = (folder / 'README.txt').read_text(encoding='utf-8')
    for f in sorted(folder.iterdir()):
        if f.name != 'README.txt':
            text += '\n' + '=' * 78 + '\n' + f.stem + '\n' + '=' * 78 + '\n\n' + f.read_text(encoding='utf-8', errors='replace')
    return text


def common_files(stage, version):
    shutil.copy2(ROOT / 'LICENSE', stage / 'LICENSE.txt')
    (stage / 'THIRD-PARTY-NOTICES.txt').write_text(notices(), encoding='utf-8')
    (stage / 'README.txt').write_text(README.format(version=version), encoding='utf-8')


def package(stage, zip_path, version, folder, protect, earlier):
    args = [sys.executable, str(TOOL), 'package', str(stage), '--version', version, '--zip', str(zip_path),
            '--asset', f'{folder}-*-windows-x64.zip']
    for p in protect:
        args += ['--protect', p]
    if earlier:
        args += ['--earlier', str(ROOT / 'release' / f'v{earlier}' / 'packages' / f'{folder}-v{earlier}-windows-x64.zip')]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0 or 'PACKAGE CHECK PASSED' not in r.stdout:
        sys.exit(f'packaging {folder} failed:\n{r.stdout}\n{r.stderr}')


def check_zip(path, game_hashes):
    """Stops the release if anything of the player's or the game's is in a zip."""
    problems = []
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            parts = [p.lower() for p in info.filename.split('/')]
            name = parts[-1]
            if name in FORBIDDEN_NAMES or name.endswith(FORBIDDEN_ENDINGS) or name.startswith(('slus_', 'sles_')) \
                    or set(parts[:-1]) & set(FORBIDDEN_PARTS):
                problems.append(info.filename)
            elif game_hashes and hashlib.sha1(z.read(info)).hexdigest() in game_hashes:
                problems.append(info.filename + ' (identical to a file from the disc)')
    if problems:
        sys.exit(f'{path.name} must not hold:\n  ' + '\n  '.join(problems))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--earlier', default='', help='the previous version, when there is one')
    args = ap.parse_args()
    version = (ROOT / 'VERSION').read_text().strip()

    top = ROOT / 'release' / f'v{version}'
    if top.exists():
        sys.exit(f'{top} already exists')
    crt = [Path(p) for p in sorted(glob.glob(CRT_GLOB))]
    if not crt:
        sys.exit('the Visual C++ redistributable folder was not found')
    for g in GAMES:
        if not (ROOT / 'build' / g['build'] / 'cnr.exe').exists():
            sys.exit(f"build/{g['build']}/cnr.exe is missing: build the games first")

    r = subprocess.run(['cmd', '/c', 'scripts\\vsenv.bat', 'cmake', '--build', 'build\\launcher'], capture_output=True,
                       text=True, cwd=ROOT)
    if r.returncode != 0:
        sys.exit('the launcher did not build:\n' + r.stdout[-2000:])

    stage_root = ROOT / 'release' / 'stage' / f'v{version}'
    shutil.rmtree(stage_root, ignore_errors=True)
    packages = top / 'packages'
    packages.mkdir(parents=True)

    stage = stage_root / LAUNCHER['folder']
    stage.mkdir(parents=True)
    shutil.copy2(ROOT / 'build' / 'launcher' / 'cnr_launcher.exe', stage)
    common_files(stage, version)
    zips = [packages / f"{LAUNCHER['folder']}-v{version}-windows-x64.zip"]
    package(stage, zips[0], version, LAUNCHER['folder'], LAUNCHER['protect'], args.earlier)

    for g in GAMES:
        src = ROOT / 'build' / g['build']
        stage = stage_root / g['folder']
        stage.mkdir(parents=True)
        shutil.copy2(src / 'cnr.exe', stage / f"{g['id']}.exe")
        for dll in GAME_DLLS:
            shutil.copy2(src / dll, stage)
        for dll in CRT_FILES:
            shutil.copy2(crt[-1] / dll, stage)
        shutil.copytree(src / 'resources', stage / 'resources')
        common_files(stage, version)
        zips.append(packages / f"{g['folder']}-v{version}-windows-x64.zip")
        package(stage, zips[-1], version, g['folder'], GAME_PROTECT + (g['id'] + '.toml',), args.earlier)

    # The players' release: the launcher with each game in games/<id>, as the launcher's own download would put it there.
    players = stage_root / 'players' / LAUNCHER['folder']
    shutil.copytree(stage_root / LAUNCHER['folder'], players)
    for g in GAMES:
        shutil.copytree(stage_root / g['folder'], players / 'games' / g['id'])
    players_zip = top / f"{LAUNCHER['folder']}-v{version}-windows-x64.zip"
    with zipfile.ZipFile(players_zip, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(players.rglob('*')):
            if f.is_file():
                z.write(f, f.relative_to(players.parent).as_posix())

    # One SHA256SUMS.txt for the whole release: Windows line ends, two spaces (each package call wrote one for its own zip).
    lines = [f'{sha256(p)}  {p.name}' for p in sorted(packages.glob('*.zip'))]
    (packages / 'SHA256SUMS.txt').write_bytes(('\r\n'.join(lines) + '\r\n').encode('ascii'))

    game_hashes = set()
    for f in (ROOT / 'extracted').rglob('*'):
        if f.is_file():
            game_hashes.add(hashlib.sha1(f.read_bytes()).hexdigest())
    for z in zips + [players_zip]:
        check_zip(z, game_hashes)
        print(f'{z.name:<52} {z.stat().st_size / 1e6:6.1f} MB')
    print(f'SHA256SUMS.txt: {len(lines)} zips')
    print('Next: every zip through VirusTotal before anything is published. Keep these zips: the next release needs them.')


if __name__ == '__main__':
    main()
