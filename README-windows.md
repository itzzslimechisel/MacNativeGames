# Windows 11 build (native x86-64 port)

`build_windows.py` makes a self-contained `Tears of the Kingdom/` folder from **your own** NSP files and `prod.keys`. The game's ARM64 code is run by a JIT (dynarmic) inside a native Windows runtime that re-implements the Switch OS services; graphics go through a Vulkan backend. No emulator is downloaded, and nothing from the game, no keys, no firmware are distributed. You must own the game; the result is for your own use, do not share it.

## Requirements

- Windows 11 x64, a GPU and driver with **Vulkan 1.3**, a sound device, ~20 GB free disk, 16 GB RAM or more (the process takes about 6 GB).
- **.NET 10 runtime** (`dotnet.exe` on PATH): the shader translator is a framework-dependent .NET program started by the game (not AOT yet).
- Python 3.8+ with `pip install cryptography` (for the import).
- Game files: the base NSP (1.0.0) and the **1.4.3 update NSP** (the runtime is only patched for 1.4.3), plus your `prod.keys`.
- Only to build the runtime yourself: LLVM-MinGW (UCRT, x86_64), CMake, Ninja, Python; MSYS2 `make`/`bash` for the cutscene decoder (libvpx), otherwise cutscenes use a built-in decoder that stops at the first one.

## Run

```
python build_windows.py base.nsp --update update-1.4.3.nsp -k prod.keys -o D:\Games
python build_windows.py base.nsp --update update-1.4.3.nsp -k prod.keys -o D:\Games --prebuilt <folder with totk-win.exe + runtime-index.bin>
python build_windows.py -o D:\Games --data-dir <earlier import output>     # no import, no copy
```

Then start `Tears of the Kingdom.bat` (or the `.lnk`). The output folder must be outside the repository. Edit `settings.bat` for the window size, sound off, mouse-as-stick and the data folder; the log is `logs\totk.log`. `--translator <dir>` points at the published `totk_xlate.dll` (default `build\totk_xlate-win`; built from `games/totk/tools/totk_xlate`, needs the vendored Ryujinx shader sources).

## Controls

Keyboard: the same layout as the Mac build (`keymap_sdl.h`, remappable through `controls.txt`). Controllers: any SDL3-supported pad (XInput, DualSense, Switch Pro), rumble and motion. **F11 / Alt+Enter**: fullscreen. Closing the window waits for a running save.

## What works / what is untested

- Works (verified on the author's PC): import of both NSPs, boot, the prologue rendering in the window at 1920x1080 through Vulkan, sound through WASAPI (4 start-up underruns, none after), window events.
- Untested by hand: keyboard and gamepad input in game (the code is wired and the controller / keyboard devices initialize, but nobody has played with them), rumble, motion, fullscreen toggling, window resize, text entry, saving, long play sessions, other GPUs (tested on one NVIDIA card only).
- Performance is not good yet: the first run translates every shader on demand (slow, stuttering; the shader cache in `shader-cache\` fills up and later runs are faster) and frames are often far below 30 FPS. No picture-side FPS overlay on Windows.
- Only 1.4.3 is supported; the tests and the CMake build for ARM64 Windows are experimental.
