# Windows 11 build (native x86-64 port)

`build_windows.py` makes a self-contained `Tears of the Kingdom <version>/` folder (one per game version) from **your own** NSP files and `prod.keys`. The game's ARM64 code is run by a JIT (dynarmic) inside a native Windows runtime that re-implements the Switch OS services; graphics go through a Vulkan backend. No emulator is downloaded, and nothing from the game, no keys, no firmware are distributed. You must own the game; the result is for your own use, do not share it.

## Requirements

- Windows 11 x64, a GPU and driver with **Vulkan 1.3**, a sound device, ~20 GB free disk, 16 GB RAM or more (the process takes about 6 GB).
- **.NET 10 runtime** (`dotnet.exe` on PATH): the shader translator is a framework-dependent .NET program started by the game (not AOT yet).
- Python 3.8+ with `pip install cryptography` (for the import).
- Game files: the base NSP (1.0.0) and optionally one update NSP, plus your `prod.keys`. Versions with a Windows patch table: **1.0.0** (base alone), **1.1.1**, **1.2.0**, **1.4.3** (smoke-tested: boot to rendered frames through Vulkan), 1.1.2 and 1.2.1 (tables from the Mac port, not tested on Windows). 1.4.2 and other updates are refused by the import (no patch table).
- Only to build the runtime yourself: LLVM-MinGW (UCRT, x86_64), CMake, Ninja, Python; MSYS2 `make`/`bash` for the cutscene decoder (libvpx), otherwise cutscenes use a built-in decoder that stops at the first one.

## Run

```
python build_windows.py base.nsp --update update-1.4.3.nsp -k prod.keys --version 1.4.3 -o D:\Games   # -> "Tears of the Kingdom 1.4.3"
python build_windows.py base.nsp -k prod.keys --version 1.0.0 -o D:\Games                            # -> "Tears of the Kingdom 1.0.0"
python build_windows.py -o D:\Games --data-dir <earlier import output> --version 1.2.0               # no import, no copy
python build_windows.py ... --prebuilt <folder with totk-win.exe + runtime-index.bin built for that version>
```

The runtime is built per version (`build/totk-win-x64-vk-<ver>`, `-DTOTK_GAME_VERSION=v0|v111|v120|v143|...`): the patch tables in `runtime/game_version.h` are compiled in. One import folder can hold several versions (`extracted/<ver>`); packages in the same output folder share the translator files as hard links. Then start `Tears of the Kingdom <version>.bat` (or the `.lnk`). The output folder must be outside the repository. Edit `settings.bat` for the window size, sound off, mouse-as-stick and the data folder; the log is `logs\totk.log`. `--translator <dir>` points at the published `totk_xlate.dll` (default `build\totk_xlate-win`; built from `games/totk/tools/totk_xlate`, needs the vendored Ryujinx shader sources).

## Controls

Keyboard: the same layout as the Mac build (`keymap_sdl.h`, remappable through `controls.txt`). Controllers: any SDL3-supported pad (XInput, DualSense, Switch Pro), rumble and motion. **F11 / Alt+Enter**: fullscreen. Closing the window waits for a running save.

## What works / what is untested

- Works (verified on the author's PC): import of both NSPs, boot, the prologue rendering in the window at 1920x1080 through Vulkan, sound through WASAPI (4 start-up underruns, none after), window events.
- Untested by hand: keyboard and gamepad input in game (the code is wired and the controller / keyboard devices initialize, but nobody has played with them), rumble, motion, fullscreen toggling, window resize, text entry, saving, long play sessions, other GPUs (tested on one NVIDIA card only).
- Performance is not good yet: the first run translates every shader on demand (slow, stuttering; the shader cache in `shader-cache\` fills up and later runs are faster) and frames are often far below 30 FPS. No picture-side FPS overlay on Windows.
- 1.1.1 and 1.2.0 (Windows only) have no cloth/hair real-time clock patch (cloth follows the game clock at frame rates other than 30). The tests and the CMake build for ARM64 Windows are experimental.
