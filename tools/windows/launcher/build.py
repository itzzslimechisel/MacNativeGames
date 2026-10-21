#!/usr/bin/env python3
"""Build 'Tears of the Kingdom.exe' (GUI subsystem, LLVM-MinGW). usage: build.py <out.exe> [llvm-mingw bin dir]"""
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def find(names, dirs):
    for d in dirs:
        for n in names:
            if d and (Path(d) / (n + '.exe')).is_file():
                return str(Path(d) / (n + '.exe'))
    for n in names:
        if shutil.which(n):
            return shutil.which(n)
    return None


def build(out, bindir=None):
    wg = Path.home() / 'AppData/Local/Microsoft/WinGet/Packages'
    dirs = [bindir] + ([p for p in sorted(wg.glob('*LLVM-MinGW*/*/bin'))] if wg.exists() else [])
    cc = find(['x86_64-w64-mingw32-clang', 'clang'], dirs)
    rc = find(['x86_64-w64-mingw32-windres', 'llvm-windres', 'windres'], dirs)
    if not cc or not rc:
        raise SystemExit('LLVM-MinGW (clang + windres) not found; pass its bin folder as 2nd argument')
    ico = HERE / 'totk.ico'
    if not ico.is_file():
        subprocess.check_call([sys.executable, str(HERE / 'make_icon.py'), str(ico)])
    out = Path(out).resolve()
    res = out.with_suffix('.res.o')
    subprocess.check_call([rc, '-i', 'launcher.rc', '-o', str(res), '-O', 'coff'], cwd=HERE)
    subprocess.check_call([cc, '-Os', '-s', '-mwindows', '-municode', '-o', str(out), str(HERE / 'launcher.c'), str(res),
                           '-lshell32', '-static'])
    res.unlink()


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
