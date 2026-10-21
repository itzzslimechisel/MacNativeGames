#!/usr/bin/env python3
"""Package the native Windows x86-64 port of Tears of the Kingdom from YOUR OWN game files and keys.

  python build_windows.py base.nsp --update update-1.4.3.nsp -k prod.keys -o "C:\\Games"

Steps: (a) tools/windows/import_game.py verifies and extracts ExeFS + RomFS from your NSP(s) with your prod.keys;
(b) the runtime (games/totk, target totk-win) is copied from a prebuilt folder or built with CMake + LLVM-MinGW + Ninja;
(c) a self-contained folder is laid out:

  Tears of the Kingdom/
    Tears of the Kingdom.bat / .lnk   launcher (sets the environment, logs to logs/totk.log)
    settings.bat                      the settings you may edit (window size, sound, data folder, translator command)
    bin/totk-win.exe, runtime-index.bin, translator/   the runtime and the shader translator (needs the .NET 10 runtime)
    data/extracted/v143/{exefs,romfs} your game data, written by the import (about 17 GB; never share it)
    user-data/ shader-cache/ logs/

Nothing from the game or keys is downloaded or distributed. Python standard library only.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent
APP = 'Tears of the Kingdom'
VERSION = 'v143'          # the only game version the Windows runtime is patched for (1.4.3)
TRANSLATOR_DEFAULT = REPO / 'build' / 'totk_xlate-win'


def say(msg):
    print(f'[build_windows] {msg}', flush=True)


def die(msg):
    sys.exit(f'build_windows: {msg}')


def run(cmd, **kw):
    say(' '.join(f'"{c}"' if ' ' in str(c) else str(c) for c in cmd))
    if subprocess.call([str(c) for c in cmd], **kw):
        die(f'command failed: {cmd[0]}')


def find_tool(name, globs=()):
    found = shutil.which(name)
    if found:
        return found
    for base, pattern in globs:
        for p in sorted(Path(base).glob(pattern)) if Path(base).exists() else []:
            if p.is_file():
                return str(p)
    return None


def build_runtime(data_version_dir, build_dir):
    """Configure + build totk-win (Vulkan backend) with LLVM-MinGW, CMake and Ninja; returns the build dir."""
    winget = Path(os.environ.get('LOCALAPPDATA', '')) / 'Microsoft/WinGet/Packages'
    clang = find_tool('x86_64-w64-mingw32-clang', [(winget, '*LLVM-MinGW*/*/bin/x86_64-w64-mingw32-clang.exe')])
    cmake = find_tool('cmake', [('C:/Program Files/CMake/bin', 'cmake.exe')])
    ninja = find_tool('ninja', [(winget, '*Ninja*/ninja.exe')])
    if not (clang and cmake and ninja):
        die('building needs LLVM-MinGW (x86_64-w64-mingw32-clang), CMake and Ninja; or pass --prebuilt <folder>')
    env = os.environ.copy()
    env['PATH'] = os.pathsep.join([str(Path(clang).parent), str(Path(cmake).parent), str(Path(ninja).parent), env['PATH']])
    run([cmake, '-S', REPO / 'games/totk', '-B', build_dir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
         '-DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang', '-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++',
         '-DTOTK_GFX=vk', f'-DTOTK_DATA_DIR={data_version_dir.as_posix()}', f'-DTOTK_GAME_VERSION={VERSION}',
         '-DTOTK_BUILD_TESTS=OFF'], env=env)
    run([cmake, '--build', build_dir, '--target', 'totk-win'], env=env)
    return build_dir


SETTINGS = r'''@echo off
rem Settings for "@APP@". Edit and relaunch.
rem Folder holding extracted\@VER@\exefs and extracted\@VER@\romfs (the import output).
set "TOTK_DATA=@DATA@"
rem Window size at start (F11 or Alt+Enter toggles fullscreen).
set "TOTK_WINDOW_SIZE=1920x1080"
rem Set to 1 to run without sound.
set "TOTK_AUDIO_OFF=0"
rem Mouse as right stick and ZR/ZL/R3 buttons: remove "rem" on the next line.
rem set "TOTK_MOUSE=1"
rem Shader translator host (needs the .NET 10 runtime: https://dotnet.microsoft.com/download/dotnet/10.0).
set "TOTK_XLATE=dotnet.exe"
'''

LAUNCHER = r'''@echo off
setlocal
set "HERE=%~dp0"
call "%HERE%settings.bat"
if not exist "%HERE%logs" mkdir "%HERE%logs"
set "TOTK_ROMFS=%TOTK_DATA%\extracted\@VER@\romfs"
set "TOTK_ROMFS_INDEX=%HERE%bin\runtime-index.bin"
set "TOTK_PROFILE_DIR=%HERE%user-data"
set "TOTK_SHADER_CACHE=%HERE%shader-cache"
set "TOTK_XLATE_CMD="%TOTK_XLATE%" "%HERE%bin\translator\totk_xlate.dll""
if "%TOTK_AUDIO_OFF%"=="0" set "TOTK_AUDIO_OFF="
if not exist "%TOTK_ROMFS%\romfs.bin" (echo Game data not found in "%TOTK_DATA%". Edit settings.bat. & pause & exit /b 1)
pushd "%HERE%"
"%HERE%bin\totk-win.exe" "%TOTK_DATA%\extracted\@VER@\exefs" boot 2> "%HERE%logs\totk.log"
set RC=%ERRORLEVEL%
popd
if not "%RC%"=="0" (echo The game exited with code %RC%. See logs\totk.log & pause)
exit /b %RC%
'''


def write_bat(path, text):
    path.write_bytes(text.replace(chr(13), '').replace(chr(10), chr(13) + chr(10)).encode('utf-8'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base', nargs='?', type=Path, help='base game NSP (1.0.0); omit with --data-dir')
    ap.add_argument('--update', type=Path, help='cumulative update NSP (1.4.3 for the Windows runtime)')
    ap.add_argument('-k', '--keys', type=Path, help='your prod.keys')
    ap.add_argument('-o', '--out', type=Path, default=Path('.'), help=f'folder that gets "{APP}/" (outside the repository)')
    ap.add_argument('--data-dir', type=Path, help='keep the game data in this import output folder (holding extracted/) instead of <package>/data; without base/--update/-k it only reuses it')
    ap.add_argument('--prebuilt', type=Path, help='folder with totk-win.exe and runtime-index.bin (or gen/runtime-index.bin): copy instead of building')
    ap.add_argument('--translator', type=Path, default=TRANSLATOR_DEFAULT, help='folder with the published totk_xlate.dll')
    ap.add_argument('--romfs', choices=('full', 'index', 'none'), default='full', help='passed to import_game.py')
    ap.add_argument('--no-shortcut', action='store_true')
    ap.add_argument('--dry-run', action='store_true', help='print the plan only')
    a = ap.parse_args()

    out = a.out.expanduser().resolve()
    pkg = out / APP
    if out == REPO or REPO in out.parents:
        die('choose an output folder outside the repository (it will hold game data)')
    importing = bool(a.base)
    if importing and not (a.keys and a.update):
        die('give the base NSP, --update <1.4.3 NSP> and -k prod.keys (or only --data-dir of an earlier import)')
    if not importing and not a.data_dir:
        die('give the base NSP, --update and -k, or --data-dir of an earlier import')
    data = a.data_dir.expanduser().resolve() if a.data_dir else pkg / 'data'
    vdir = data / 'extracted' / VERSION
    say(f'package {pkg}; game data {data}')
    if a.dry_run:
        return

    # (a) import
    if importing:
        run([sys.executable, REPO / 'tools/windows/import_game.py', '--base', a.base, '--update', a.update, '--keys', a.keys,
             '--output', data, '--romfs', a.romfs])
        if not (vdir / 'romfs' / 'romfs.bin').is_file():
            die(f'import finished without {vdir}\\romfs\\romfs.bin: the Windows runtime needs the 1.4.3 update and --romfs full')

    # (b) runtime
    src = a.prebuilt.resolve() if a.prebuilt else (REPO / 'prebuilt/windows-x64' if (REPO / 'prebuilt/windows-x64/totk-win.exe').is_file() else None)
    if src is None:
        src = build_runtime(vdir, REPO / 'build' / 'totk-win-x64-vk')
    exe = src / 'totk-win.exe'
    index = next((p for p in (src / 'runtime-index.bin', src / 'gen/runtime-index.bin') if p.is_file()), None)
    if not exe.is_file() or not index:
        die(f'{src} lacks totk-win.exe or runtime-index.bin')
    if not (a.translator / 'totk_xlate.dll').is_file():
        die(f'translator not found at {a.translator} (publish games/totk/tools/totk_xlate, or pass --translator)')

    # (c) layout
    (pkg / 'bin').mkdir(parents=True, exist_ok=True)
    for d in ('user-data', 'shader-cache', 'logs'):
        (pkg / d).mkdir(exist_ok=True)
    shutil.copy2(exe, pkg / 'bin/totk-win.exe')
    shutil.copy2(index, pkg / 'bin/runtime-index.bin')
    tdst = pkg / 'bin/translator'
    if tdst.exists():
        shutil.rmtree(tdst)
    shutil.copytree(a.translator, tdst, ignore=shutil.ignore_patterns('*.pdb', '*.nupkg'))
    rel = '%~dp0data' if not a.data_dir else str(data)
    settings = pkg / 'settings.bat'
    if not settings.exists():   # keep the player's edits on a re-run
        write_bat(settings, SETTINGS.replace('@APP@', APP).replace('@VER@', VERSION).replace('@DATA@', rel))
    launcher = pkg / f'{APP}.bat'
    write_bat(launcher, LAUNCHER.replace('@VER@', VERSION))
    if not a.no_shortcut and os.name == 'nt':
        ps = (f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{pkg / (APP + '.lnk')}');"
              f"$s.TargetPath='{launcher}';$s.WorkingDirectory='{pkg}';$s.WindowStyle=7;$s.Save()")
        subprocess.call(['powershell', '-NoProfile', '-Command', ps])
    say(f'done. Launch "{launcher}" (or the .lnk). Log: {pkg / "logs/totk.log"}')


if __name__ == '__main__':
    main()
