# native/ - Color Grade native After Effects effect (Phase 1)

The Windows-first native re-platform of Color Grade from a CEP panel to a C++ AE Effect
SDK plugin. Phase 1 is the spine: a registering, draggable **SmartFX** effect that applies
a baked 3D LUT to a layer via the ported trilinear `sampleLut`, on a CPU path and a
GPU path (DirectX + CUDA). Later phases add the engine port, editor window, live preview, and
in-effect analysis (see `firstmate:native-scope-m2/report.md`).

## Layout

```
native/
  BUILDING.md                       pinned toolchain, env vars, Phase 0 checklist + fixes
  ColorGradeFX/
    ColorGrade.h / .cpp             the SmartFX effect (register, params, CPU render)
    ColorGradeGPU.inc               GPU path: device setup + SmartRenderGPU (DirectX + CUDA)
    ColorGrade_Kernel.cu / .chlsl   GF-macro trilinear LUT kernel (nvcc for CUDA, DXC for DirectX)
    ColorGradePiPL.r                PiPL resource (effect discovery + Global OutFlags)
    lut/CubeLut.h                   ported parseCube + sampleLut (mirrors src/core/lut/cube.ts)
    embedded/EmbeddedLut.h          GENERATED default LUT (teal-orange grade, 17^3)
    Win/ColorGradeFX.vcxproj/.sln   MSBuild project (CPU default; /p:CG_GPU=true builds DirectX + CUDA)
  tests/parity/parity_test.cpp      C++ harness for the cross-engine parity gate
  scripts/
    build.sh                        WSL -> NTFS mirror -> MSBuild interop build
    gen-embedded-lut.ts             bake the embedded LUT header from the TS engine
    parity-test.ts                  C++ sampleLut vs TS oracle, ~1e-4 (local, not in CI)
```

## Commands

```bash
npm run native:gen-lut     # regenerate embedded/EmbeddedLut.h from the TS engine
npm run native:parity      # cross-engine parity gate (needs g++/clang; not wired into CI)
native/scripts/build.sh Debug          # CPU-only build
native/scripts/build.sh Release --gpu  # CPU + GPU (DirectX + CUDA) build
```

See **BUILDING.md** for the full toolchain, env vars, permissions, and the captain-assisted
AE runtime verification steps (AE registration, LUT apply, GPU path).
