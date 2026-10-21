# Windows port plan (native, not an emulator wrapper)

Status: planning document. Nothing here is implemented yet. The current `build_windows.py` / `README-windows.md`
path wraps a Ryujinx-compatible emulator; this plan replaces it with a real port of the macOS runtime: the game's
own ARM64 code runs in-process, Horizon services are answered by the host, and NVN is rebuilt on a Windows GPU API.

All file paths below are relative to `games/totk/` unless they start with `docs/` or are top-level (`build_app.py`).
LOC counts are `wc -l` at the time of writing.

---

## 0. How the macOS runtime actually works (what the port must preserve)

Facts taken from the code, because they drive every decision below.

1. **The guest code is pre-rewritten, not translated.** `third_party/d2r/patch_nso.py` (1105 LOC, Python + capstone)
   rewrites every instruction macOS cannot run as-is into a branch to a stub in a patch area appended to each module:
   - `x18` reads/writes -> thread slot `K18`
   - `mrs/msr tpidr_el0` -> thread slot `KTLS`; `mrs tpidrro_el0` -> slot `KTLR` (Horizon TLR pointer)
   - `svc #n`, `mrs cntpct_el0` -> host gate (`runtime/gate.S`, address in slot `KGATE`)
   - LDXR/STXR "exclusive windows" and `br/blr x18` jump tables get special windows (`call_window.py`).
   Every stub locates its thread slots with **`mrs xT, tpidrro_el0` (the Darwin TSD base) + key-page offset**.
   The key page holds "actual pthread key number x 8", written by the loader (`runtime/loader.c:292-294` checks the
   Darwin TSD layout). The output is byte-verified against `assets/expected-v*.json`; each version's rewrite was
   equivalence-checked site by site (`d2r/equiv.py`, `d2r/structck.py`).
2. **One shared address space; guest pointer == host pointer.** `runtime/loader.c` maps main/sdk/subsdk0 at their
   original relative layout (mmap + copy + `mprotect` RX + `sys_icache_invalidate`), resolves imports, and replaces
   chosen imports with host C functions (`runtime_override`, `runtime_*_override`). NVN is taken over at
   `nvnBootstrapLoader` (`hle_nvn.c`); the SDK's NVN driver never runs. Host C code reads/writes guest structures by
   raw pointer, and guest code calls host C functions directly with AAPCS64.
3. **Guest memory must be below 2^39** (HIPC descriptors encode 39-bit addresses; `runtime/services.c:18-123`).
   The host reserves stack + heap + a 64 GiB "alias" region (MapPhysicalMemory, svc 0x2c/0x2d) at fixed addresses,
   with a fallback hole search (macOS 27, iOS). Commit/decommit uses `mprotect` / `mmap(MAP_FIXED)`, tracked per 4 KiB
   page but executed at the 16 KiB macOS page size. `stack_move` implements svcMapMemory by copy + protect.
4. **Real page aliasing exists in exactly one place**: NVN virtual memory pools (`third_party/d2r_gfx/gfx/nvn/totk_vpool.c`)
   map 64 KiB pages of physical pools a second time with `mach_vm_remap(copy=FALSE)`.
5. **Fault-driven tricks**: `nvn_dirty.c` write-protects texture pool pages and catches SIGSEGV/SIGBUS to mark them
   dirty; `loader.c` emulates `mrs CTR_EL0` on SIGILL and installs crash handlers; `hle_nvn.c` / `nvn_mtl.m` guard
   guest reads with `sigsetjmp`/`siglongjmp` (`runtime_fault_guard`).
6. **Threads are 1:1 host threads** with a Horizon-style core/priority scheduler in `runtime/threads.c` built on
   pthread mutex/cond. Guest threads run on guest stacks (`runtime/thread_enter.S`); host calls switch to a per-thread
   host stack held in a TSD slot (`runtime/hle_stack.S`, `gate.S`). Async preemption (SIGUSR2 +
   `thread_suspend`/`thread_get_state`) is `GAME_BOTW`-only; TOTK preempts at SVC entry.
7. **GPU path assumes unified memory.** Buffers are `newBufferWithBytesNoCopy` over guest pool memory
   (`nvn_mtl.m:665`, `MTLResourceStorageModeShared`); textures are uploaded/deswizzled and re-uploaded when dirty.
   Shaders: Maxwell -> Ryujinx translator -> **SPIR-V** -> SPIRV-Cross -> MSL (`tools/totk_xlate`, .NET NativeAOT,
   spawned with `posix_spawn` from `totk_shader_rt.m`). Reflection JSON is derived from MSL text (`Service.cs`).
8. No JIT, no `MAP_JIT`, no `pthread_jit_write_protect_np`: code pages are written once at load, then RX.

---

## 1. Inventory of runtime components

Legend for APIs: **Mach** = `mach_vm_*`/`task_*`/`thread_*` Mach calls; **TSD** = Darwin TSD via `tpidrro_el0`;
**sig** = POSIX signals/ucontext; **GCD** = libdispatch; **pth** = pthreads (portable-ish); **mtime** = `mach_absolute_time`/timebase.

### 1a. Loader, CPU/ABI glue, memory

| Component | Files | Lang | LOC | Apple/POSIX-specific APIs |
|---|---|---|---|---|
| Module loader, import resolution, crash handlers, CTR_EL0 emulation, mode dispatch | `runtime/loader.c`, `runtime/runtime.h` | C | 544 + 151 | mmap, mprotect, `mach_vm_remap` (iOS embedded path), `mach_vm_read_overwrite` (25x, safe reads), `mach_vm_region`, `sys_icache_invalidate`, sigaction SIGSEGV/BUS/ILL/TRAP/ABRT + ucontext `__ss`, pthread keys, **TSD layout check**, `getsectiondata` (iOS) |
| Host gate (svc/cntpct entry), save/restore x0-x30,q0-q31,nzcv,fpsr,fpcr | `runtime/gate.S` | ARM64 asm (Mach-O syntax) | 124 | **TSD** (`tpidrro_el0`), `@PAGE/@PAGEOFF`, `_` symbol prefix |
| Host-stack switch for host calls | `runtime/hle_stack.S`, `runtime/hle_stack.c` | asm + C | 97 + 109 | **TSD**, dlsym |
| Guest thread entry/leave | `runtime/thread_enter.S` | asm | 24 | Mach-O syntax, pthread_exit |
| Horizon memory: layout below 2^39, heap, alias region, svcMapMemory, permissions, GetInfo | `runtime/services.c` | C | 408 | `mach_vm_map(FIXED)`, `mach_vm_deallocate`, `mach_vm_region`, mmap MAP_FIXED, mprotect, `pthread_get_stackaddr_np` |
| Memory diagnostics | `runtime/memory_probe.c`, `runtime/world_survey.c` | C | 82 + 191 | `mach_vm_region`, `mach_vm_read_overwrite` |
| Code rewriter + verifiers (build time) | `third_party/d2r/*.py` | Python | 2194 | none (portable); **encodes the TSD access primitive into the output** |

### 1b. Horizon kernel/services (mostly portable C)

| Component | Files | Lang | LOC | APIs |
|---|---|---|---|---|
| Threads, sync, scheduler, events, WaitSynchronization, address arbiter | `runtime/threads.c`, `thread_compat.h` | C | 1036 + 13 | pth (mutex/cond/keys), `pthread_mach_thread_np`, `thread_suspend`/`thread_get_state` (diagnostics + BotW preemption), SIGUSR2 (BotW), `pthread_*_qos_class_np`, `mach_vm_read_overwrite` |
| IPC / service manager dispatch | `runtime/ipc.c`, `ipc.h` | C | 266 | pth |
| File system (saves, profile), RomFS, DLC RomFS | `runtime/fs_host.c`, `romfs.c`, `aoc_romfs.c`, `romfs_path.h` | C | 373+343+177+30 | POSIX file I/O, mmap (read-only image), ftruncate, pth |
| Time, settings, account, shared font, applets, swkbd | `time_host.c`, `settings_service.c`, `account.c`, `shared_font.c`, `applet_host.c`, `swkbd_host.c` | C | 128+51+59+54+117+271 | mtime, `mach_continuous_time`, pth, `pthread_threadid_np` |
| Audio renderer (Horizon audren wire format) + effects + reverb | `audio_renderer.c`, `audio_effects.c`, `audio_reverb.c` (+ .h) | C | 598+183+82 (+139) | pth, `pthread_set_qos_class_self_np`, clock_gettime |
| Audio output | `runtime/audio_mixer.c`, `.h` | C | 767 + 41 | **CoreAudio AudioUnit / AudioComponent / kAudio\*** |
| Opus decoder, movie (VP9) | `opus_codec.c`, `movie_host.c` | C | 146 + 467 | libopus, libvpx (Homebrew static libs), pth |
| HID state, motion fusion, feedback, autoload | `input.c`, `motion_fusion.c`, `hid_feedback.c`, `autoload.c`, `controller_map.h`, `gamepad_layout.h` | C | 233+145+158+91+108+49 | pth |
| Game controllers | `runtime/gamepad.m` | Obj-C | 196 | **GameController** (GCController, GCExtendedGamepad, GCDualSense), GCD |
| Rumble / adaptive triggers / DualSense HID | `runtime/gamepad_feedback.m` | Obj-C | 316 | **CoreHaptics** (CHHaptic\*), **IOKit HID**, GCD timers |
| Window, keyboard, focus, text entry, CAMetalLayer, main loop | `runtime/frontend.m` (`frontend_ios.m` = iOS variant) | Obj-C | 408 (142) | **AppKit** (NSApplication/NSWindow/NSView), CAMetalLayer, GCD main queue |
| Fixed game patches (frame timing, 60 FPS, output res, aspect) | `game_patches.c`, `game_patches_v143.c`, `aspect_patch.h`, `game_version.h` (`game_patches_botw.c` BotW) | C | 595+496+102+166 (519) | v143: `task_threads`/`thread_suspend`/`ARM_DEBUG_STATE64` watchpoints (diagnostic only), ucontext |
| Game slot (single instance) | `runtime/game_slot.c` | C | 355 | POSIX file locks, clock_gettime |
| GFX bridge glue | `runtime/gfx_bridge.c`, `gfx_probe.c` | C | 86 + 115 | mprotect, sig/ucontext, `mach_vm_read_overwrite` |
| GPU/vpool probes (dev only) | `runtime/gpu_path_probe.m`, `vpool_probe.m` | Obj-C | 37 + 88 | Metal |
| Launcher | `app/launcher.m` | Obj-C | 268 | **Cocoa** (NSApplication, NSFileManager, NSBundle), `~/Library/Application Support` paths |

### 1c. Graphics (NVN on Metal), `third_party/d2r_gfx/gfx/`

| Component | Files | Lang | LOC | APIs |
|---|---|---|---|---|
| NVN API layer (objects, command recording, pools, sync, queue) | `nvn/hle_nvn.c`, `nvn_obj.h`, `nvn_protos.h`, `nvn_hmap.h`, `nvn_pool_index.h`, small headers | C | 2937 + ~1500 | pth, mprotect, sigsetjmp guards, mtime, `task_info`, a few MTL types leak via `nvn_obj.h` |
| NVN entry thunks (call counting + slot jump), 1050 entries | `nvn/nvn_thunks.S` (generated by upstream `gen_protos.py`, not in repo), `nvn_entry.S` | asm | 6813 + 79 | Mach-O syntax, LSE `stadd`, mtime |
| Dirty-page tracking | `nvn/nvn_dirty.c/.h` | C | 281 | mprotect + SIGSEGV handler, mtime |
| Virtual memory pools | `nvn/totk_vpool.c/.h` | C | 147 | **`mach_vm_remap` (true aliasing)**, `mach_vm_allocate` |
| **Metal replay backend** | `nvn/nvn_mtl.m`, `nvn_mtl.h`, `nvn_refl.m`, `nvn_render_stream.h`, `nvn_cube_group.h`, `nvn_rtex.inc`, `rt_range_check.inc`, probes | Obj-C | 14186 + ~900 | **Metal 4** (2557 MTL refs, MTL4 encoders/argument tables/residency), CAMetalLayer, os_unfair_lock, GCD, NSFileManager |
| MetalFX upscaling / frame interpolation / TAA | `nvn/totk_mfx.m`, `totk_mfx_up.m`, `totk_taa.m` | Obj-C | 386+235+238 | **MetalFX**, Metal |
| Shader runtime (spawn translator, cache, seed, scope) | `nvn/totk_shader_rt.m`, `totk_shader_seed.h`, `totk_shader_observed.h` | Obj-C | 353+216+69 | `posix_spawn`, GCD, NSFileManager, NSBundle |
| Texture formats, deswizzle, BC decode, BC1->ASTC, GPU deswizzle | `tex/*` (+ `third_party/bcdec`) | C, Obj-C, MSL | 1547 | `nvn_fmt_mtl.c` maps to MTLPixelFormat; `nvn_deswizzle_gpu.m`, `nvn_bctc.metal`, `*_msl.h` are Metal |
| Overlays, sysload, timeline, lifelog | `fps_overlay.c`, `totk_sysload.c`, `totk_timeline.c`, `totk_lifelog.c`, `totk_nvn_ext.c` | C | 238+198+76+76+275 | `task_threads`, `host_statistics`, IOKit, sysctl, mtime |
| Host interface headers | `third_party/d2r_gfx/host/{dm,front,hle}.h` | C | 29 | `front_metal_layer()` |

### 1d. Tooling / build

| Component | Files | Lang | LOC | Platform-specific |
|---|---|---|---|---|
| Shader translator service | `tools/totk_xlate/*.cs` + 5 Ryujinx patches; prebuilt `prebuilt/macos-arm64/totk_xlate` | C# (.NET NativeAOT) | 769 | MSL output + MSL-derived reflection; NativeAOT can target win-arm64 / win-x64 |
| Build orchestrator | `build_app.py` (top-level) | Python | 1350 | `xcrun clang`, `codesign`, `Info.plist`, `lsregister`, metal compiler (`step_shader_libs`, `step_warm_metal`), `clonefile`, Homebrew paths; container/crypto/hactool/RomFS steps are portable |
| Runtime build | `Makefile` | make | 426 | `xcrun clang`, `-framework`, `/opt/homebrew` libvpx/libopus |
| Data prep scripts | `scripts/*.py` (prepare_runtime, prepare_modules, prepare_shader_seed, make_romfs_patches ...) | Python | 6193 | mostly portable; shader seed emits MSL |
| Metal cache warmer | `tools/warm_metal_cache.m` | Obj-C | 134 | Metal |

Totals: runtime C/H 10.2k, runtime Obj-C 1.2k, runtime asm 0.25k, gfx C/H 7.7k, gfx Obj-C 15.5k, gfx asm 6.9k
(generated), tex 1.5k. Roughly **18k LOC portable C**, **17k LOC Apple-only Obj-C** (of which 14k is the Metal
backend), **7k LOC asm** needing a syntax port.

---

## 2. Windows replacement per component

Difficulty: S (days), M (1-3 weeks), L (1-2 months), XL (multi-month).

| Component | Windows replacement | Difficulty | Notes |
|---|---|---|---|
| **Rewriter TSD primitive** (`patch_nso.py`, `gate.S`, `hle_stack.S`, `loader.c` TSD check) | Replace `mrs xT, tpidrro_el0` with `mov xT, x18` (x18 = TEB on Windows ARM64, documented and never touched by the rewritten guest). Slots live in `TEB->TlsSlots` (offset `0x1480`, index from `TlsAlloc`, must be < 64); the loader writes `0x1480 + 8*index` into the key page. Same instruction count, so the patch-area layout is unchanged. | M | New `expected-v*-win.json` manifests; re-run `structck.py` + `equiv.py` for every version. This is the only change to guest code. |
| asm syntax (`gate.S`, `hle_stack.S`, `thread_enter.S`, `nvn_entry.S`, `nvn_thunks.S`) | GNU-as COFF syntax: drop `_` prefix, `@PAGE`/`@PAGEOFF` -> `sym` / `:lo12:sym`; a `PAGE()/PAGEOFF()` macro header or a sed transform. `nvn_thunks.S` is generated: transform mechanically (generator not vendored). Add `.seh_*`-free leaf code; mark as non-unwindable. | S | Toolchain: clang targeting `aarch64-pc-windows-msvc` (clang-cl + lld-link) or llvm-mingw `aarch64-w64-mingw32`. |
| Loader mapping | `VirtualAlloc2(MEM_RESERVE)` at the original layout, copy segments, `VirtualProtect` RX/R/RW, `FlushInstructionCache`. Drop the Mach-O section embedding path (iOS only). | S | 4 KiB pages on Windows ARM64 (finer than macOS 16 KiB: Horizon-native). Reservations are 64 KiB-granular. |
| Safe reads (`mach_vm_read_overwrite`) | `ReadProcessMemory(GetCurrentProcess(),...)` or a VEH-guarded memcpy | S | |
| Signals (crash, SIGILL CTR_EL0, dirty faults, fault guards) | `AddVectoredExceptionHandler` (first) on `EXCEPTION_ACCESS_VIOLATION` / `EXCEPTION_ILLEGAL_INSTRUCTION`; `CONTEXT.Pc/X[]` replaces `ucontext __ss`. `sigsetjmp/siglongjmp` guards -> VEH rewrites `Pc/Sp` to a recovery trampoline (do not longjmp: Windows longjmp unwinds via unwind data that guest frames lack). | M | Verify whether `mrs CTR_EL0` traps at all on Windows (likely allowed; keep emulation path). |
| Memory layout below 2^39 (`services.c`) | `VirtualAlloc2` with `MEM_RESERVE` at fixed addresses; commit/decommit = `VirtualAlloc(MEM_COMMIT)` / `VirtualFree(MEM_DECOMMIT)`; `mprotect` -> `VirtualProtect`. Windows leaves `0x1_0000_0000..2^39` essentially free (no shared cache), so the fixed layout should always fit. | S-M | Per-4K bookkeeping already exists; the 16K rounding can become 4K. |
| vpool aliasing (`totk_vpool.c`) | Back the aliased storage with a pagefile section: `CreateFileMapping2(SEC_RESERVE)` + `MapViewOfFile3` into `MEM_RESERVE_PLACEHOLDER` ranges; the virtual pool maps the same section offsets again. | **L** | Views need 64 KiB aligned addresses/offsets (vpool pages are 64 KiB: good) but CPU-visible physical pools live in game heap memory with 4-16 KiB alignment, so either the whole heap/alias region becomes section-backed (and decommit becomes "zero + NOACCESS", sections cannot be decommitted inside a view) or CPU-visible pools are aliased GPU-side only. Decide with `probe-vpool` traces. |
| Threads / sync (`threads.c` and every `pthread_mutex`) | Thin `host_thread.h`: SRWLOCK + CONDITION_VARIABLE, `CreateThread`, `TlsAlloc`, `SetThreadDescription`, `SetThreadPriority`/QoS via `SetThreadInformation(ThreadPowerThrottling)`; or keep pthreads via llvm-mingw winpthreads for a first build. Mach thread suspend/state -> `SuspendThread`/`GetThreadContext`; BotW async preemption -> suspend + `SetThreadContext` redirect. | M | When switching stacks (guest stack, host stack) update `NT_TIB.StackBase/StackLimit` (via x18) so exception dispatch and `__chkstk` see a valid stack. |
| Time (`mach_absolute_time`, timebase, continuous time) | `QueryPerformanceCounter`/`QueryPerformanceFrequency`, `QueryUnbiasedInterruptTimePrecise` | S | Also `nvn_entry.S` reads `mach_absolute_time`: call a host wrapper or read `cntvct_el0` directly (readable in EL0 on Windows ARM64). |
| File system, RomFS, saves | Win32 `CreateFileW` / `CreateFileMapping` for the read-only RomFS image, UTF-8<->UTF-16 paths, `%LOCALAPPDATA%\MacNativeGames\TOTK\{saves,shader-cache,mods,logs}`; `LockFileEx` for the game slot | M | Horizon paths are case-sensitive; NTFS is not. RomFS is served from its index (fine); saves/mods need care. |
| Audio output (`audio_mixer.c`) | WASAPI shared-mode, event-driven (`IAudioClient3` for low latency), or SDL3 audio streams | M | Renderer/effects/reverb are portable C. Handle device change (`IMMNotificationClient`) where `runtime_audio_output_restart` is used today. |
| Opus, VP9 | libopus, libvpx built with clang for win-arm64 / win-x64 (vcpkg has both) | S | |
| Window, keyboard, focus, text entry (`frontend.m`) | SDL3 (window, keyboard, IME text input, display/HiDPI, Vulkan surface) or raw Win32 (`CreateWindowExW`, `WM_INPUT`, `WM_CHAR`, `ImmGetContext`) | M | Keep the existing `runtime_front_*` function set as the interface; iOS already proves the seam. |
| Controllers (`gamepad.m`) | SDL3 gamepad (XInput, GameInput, DualSense/Switch Pro via HIDAPI, gyro/accel sensors) | M | SDL3 also gives motion for `motion_fusion.c`. |
| Rumble/triggers (`gamepad_feedback.m`) | SDL3 `SDL_RumbleGamepad`, `SDL_RumbleGamepadTriggers`, `SDL_SendGamepadEffect` (DualSense raw effects) | M | Replaces CoreHaptics + IOKit HID. |
| Launcher (`app/launcher.m`) | Small Win32/C launcher exe (sets profile/cache/log dirs, version check `--check`, single-instance), or fold into the game exe | S | |
| Sysload / overlays / diagnostics | `GetProcessTimes`, `NtQuerySystemInformation` / PDH, `CreateToolhelp32Snapshot` for thread lists, `GlobalMemoryStatusEx` | S | Diagnostics can be stubbed first. |
| **NVN backend (`nvn_mtl.m` & friends, ~15.5k LOC)** | **Vulkan 1.3** backend written in C/C++ behind the existing `mtl_*` seam (rename to `gpu_*`): dynamic rendering, descriptor buffers or push descriptors, timeline semaphores (NVN sync/fences), `VK_EXT_external_memory_host` for no-copy pool buffers (fallback: staging copies driven by dirty tracking), extended dynamic state | **XL** | Vulkan is the natural target: the translator already emits SPIR-V (SPIRV-Cross/MSL step disappears) and Ryujinx's own Vulkan backend is a working reference for Maxwell->Vulkan semantics. D3D12 would need SPIR-V->DXIL (spirv-to-dxil) and buys only driver robustness on Qualcomm; keep it as a fallback, not Phase 1. |
| MetalFX upscale/interp, TAA | FSR 1 (EASU/RCAS, already in game shaders path) then FSR 2/3 (MIT, Vulkan); interpolation optional/off initially | M | |
| Shader runtime (`totk_shader_rt.m`) | `CreateProcessW` with pipes for `totk_xlate --service`; cache under `%LOCALAPPDATA%` | S-M | |
| Translator (`tools/totk_xlate`) | Add `--spirv` service mode: output SPIR-V + **binding reflection from Ryujinx's `ShaderProgramInfo`** (not MSL regex); NativeAOT publish for `win-arm64` and `win-x64` | M | Patch 0005 (vertex output defaults) and the vertex-counter lowerings stay valid since they act before SPIR-V. |
| Texture formats / deswizzle (`tex/*`) | Map NVN formats to `VkFormat`; port GPU deswizzle and BC/ASTC helper kernels to GLSL/HLSL compute (compiled to SPIR-V offline) | M | **ASTC**: Adreno (Snapdragon X) supports ASTC LDR natively; NVIDIA/AMD/Intel desktop do not -> Phase 2 needs a compute ASTC decoder (Ryujinx has one) or offline transcode. |
| Build (`Makefile`, `build_app.py`) | CMake + clang (or a `build_windows_native.py` reusing `build_app.py`'s container/crypto/RomFS steps); hactool builds with clang/mingw; drop codesign/Info.plist/lsregister/metal steps; output a folder with `TOTK.exe`, data, shader seed (SPIR-V) | M | Keep step 1-2 (XCI/NSP, ExeFS, RomFS, fonts, overlays) shared with macOS: they are plain Python. |

---

## 3. The central decision: Windows on ARM64 first, x86-64 second

### Why Windows ARM64 can do what the Mac does

Windows 11 on ARM64 (Snapdragon X, and future ARM64 PCs) executes AArch64 user code natively, exactly like Apple
silicon. Everything that makes the Mac port a *port* rather than an emulator carries over:

- **Same instruction set, same calling convention.** Windows ARM64's ABI is AAPCS64 with x18 reserved (as on Darwin).
  Callee-saved registers (x19-x28, d8-d15), struct return in x8 and 8-byte stack argument slots match Horizon even
  better than Darwin does (Darwin packs stack arguments). Host C functions can keep being called directly by guest
  code.
- **The rewriter already removes everything the OS reserves.** x18 (TEB on Windows, TSD-irrelevant), `tpidr_el0`,
  `tpidrro_el0`, `svc`, `cntpct` are all redirected. The only Darwin assumption baked into the rewritten bytes is
  *how the stubs find their thread slots* (`mrs tpidrro_el0`). On Windows the TEB is in x18, which the guest no
  longer touches, so `mov xT, x18` + `TlsSlots` offset is a one-instruction substitution. The equivalence-checking
  infrastructure (`equiv.py`, `structck.py`, `make verify`) can re-prove it per version.
- **Shared address space with fixed low placement.** The 2^39 constraint is easier on Windows (no dyld shared cache
  in the low 64 GiB, 47-bit user VA). Guest pointer == host pointer remains true, so all ~18k LOC of portable service
  and NVN C code keeps working unchanged in semantics.
- **No JIT, no W^X dance.** Code is copied once and made RX; Windows allows that without `MAP_JIT` equivalents
  (only ACG/"dynamic code prohibited" policies would block it, and they are opt-in).
- **4 KiB host pages** match Horizon's page size, removing the 16 KiB rounding workarounds in `services.c`.
- **Unified memory GPU** (Adreno X1) matches the Mac's no-copy buffer design, and **native ASTC** support matches
  Apple GPUs.

### Why x86-64 needs a CPU translator, and why it should come second

On x86-64 the game's AArch64 code cannot execute. Options:

1. **dynarmic** (A64 JIT used by yuzu/suyu/eden; 0BSD/MIT-style licensed fork lineage, check exact license of the
   chosen fork) embedded in the same process.
2. Ryujinx's ARMeilleure (C#): wrong language/runtime for this codebase.
3. A custom static AOT translator of the three modules: huge effort; self-modifying/indirect branches still need a JIT.

dynarmic is the realistic choice, but it changes the model in ways the Mac code does not have to deal with:

- **Run the original (unrewritten) NSOs** under dynarmic: it natively handles `svc` (callback), `tpidr_el0`/
  `tpidrro_el0` (per-JIT fields), `cntpct` (callback), exclusives (global `ExclusiveMonitor`). The rewriter is not
  needed on x64; the fixed game patches (`game_patches*.c`) still apply as guest-memory writes plus JIT cache
  invalidation.
- **Identity-mapped fastmem.** Keep guest memory at the same host VA below 2^39 and give dynarmic a fastmem base of 0
  (with a page table or fastmem arena) so host C code keeps dereferencing guest pointers directly. This preserves the
  "shared address space" design and lets hle_nvn.c, services and audio code run unchanged.
- **Host<->guest calls become marshalled.** Today guest code `blr`s straight into host C (`runtime_override`, ~1050
  NVN thunk slots, services overrides) and host code calls guest functions (`nnMain`, callbacks, thread entries).
  Under dynarmic, host functions must live at trap addresses (e.g. an `svc #imm` or unmapped-page trampoline per
  override) with generated AAPCS64 -> Win64 marshalling. `nvn_protos.h` (577 LOC of prototypes) makes the NVN side
  generatable; varargs and by-value structs need hand review. Host->guest calls need a re-entrant `Jit::Run` with a
  return-sentinel address.
- **Threads.** 1:1 guest threads would mean one JIT instance per thread (TOTK creates dozens): use dynarmic with a
  shared code cache per core and keep the `threads.c` core-ownership scheduler (it already models 4 cores), mapping
  each core to a JIT context, or accept per-thread JITs with a shared IR cache. This is a real design task.
- **Fault tricks** (dirty tracking via write faults on guest memory) still work with fastmem because faults occur
  in JIT-emitted code: dynarmic's fastmem fault handler must forward non-fastmem faults to `nvn_dirty`.
- **GPU:** desktop dGPUs need ASTC decode and lack UMA, so the no-copy buffer path must fall back to upload copies.
- **Performance:** expect a 2-4x CPU cost versus native; TOTK is CPU-heavy. Feasible on modern desktop CPUs
  (yuzu-class results), but it is a separate tuning project.

### Recommendation

- **Phase 1 = Windows ARM64 native + portability layer.** Produces a true native port with the least risk and the
  same architecture as macOS. It also forces the portability work (memory, threads, exceptions, file system,
  windowing, input, audio, Vulkan NVN backend, SPIR-V shader path) that Phase 2 reuses 100%.
- **Phase 2 = x86-64 via dynarmic.** Adds a CPU backend behind a `cpu_backend.h` interface (native-rewritten vs JIT),
  the host-call marshalling generator, ASTC decode and a discrete-GPU memory path. Everything above the CPU backend
  (services, NVN layer, Vulkan backend, frontend) is shared.

Justification from the code: ~80% of the code (services, NVN layer, audio renderer, input logic) is portable C that
assumes direct pointer access and direct AAPCS64 calls; Windows ARM64 preserves both, x86-64 preserves neither without
new machinery. The only guest-code change needed for ARM64 is one instruction pattern in a rewriter that already has
a verification harness.

---

## 4. Work breakdown (ordered, parallelizable chunks)

Each chunk lists its interface contract so agents can work independently. Chunks in the same wave can run in
parallel. "Mac must keep building" applies to every chunk that touches shared files: use `#ifdef _WIN32` or a
separate `*_win.c` file, never break the macOS build.

### Wave 0: foundations (sequential, small, unblock everyone)

**C0. Platform abstraction headers (`runtime/platform/`)**
- Deliver `host_mem.h` (reserve_at, commit, decommit, protect, flush_icache, page_size, alloc_granularity,
  section_create/map_view for aliasing), `host_thread.h` (thread create/join, mutex, cond, once, tls_alloc/get/set,
  thread id, set name/priority, suspend/get_context/set_context), `host_time.h` (now_ns, monotonic_continuous_ns,
  counter freq), `host_fault.h` (install handler: `int (*)(host_fault_info*)`, with pc/sp/regs[31]/fault addr/kind;
  guarded-read API replacing `runtime_fault_guard`), `host_fs.h` (open/read/pread/write/mmap_ro/lock, UTF-8 paths,
  user dirs: saves, cache, mods, logs).
- macOS implementation = thin wrappers over today's calls; Windows implementation = stubs that compile.
- Interface owner for all later chunks.

**C1. Toolchain + build skeleton for Windows ARM64**
- CMake (or `build_win.py`) with clang `--target=aarch64-pc-windows-msvc`, lld-link; vcpkg triplets `arm64-windows`
  for libvpx, libopus, SDL3, Vulkan headers/loader, volk, VMA.
- Must compile the portable C subset with stubs. Output: `TOTK.exe`.

### Wave 1: independent ports (parallel)

**C2. Rewriter Windows variant** (`third_party/d2r/patch_nso.py`, `structck.py`, `equiv.py`)
- Add `--abi windows`: stub primitive `mov xT, x18` instead of `mrs xT, tpidrro_el0`; key page values written by loader
  as `0x1480 + 8*tls_index`.
- Generate and verify `assets/expected-v{1.0.0,1.1.2,1.2.1,1.4.3}-win.json`; equivalence model must treat x18 as TEB.
- Interface: same `.patched.bin` / `.fixups.tsv` / `.gates.tsv` formats. Depends on nothing but C0 conventions.

**C3. Gate/stack/thread asm for Windows** (`gate.S`, `hle_stack.S`, `thread_enter.S`, `nvn_entry.S`, `nvn_thunks.S`)
- COFF syntax, TEB-based slot access (`ldr xN, [x18, #0x1480 + idx*8]` with idx loaded from a global),
  update `NT_TIB.StackBase/StackLimit` on every stack switch, restore on return.
- Script `scripts/asm_to_coff.py` for the generated thunk file.
- Interface: unchanged C symbols (`runtime_gate`, `runtime_dispatch`, `dm_thread_enter`, `runtime_thread_leave`,
  `nvnth_*`, `nvn_slot`).

**C4. Memory + loader on Windows** (`loader.c`, `services.c`, `memory_probe.c`)
- Fixed reservation below 2^39 via `host_mem`; commit/decommit/permissions; module mapping with copy + RX;
  `runtime_module_permission`; replace `mach_vm_read_overwrite`.
- Probe: `TOTK.exe <data> probe` / `probe-memory` / `probe-loader` pass on Windows ARM64.
- Depends on C0; consumes C2 output and C3 symbols for the full probe.

**C5. Exceptions/faults** (`loader.c` crash path, `nvn_dirty.c`, `gfx_bridge.c`, guards in `hle_nvn.c`/backend)
- VEH implementation of `host_fault.h`; recovery-trampoline guarded reads; CTR_EL0 emulation if it traps.
- Must be async-safe like today (atomics + VirtualProtect only).
- Probe: port `tests/dirty_race/probe.c`.

**C6. Threads/sync/time** (`threads.c`, `time_host.c`, `audio_renderer.c` QoS, all `pthread_*` users)
- Move to `host_thread.h`; replace Mach suspend/state (diagnostics, BotW preemption) with Suspend/GetThreadContext.
- Probe: `scripts/thread_probe.c` (4-thread / 4000-call loader probe) green on Windows.

**C7. File system & paths** (`fs_host.c`, `romfs.c`, `aoc_romfs.c`, `game_slot.c`, `shared_font.c`, mods)
- `host_fs.h`; `%LOCALAPPDATA%` layout; case-insensitivity audit for saves/mods; RomFS verification record keyed by
  NTFS file ID + mtime.
- Probes: `probe-fs`, `probe-save`, RomFS probe (65-file independent read).

**C8. Audio output** (`audio_mixer.c` backend split: `audio_out_coreaudio.c` / `audio_out_wasapi.c`)
- Interface: `audio_out_open(rate, channels, pull_cb)`, `audio_out_restart(recreate)`, `audio_out_close()`.
- Probe: `probe-audio`, `tests/audio_effects`, `tests/reverb` (latter two are already portable).

**C9. Frontend + input** (`frontend_win.c`, `gamepad_sdl.c`, `gamepad_feedback_sdl.c`)
- Implement the existing `runtime_front_*`, `runtime_gamepad_*`, `runtime_hid_feedback_*` surface with SDL3.
- New: `front_vk_surface(VkInstance)` replacing `front_metal_layer()` in `host/front.h`.
- Probes: `scripts/input_probe.c`, `autoload_probe.c`, `tests/gamepad_layout`.

**C10. Shader translator SPIR-V mode** (`tools/totk_xlate`)
- `--service --target spirv`: write `<variant>.spv` + `<variant>.json` reflection from `ShaderProgramInfo`
  (set/binding per constant buffer, storage buffer, texture, image; rw flags; vertex attributes; fragment outputs).
- NativeAOT `win-arm64` and `win-x64` binaries with SHA-256 pins; extend `prepare_shader_seed.py` to emit SPIR-V seeds.
- Interface: JSON schema document consumed by C11; request protocol unchanged.

**C11. Vulkan NVN backend** (`gfx/nvn/nvn_vk.c` + `nvn_vk_*.c`, replacing `nvn_mtl.m`, `nvn_refl.m`, `totk_mfx*.m`, `totk_taa.m`)
- Implement the `mtl_*` seam in `nvn_mtl.h` (rename to `gpu_*`): pools (`gpu_pool_add/del`, `gpu_addr`), submission
  (`gpu_sub_begin/copy/event/counter/commit`), fences, textures, render passes, draws, compute, present.
- Sub-chunks (parallel within C11 once the skeleton exists):
  - C11a device/swapchain/present/frame pacing (`nvn_pace.h`), memory: pool import via `VK_EXT_external_memory_host`
    with staging fallback, VMA for host-owned resources.
  - C11b textures: `nvn_fmt_vk.c`, deswizzle (CPU path is portable C; GPU path to compute SPIR-V), views, samplers,
    BC/ASTC paths.
  - C11c pipelines: PSO key (`nvn_pso_skey.h`), pipeline cache, descriptor model from C10 reflection, dynamic state.
  - C11d command replay: render targets/passes (`nvn_render_stream.h`, `nvn_cube_group.h`, `nvn_rtex.inc`), clears
    (including the D16 clear rule from `nvn_depth_clear.h`), draws, queries/counters, barriers incl. the aliased-copy
    visibility rule (docs/how-it-works.md "Buffer copies through aliased memory").
  - C11e upscaling: FSR1 first, then FSR2/3; fps overlay.
- Port the GPU probes (`probe-gpu-*`, `tests/depth16_clear`, `tests/texture_auto_layout`, `tests/vertex_i2f`) as the
  acceptance suite: each probe is game-data-free and compares to a CPU reference.
- Depends on C0, C9 (surface), C10 (SPIR-V + reflection). Can start immediately against probes.

**C12. vpool aliasing on Windows** (`totk_vpool.c`, possibly `services.c` heap backing)
- Measure which pools need CPU-visible aliasing (instrument on macOS first), then implement section-backed views or a
  GPU-only alias. Interface: `totk_vpool.h` unchanged.

**C13. Build orchestrator for Windows native** (`build_windows_native.py` or `build_app.py --target windows-arm64`)
- Reuse container/crypto/RomFS/font/overlay steps; build hactool with clang; drop codesign/plist/lsregister/metal;
  package `TOTK\` folder (exe, data, SPIR-V seed, translator); `--check`; shortcut creation as in `build_windows.py`.
- Keep the existing emulator-based `build_windows.py` until the native path reaches parity, then retire it.

### Wave 2: integration (sequential)

**C14. Boot bring-up**: `nn::init::Start` to title screen headless (`TOTK_HEADLESS`), then with Vulkan.
  - Status (x86-64, dynarmic, 1.4.3): `totk-win.exe` (CMake target, `TOTK_DATA_DIR` = imported `extracted/v143`) maps the
    original rtld/main/subsdk0/sdk NSOs at 0x8_0000_0000 (`runtime/loader_win.c`), links them on the host with overrides as
    host traps (`runtime/jit_glue.c`), applies the 1.4.3 game patches and boots through nnMain into the game's main loop:
    the NVN layer (`hle_nvn.c`) runs under the JIT with the null `mtl_*` backend (`gfx/nvn/nvn_null.c`: no drawing, but
    events, counters, buffer copies and uniform updates happen and fences complete at once), the NVN entry traps take
    their marshalling from `nvn_protos.h` (`tools/windows/gen_nvn_traps.py` -> `nvn_jit_sigs.h`, `gfx/nvn/nvn_jit.c`),
    virtual memory pools alias through sections (`gfx/nvn/totk_vpool_win.c`), libopus and libvpx are built from source.
    Headless it reaches the title screen (opening-field scene, 54 guest threads, audio renderer
    running, ~45 presents a second) and, with scripted A presses (TOTK_INPUT_SCRIPT), starts a new game into the prologue
    (Zelda cave walk: stable for 10 min, waiting for movement input). Shared sources build through `runtime/win/compat` (shadowed Apple headers) +
    `runtime/win/win_posix.h`; registration sections use `host/dm_section.h`. Guest data aborts are exact
    (`precise_aborts`): the loader.c recoveries (#57, menu reference) and dirty-page faults run on them.
  - Next: the Vulkan backend in place of `nvn_null.c` (same seam); shared fonts (firmware import); performance.
**C15. Gameplay parity**: game patches (frame step, 60 FPS, output res, aspect), movie, swkbd, applets, saves upgrade
backup, mods; soak test scripts (`scripts/mac_v143_soak.py` -> generic).
**C16. Performance**: pipeline warm-up from the seed, async compile, dirty-tracking cost, Adreno-specific tuning.

### Wave 3: x86-64 (Phase 2)

**C17. CPU backend interface**: `cpu_backend.h` (`create_thread_context`, `run`, `call_guest(fn,args)`,
`register_host_trap(addr, fn, signature)`, `invalidate(range)`); native-ARM64 implementation wraps today's model.
**C18. dynarmic integration** (C++): A64 JIT, fastmem at base 0 over the same reservation, svc/cntpct/tpidr callbacks
routed to `runtime_svc`, global exclusive monitor, per-core JIT contexts tied to `threads.c` core ownership.
**C19. Host-call marshalling generator**: from `nvn_protos.h` and override tables produce trap stubs + AAPCS64 ->
Win64 shims; host->guest calls (thread entry, `nnMain`, callbacks).
**C20. Desktop GPU path**: ASTC compute decode, non-UMA pool upload path (staging + dirty ranges), vendor testing.

### Dependency summary

```
C0 -> C1
C0 -> {C2, C3, C4, C5, C6, C7, C8, C9, C10, C12}   (parallel)
C2 + C3 + C4 + C5 + C6 + C7 -> C14 (headless boot)
C9 + C10 -> C11 -> C14 (rendered boot)
C8, C12, C13 -> C15 -> C16
C14..C16 -> C17 -> {C18, C19, C20} (parallel) -> x64 release
```

---

## 5. Risks and unknowns

1. **Red zone use in rewrite stubs.** Stubs store below SP (`STUR tmp,[sp,#-16]`; jump-table windows touch
   `[sp,#-24]`). Darwin guarantees a 128-byte red zone; Windows ARM64 does not document one. Anything that writes the
   user stack asynchronously (exception dispatch from a fault inside a stub window, debugger, APC at an alertable
   wait) could clobber it. Faults inside these windows are unlikely (they touch only TEB and the key page), but this
   must be proven with a stress probe early, or the rewriter changed to pre-decrement SP (layout change, re-verify).
2. **Stack bounds in the TEB.** Guest stacks and host-call stacks are not the thread's Windows stack. Exception
   dispatch, `__chkstk` and stack-overflow handling consult `NT_TIB.StackBase/StackLimit`; if they are not updated on
   every switch, Windows may fail-fast with `STATUS_BAD_STACK` or mis-handle guard pages.
3. **No unwind data for guest/asm frames.** Any C++ exception, `longjmp` or SEH unwind across guest frames will fail.
   Policy: no exceptions/longjmp across the gate; recovery via VEH context rewrite only.
4. **Qualcomm Adreno Vulkan driver quality** (and availability of `VK_EXT_external_memory_host`,
   `descriptor_buffer`, extended dynamic state, ASTC, BC formats, geometry/tessellation features TOTK uses). Mitigation:
   capability probe at start-up; staging fallback for pool buffers; D3D12 backend (via spirv-to-dxil) as contingency.
5. **vpool aliasing cost** on Windows (section-backed memory, 64 KiB view granularity, no decommit in views). Unknown
   how much CPU-visible aliasing TOTK actually performs.
6. **Re-verification burden.** Every supported version (1.0.0, 1.1.2, 1.2.1, 1.4.3) needs new Windows rewrite
   manifests and a full `equiv.py` run; the D2R-pinned tool fork must accept an ABI switch.
7. **CTR_EL0 / DC ZVA / counter access** semantics on Windows ARM64 (cache-line sizes differ by SoC; `dc zva` block
   size from `DCZID_EL0`). The game's `cntpct` reads are already gated; `cntvct` frequency differs (19.2 MHz on both
   Switch and Snapdragon, 24 MHz on Apple) but the gate already converts.
8. **Metal-specific fixes baked into the backend** (D16 clear rounding, alias visibility, BC1->ASTC, vertex-counter
   lowering, EASU admission) must be re-derived for Vulkan/Adreno rather than copied; the existing probes are the
   specification.
9. **Shader reflection change.** Today reflection is regex-derived from MSL; the SPIR-V path needs a new, verified
   reflection producer, and the startup seed must be regenerated (cache chain name bump).
10. **Case-insensitive file system and path length** (`\\?\` prefix for >260 chars) for saves/mods/cache.
11. **Anti-malware / SmartScreen heuristics** for an unsigned exe that maps RX memory at fixed low addresses and
    installs VEH. Consider code signing for distribution.
12. **Phase 2 unknowns:** dynarmic fork/licensing choice, JIT memory footprint with dozens of guest threads,
    marshalling of varargs/by-value structs, CPU performance on mid-range x86 CPUs, ASTC decode bandwidth on dGPUs.
13. **Legal posture unchanged:** like the Mac build, the Windows build must use only the player's own game files and
    keys and must not distribute game code, keys or firmware.
