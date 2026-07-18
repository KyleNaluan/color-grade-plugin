# Building the native Color Grade AE effect (Windows)

This is the repo-committed home for the native build knowledge - the pinned toolchain,
the environment variables, and the folded-in Phase 0 checklist with its corrections.
It supersedes firstmate's `native-phase0-*.md` notes as the source of truth for the build.

The Phase 1 deliverable is `ColorGradeFX.aex`: a SmartFX effect that applies a baked 3D
LUT to a layer through the ported trilinear `sampleLut`, with a CPU path and a DirectX
GPU path. See `../CLAUDE.md`/`AGENTS.md` for how the native tree fits the wider project.

---

## TL;DR - build from WSL, unattended

```bash
npm run native:gen-lut      # (re)generate the embedded default LUT header (committed)
npm run native:parity       # C++ sampleLut vs TS oracle, ~1e-4 (needs g++/clang; not in CI)

native/scripts/build.sh Debug            # CPU-only  .aex -> MediaCore
native/scripts/build.sh Debug --gpu      # CPU + DirectX GPU .aex + DirectX_Assets
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
| CUDA Toolkit | 12.8 - **not used by this plugin** (DirectX is the primary GPU path; CUDA is optional polish) |

All plugin versions load from the stable `...\Adobe\Common\Plug-ins\7.0\MediaCore\` folder
("7.0" is locked across CC versions); one binary serves every host version.

### Environment variables (set as Windows **user** variables)

| Variable | Value | Needed for |
|---|---|---|
| `AE_PLUGIN_BUILD_DIR` | `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore` | always (build output) |
| `DXC_SDK_BASE_PATH` | `D:\dxc_2026_05_27` (folder **containing** `bin\x64\dxc.exe`) | GPU build |
| `Boost_BASE_PATH` | `D:\Boost\include\boost-1_91` (the versioned folder; must be **space-free**) | GPU build |
| `CUDA_SDK_BASE_PATH` | `D:\NVIDIA GPU Computing Toolkit\CUDA\v12.8` | only if a CUDA path is added later |

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
  default; `/p:CG_GPU=true` (from `build.sh --gpu`) adds `HAS_HLSL=1`, compiles
  `DirectXUtils.cpp`, runs the HLSL kernel custom-build step, links `d3d12.lib` /
  `d3dcompiler.lib` / `dxgi.lib`, flips the PiPL `Global_OutFlags_2` to the GPU value via a
  `HAS_HLSL` define, and copies `DirectX_Assets/` next to the `.aex` after linking.
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
3. **`SDK_Invert_ProcAmp` requires CUDA unconditionally** (that sample, not this plugin).
   This plugin needs **no CUDA**: DirectX is the primary GPU path and CUDA is optional
   polish, so the CUDA/OpenCL toolchain is not wired into `ColorGradeFX.vcxproj`.
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
   Acceleration on; confirm the DirectX path renders (same result as CPU). If it falls back
   to CPU on this NVIDIA box, adapter/framework selection is the suspect (this box has both
   an RTX 3090 and an AMD adapter, and AE may prefer CUDA - the flagged Phase 0 unknown).
