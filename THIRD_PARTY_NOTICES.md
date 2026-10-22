# Third-party code

Code in this repository that comes from other projects keeps its license; the license file sits next to the code.

| Code | From | License | File |
|---|---|---|---|
| `games/totk/third_party/d2r` (NSO rewriter, verification tools) | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), adapted | MIT | `games/totk/third_party/d2r/LICENSE` |
| `games/totk/third_party/d2r_gfx` (NVN on Metal, texture tools, host headers) | d2r-mobile, extended for this game | MIT | `games/totk/third_party/d2r_gfx/LICENSE` |
| texture layout sizes in `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_tex.c` | deko3d | zlib | `games/totk/third_party/d2r_gfx/LICENSE.deko3d` |
| `games/totk/third_party/d2r_gfx/third_party/bcdec` | [bcdec](https://github.com/iOrange/bcdec) 0.985 | MIT or public domain | `games/totk/third_party/d2r_gfx/third_party/bcdec/LICENSE` |
| `games/totk/tools/totk_xlate` (shader translator front end) | d2r-mobile, adapted | MIT | `games/totk/tools/totk_xlate/LICENSE` |
| `vendor/ryujinx` (Ryujinx.Graphics.Shader, Ryujinx.Common, Spv.Generator) | Ryujinx, as pinned by d2r-mobile; patched by `games/totk/tools/totk_xlate/patches` | MIT | `vendor/ryujinx/LICENSE.txt` |
| `games/totk/prebuilt/macos-arm64/totk_xlate` (the shader translator, a native program) | built from `vendor/ryujinx` + the patches + `games/totk/tools/totk_xlate` with `publish_native.sh`; contains .NET runtime libraries compiled in by NativeAOT | MIT (Ryujinx, d2r-mobile, .NET) | `vendor/ryujinx/LICENSE.txt`, `games/totk/tools/totk_xlate/LICENSE` |
| audio effects in `games/totk/runtime/audio_reverb.c`, `games/totk/runtime/audio_effects.c` (delay, reverb, compressor) and the mixer | ported from Ryujinx.Audio | MIT | `games/totk/third_party/licenses/Ryujinx-Audio.txt` |

Fetched or linked at build time (not in this repository):

| Code | Use | License |
|---|---|---|
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) at `aa217ae` | SPIR-V to Metal Shading Language | Apache-2.0 |
| [hactool](https://github.com/SciresM/hactool) at `1d64a83` (with explicit private title-key-file hook), including its bundled mbedTLS | Base + update BKTR merge at build time only | ISC (hactool), Apache-2.0 (mbedTLS); see fetched source LICENSE files |
| [dynarmic](https://github.com/azahar-emu/dynarmic) (azahar-emu fork, maintained) at `a46601580d5512d324104f985b5f0209dc980ddc`, with its bundled submodules (mcl, fmt, xbyak, zydis/zycore, robin-map) | Windows x86-64 CPU backend (A64 JIT), FetchContent in `games/totk/CMakeLists.txt` | 0BSD (dynarmic); MIT (mcl, fmt, zydis, zycore, robin-map); BSD-3-Clause (xbyak) |
| [Boost](https://www.boost.org) 1.86.0 headers (`boost-1.86.0-b2-nodocs.tar.xz`, SHA-256 `a4d99d03...f51d`) | header-only dependency of dynarmic (icl, variant) | BSL-1.0 |
| libopus (Homebrew on macOS; [opus 1.5.2](https://opus-codec.org) source, SHA-256 `65c1d2f7...7ce1`, FetchContent on Windows), linked statically | Opus audio | BSD-3-Clause |
| libvpx (Homebrew on macOS; [libvpx v1.14.1](https://chromium.googlesource.com/webm/libvpx) source, SHA-256 `90174725...7977`, built by `games/totk/CMakeLists.txt` on Windows), linked statically | VP9 movies | BSD-3-Clause |
| [Dear ImGui](https://github.com/ocornut/imgui) v1.91.9 (`97428e8a`), fetched at launcher build time into `build/imgui` | Windows launcher / setup UI (`tools/windows/launcher`) | MIT |
| [Python embeddable package](https://www.python.org/downloads/windows/), `cryptography`, `lz4` (downloaded by `build_windows.py`) | Runs the import helper inside each Windows package (`support/python`) | PSF; Apache-2.0 / BSD-3-Clause (cryptography); BSD-3-Clause (lz4) |
