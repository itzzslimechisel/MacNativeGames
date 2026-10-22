#!/usr/bin/env python3
"""Build 'Tears of the Kingdom.exe' (GUI subsystem, LLVM-MinGW): launcher.cpp + Dear ImGui (Win32 + Direct3D 11 backends).
usage: build.py <out.exe> [llvm-mingw bin dir]
Dear ImGui is fetched once, pinned to the v1.91.9 tag, into <repo>/build/imgui (git clone; MIT license, see THIRD_PARTY_NOTICES.md)."""
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
IMGUI_TAG = 'v1.91.9'
IMGUI_COMMIT = '97428e8ac99e339ce05eee531cf55b77b29ea709'
IMGUI_URL = 'https://github.com/ocornut/imgui'
SRCS = ['imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'backends/imgui_impl_win32.cpp', 'backends/imgui_impl_dx11.cpp']


def find(names, dirs):
    for d in dirs:
        for n in names:
            if d and (Path(d) / (n + '.exe')).is_file():
                return str(Path(d) / (n + '.exe'))
    for n in names:
        if shutil.which(n):
            return shutil.which(n)
    return None


def fetch_imgui(root):
    dst = root / 'build' / 'imgui'
    if not (dst / 'imgui.cpp').is_file():
        dst.parent.mkdir(exist_ok=True)
        subprocess.check_call(['git', 'clone', '-q', '--depth', '1', '--branch', IMGUI_TAG, IMGUI_URL, str(dst)])
    head = subprocess.check_output(['git', '-C', str(dst), 'rev-parse', 'HEAD'], text=True).strip()
    if head != IMGUI_COMMIT:
        raise SystemExit(f'{dst} is at {head}, expected {IMGUI_COMMIT} ({IMGUI_TAG}): delete it to fetch again')
    return dst


def build(out, bindir=None):
    wg = Path.home() / 'AppData/Local/Microsoft/WinGet/Packages'
    dirs = [bindir] + ([p for p in sorted(wg.glob('*LLVM-MinGW*/*/bin'))] if wg.exists() else [])
    cxx = find(['x86_64-w64-mingw32-clang++'], dirs)
    rc = find(['x86_64-w64-mingw32-windres', 'llvm-windres', 'windres'], dirs)
    if not cxx or not rc:
        raise SystemExit('LLVM-MinGW (clang++ + windres) not found; pass its bin folder as 2nd argument')
    imgui = fetch_imgui(HERE.parents[2])
    ico = HERE / 'totk.ico'
    if not ico.is_file():
        subprocess.check_call([sys.executable, str(HERE / 'make_icon.py'), str(ico)])
    out = Path(out).resolve()
    work = out.parent / (out.stem + '-obj')
    work.mkdir(exist_ok=True)
    flags = ['-Os', '-std=c++17', '-DNDEBUG', '-DUNICODE', '-D_UNICODE', f'-I{imgui}', f'-I{imgui / "backends"}', '-fno-exceptions', '-Wno-deprecated-declarations']

    def compile_(src):
        s = Path(src)
        o = work / (s.stem + '.o')
        if not o.is_file() or o.stat().st_mtime < s.stat().st_mtime:
            subprocess.check_call([cxx, *flags, '-c', str(s), '-o', str(o)])
        return str(o)
    with ThreadPoolExecutor(4) as ex:
        objs = list(ex.map(compile_, [imgui / s for s in SRCS] + [HERE / 'launcher.cpp']))
    res = work / 'launcher.res.o'
    subprocess.check_call([rc, '-i', 'launcher.rc', '-o', str(res), '-O', 'coff'], cwd=HERE)
    subprocess.check_call([cxx, '-s', '-mwindows', '-municode', '-static', '-o', str(out), *objs, str(res),
                           '-ld3d11', '-ldxgi', '-ld3dcompiler', '-ldwmapi', '-lgdi32', '-lshell32', '-lole32', '-luuid', '-lcomdlg32'])
    shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
