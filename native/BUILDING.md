# Building the native Color Grade AE effect (Windows)

This is the repo-committed home for the native build knowledge - the pinned toolchain,
the environment variables, and the folded-in Phase 0 checklist with its corrections.
It supersedes firstmate's `native-phase0-*.md` notes as the source of truth for the build.

The Phase 1 deliverable is `ColorGradeFX.aex`: a SmartFX effect that applies a baked 3D
LUT to a layer through the ported trilinear `sampleLut`, with a CPU path and a GPU path
(DirectX + CUDA). See `../CLAUDE.md`/`AGENTS.md` for how the native tree fits the wider project.

---

## TL;DR - build from WSL, unattended

```bash
npm run native:gen-lut      # (re)generate the embedded default LUT header (committed)
npm run native:parity       # C++ sampleLut vs TS oracle, ~1e-4 (needs g++/clang; not in CI)

native/scripts/build.sh Debug            # CPU-only  .aex -> MediaCore
native/scripts/build.sh Debug --gpu      # CPU + GPU (DirectX + CUDA) .aex + DirectX_Assets
native/scripts/build.sh Release --gpu    # optimized
```

`build.sh` mirrors `native/` to an NTFS scratch dir (`C:\dev\cg-native-build`) because
MSBuild against a `\\wsl$` path is slow/broken, then invokes VS 2022 `MSBuild.exe` over
Windows interop. The worktree stays the source of truth; the mirror is disposable. The
linker drops the `.aex` (and, for `--gpu`, the `DirectX_Assets/` folder) into
`AE_PLUGIN_BUILD_DIR` (MediaCore) - the only sanctioned writes outside the worktree.

**After Effects must be closed before (re)building** - it holds the loaded `.aex` and the
linker cannot overwrite it (LNK1104).

---

## Pinned toolchain (captain-verified, do not drift)

| Component | Version / Location |
|---|---|
| **Build IDE** | **Visual Studio 2022** Community 17.14 (`D:\Microsoft\Microsoft Visual Studio\2022\Community`). **Not VS 2026** - see the CUDA note below. |
| Platform toolset | **v143** |
| MSBuild | `D:\Microsoft\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe` |
| Windows SDK | **10.0.26100.0** - pinned in the vcxproj as `WindowsTargetPlatformVersion`, never floating `10.0` |
| AE SDK | ae25.6_61.64bit at `C:\dev\AfterEffectsSDK_25.6_61_win\ae25.6_61.64bit.AfterEffectsSDK` (override with `/p:AE_SDK_PATH=...`) |
| After Effects | AE 2025 (25.x) - the compatibility floor; the `.aex` loads in 25 and newer |
| DXC (DirectX Shader Compiler) | `D:\dxc_2026_05_27` - GPU builds only |
| Boost | `D:\Boost\include\boost-1_91` - GPU builds only (headers pulled by the kernel preprocessor) |
| Python | 3.x on PATH as `python` (not just `py`) - GPU builds only (kernel preprocessing) |
| CUDA Toolkit | 12.8 at `D:\NVIDIA GPU Computing Toolkit\CUDA\v12.8` - **used by the GPU build** (nvcc compiles the CUDA kernel). CUDA is the framework AE actually offers on the NVIDIA dev box (see the DirectX-unavailable note below). |

All plugin versions load from the stable `...\Adobe\Common\Plug-ins\7.0\MediaCore\` folder
("7.0" is locked across CC versions); one binary serves every host version.

### Environment variables (set as Windows **user** variables)

| Variable | Value | Needed for |
|---|---|---|
| `AE_PLUGIN_BUILD_DIR` | `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore` | always (build output) |
| `DXC_SDK_BASE_PATH` | `D:\dxc_2026_05_27` (folder **containing** `bin\x64\dxc.exe`) | GPU build |
| `Boost_BASE_PATH` | `D:\Boost\include\boost-1_91` (the versioned folder; must be **space-free**) | GPU build |
| `CUDA_SDK_BASE_PATH` | `D:\NVIDIA GPU Computing Toolkit\CUDA\v12.8` (version root, no trailing `\`) | GPU build (nvcc + cudart_static) |

Restart VS / the shell after changing any of these. `AE_PLUGIN_BUILD_DIR` lives under
Program Files (not user-writable by default): grant your account Modify+Write on that
folder once (Properties -> Security), or link errors show up as **LNK1104 "cannot open
file ...aex"**.

---

## Permissions & iteration loop

- LNK1104 at link time = either AE is open (close it) or you lack write permission on
  `AE_PLUGIN_BUILD_DIR` (grant Modify+Write once).
- Iterate: edit in the worktree -> `build.sh` -> launch AE -> (debug) VS *Attach to
  Process* -> `AfterFX.exe` -> breakpoint in `SmartRender`/`PreRender` -> scrub or change
  a param to force a re-render. Changing the `.aex` requires restarting AE.

---

## How the build is wired

- **One vcxproj, two modes.** `ColorGradeFX/Win/ColorGradeFX.vcxproj` builds CPU-only by
  default; `/p:CG_GPU=true` (from `build.sh --gpu`) builds **both** GPU frameworks:
  - DirectX (`HAS_HLSL=1`): compiles `DirectXUtils.cpp`, runs the HLSL kernel custom-build
    step (cl `/P` -> `ParseHLSL.py` -> DXC), links `d3d12.lib`/`d3dcompiler.lib`/`dxgi.lib`,
    flips the PiPL `Global_OutFlags_2` to the GPU value via a `HAS_HLSL` define, and copies
    `DirectX_Assets/` next to the `.aex` after linking.
  - CUDA (`HAS_CUDA=1`): compiles `ColorGrade_Kernel.cu` with **nvcc** (custom build step;
    `-arch=sm_50` PTX JITs onto the sm_86 3090; `-ccbin "$(VC_ExecutablePath_x64_x64)"`
    resolves to v143 under VS 2022) into a `.obj` that the linker picks up, and links
    `cudart_static.lib`. Because `cudart_static` pulls the static CRT, the GPU link ignores
    `libcmt.lib`/`libcmtd.lib` (`IgnoreSpecificDefaultLibraries`) so the dynamic CRT (`/MD[d]`)
    wins - this silences LNK4098 and avoids CRT mixing.

  AE selects the framework at runtime and calls the matching path; both are present so the
  effect can accelerate on whichever AE offers (DirectX host **or** the NVIDIA/CUDA dev box).
- **GPU flags must match in two places** or AE silently uses the CPU path:
  `PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING | PF_OutFlag2_SUPPORTS_GPU_RENDER_F32` in
  `GlobalSetup`'s `out_flags2` **and** in the PiPL's `AE_Effect_Global_OutFlags_2`
  (`0x2a001400`). The PiPL `.r` selects the value with `#ifdef HAS_HLSL`.
- **DirectX_Assets** (`ColorGradeKernel.cso` + `.rs`) must ship in a `DirectX_Assets/`
  folder next to the binary; AE resolves it by the module's own path. `build.sh` forces the
  PiPL + kernel custom-build steps to re-run each build (their up-to-date check is
  timestamp-based and would otherwise miss a CPU<->GPU flag flip).
- **Charset is MultiByte**, not Unicode: the SDK's `DirectXUtils.cpp` casts a
  `GetModuleHandleEx` string arg to `LPCSTR`; Unicode would break it. Our own code uses
  explicit `...W` Win32 APIs, so it is charset-agnostic.

---

## Phase 0 checklist (folded in, with the six corrections applied)

The original firstmate Phase 0 checklist had six documented errors. They are corrected
here so the build knowledge lives in the repo:

1. **`SDK_Invert_ProcAmp` is a GPU sample, not a CPU one** - it needs the full GPU
   dependency chain and defines `HAS_CUDA` unconditionally. For a first CPU-only sanity
   build use **Gamma_Table** instead (CPU-only and it visibly changes the image).
2. **Python is a required (undocumented) GPU build dependency** - the `.cl`/`.chlsl`
   custom build steps shell out to `python` (literally `python`, not `py`) via
   `CreateCString.py` / `ParseHLSL.py`. Build-time only; never shipped.
3. **`SDK_Invert_ProcAmp` requires CUDA unconditionally** (defines `HAS_CUDA` in every
   config). This plugin's CUDA is opt-in via `CG_GPU=true` (the CPU-only default needs no
   CUDA). **Update from the original plan:** DirectX was intended as the primary GPU path,
   but the dev box's AE 2025 exposes only Mercury CUDA/OpenCL/Software (no DirectX), so the
   CUDA kernel path was added and is the one that actually engages there. Both DirectX and
   CUDA are built by `--gpu`; OpenCL remains unwired (a cheap future add for AMD).
4. **Boost is not header-only in general** - but for our use it is only pulled by the
   kernel preprocessor's include path; no compiled Boost libs are linked.
5. **The AE SDK zip nests** as `AfterEffectsSDK_25.6_61_win\ae25.6_61.64bit.AfterEffectsSDK`,
   not a flat `C:\dev\AfterEffectsSDK`. `AE_SDK_PATH` points at the nested folder.
6. **VS 2026 is the wrong IDE for GPU work.** VS 2026's `$(VC_ExecutablePath_x64_x64)`
   resolves to its native v145 toolset regardless of the project's Platform Toolset, which
   crashes `cudafe++` (ACCESS_VIOLATION) for CUDA builds. **Use VS 2022.** (Not a factor
   for this plugin's DirectX-only build, but it is why the pinned IDE is VS 2022.)

Additional pins beyond the checklist:
- `WindowsTargetPlatformVersion` is pinned to `10.0.26100.0` (never floating `10.0`, which
  means "newest installed" and is a reproducibility hazard).
- If VS 2026 is ever uninstalled, verify `D:\Windows Kits\10\Include\10.0.26100.0` still
  exists (VS 2026's installer originally brought that SDK); re-add it via the VS 2022
  installer and re-pin if it is gone.

---

## Verifying the GPU path (CUDA/DirectX active vs CPU fallback)

AE silently uses the CPU path when it will not / cannot run the effect's GPU path, so a
correct-looking image does **not** prove DirectX ran. Two ways to tell them apart:

### A. Cheapest - no debugger: `OutputDebugString` trace (Debug builds)

`ColorGrade.cpp` / `ColorGradeGPU.inc` print one line per render on each path (compiled
only in Debug, via the `CG_DBG` macro; no-op in Release). Watch them live with
**Sysinternals DebugView** (run as admin, Capture -> Capture Win32) or in the Visual
Studio **Output** window while attached. Filter on `[ColorGradeFX]`. You will see:

| Message | Meaning |
|---|---|
| `[ColorGradeFX] GPUDeviceSetup: AE offered <FRAMEWORK> ...` | Which framework AE is offering (CUDA / OpenCL / Metal / DirectX). Fires once per device at setup and names AE's actual choice - the fastest way to see whether DirectX or CUDA is even in play on this box. |
| `[ColorGradeFX] GPUDeviceSetup: CUDA ready (kernel statically linked, GPU offered)` | The **CUDA** path set up successfully (the path that engages on the NVIDIA dev box). |
| `[ColorGradeFX] GPUDeviceSetup: DirectX device set up, ColorGradeKernel loaded (GPU offered)` | The **DirectX** path set up (shader loaded). |
| `[ColorGradeFX] SmartRenderGPU: CUDA GPU render path active` | **CUDA is actually rendering** frames. Confirmation on NVIDIA. |
| `[ColorGradeFX] SmartRenderGPU: DirectX GPU render path active` | **DirectX is actually rendering** frames. Confirmation on a DirectX host. |
| `[ColorGradeFX] SmartRenderCPU: CPU render path active` | The **CPU fallback** is rendering (no GPU path used for that frame). |

Trigger a render by scrubbing the playhead or changing Strength. Seeing a `SmartRenderGPU`
line = that GPU framework confirmed; seeing only `SmartRenderCPU` = fallback. On the NVIDIA
dev box (AE offers only CUDA/OpenCL/Software), expect the CUDA lines; DirectX lines would
only appear on a host/AE config that offers the DirectX framework.

### B. Breakpoints (VS *Attach to Process* -> `AfterFX.exe`)

Set breakpoints on these exact symbols (both in this project's source):

| Path | Symbol | File |
|---|---|---|
| **GPU render (CUDA or DirectX)** | `SmartRenderGPU` | `native/ColorGradeFX/ColorGradeGPU.inc` (branch on `what_gpu` inside) |
| **CPU render** | `SmartRenderCPU` | `native/ColorGradeFX/ColorGrade.cpp` |
| GPU device offered (once) | `GPUDeviceSetup` | `native/ColorGradeFX/ColorGradeGPU.inc` |

`SmartRenderGPU` handles both frameworks (a `#if HAS_CUDA` branch calling `ColorGrade_CUDA`
and a `#if HAS_HLSL` branch dispatching the DirectX shader) - the breakpoint hits for
whichever AE chose. `EffectMain`'s `PF_Cmd_SMART_RENDER_GPU` case dispatches to `SmartRenderGPU`; the
`PF_Cmd_SMART_RENDER` case dispatches to `SmartRenderCPU`. Whichever breakpoint hits on
a scrub is the path AE chose. (Requires a Debug build so the symbols resolve; the `.pdb`
sits next to the `.aex`'s intermediate in the NTFS mirror - point VS at the mirror source
or load the `.pdb` when prompted.)

Prerequisites for the GPU path to be eligible: a Debug **or** Release `--gpu` build, and
Project Settings -> Video Rendering and Effects -> Mercury GPU Acceleration set to a GPU
option. On the NVIDIA dev box that means **CUDA** (AE offers no DirectX there), so expect
the CUDA tracer lines; `DirectX_Assets/` next to the `.aex` matters only on a host that
offers DirectX. If `GPUDeviceSetup` logs a framework that is neither CUDA nor DirectX
(e.g. OpenCL), that framework is not built and the effect falls back to CPU - switch
Mercury to CUDA, or add the OpenCL path.

## Runtime verification (captain-assisted - cannot be automated here)

A build cannot prove the effect works in AE; that needs a human at the GUI. When a build
is ready, verify in AE 2025:

1. **Registers & drags:** AE launched, the effect appears under **Effects & Presets ->
   Color Grade -> "CG Color Grade"**, and drags onto a layer with **Strength** and
   **LUT Source** controls in Effect Controls.
2. **CPU LUT applies:** with LUT Source = "Embedded (Teal-Orange)", the layer takes on a
   teal-orange grade; **Strength** scrubs it from 0% (identity) to 100%.
3. **External .cube:** set `LUT Source` = "External .cube file" and either set env
   `CG_LUT_PATH` to a `.cube` **or** drop a `ColorGrade_LUT.cube` next to the `.aex` in
   MediaCore; the layer takes on that LUT. A bad/missing path falls back to the embedded LUT.
4. **GPU path active:** Project Settings -> Video Rendering and Effects -> Mercury GPU
   Acceleration on; confirm the GPU path renders (same result as CPU). On this NVIDIA box
   (RTX 3090 + AMD) AE offers only CUDA, so expect the CUDA tracer lines; DirectX would
   engage only on a host that offers it. If it falls back to CPU, adapter/framework
   selection is the suspect - check the `GPUDeviceSetup` trace for which framework AE offered.
