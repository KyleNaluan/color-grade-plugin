# native/ - Color Grade native After Effects effect (Phase 2)

The Windows-first native re-platform of Color Grade from a CEP panel to a C++ AE Effect
SDK plugin.

- **Phase 1** (the spine): a registering, draggable **SmartFX** effect that applies a baked
  3D LUT to a layer via the ported trilinear `sampleLut`, on a CPU path and a GPU path
  (DirectX + CUDA).
- **Phase 2** (this milestone): the full `src/core` color engine ported to C++
  (`ColorGradeFX/core/`), an effect **parameter surface + arb-data** grade recipe that drives
  it (theme popup, strength / skin-protection / chroma-gain knobs, plus the persisted
  measured-stats + typed-knob-space recipe), an in-effect **Auto** bake
  (`buildTransform` -> `bakeLut` natively), and a **cross-engine golden harness** proving the
  C++ core matches the TS oracle to ~1e-4 (achieved: grade/decode/recipe bit-exact, stats 2e-13).

Later phases add the editor window, live preview, and in-effect analysis
(see `firstmate:native-scope-m2/report.md`).

## Layout

```
native/
  BUILDING.md                       pinned toolchain, env vars, Phase 0 checklist + fixes
  ColorGradeFX/
    ColorGrade.h / .cpp             the SmartFX effect (register, params, arb-data, render, Auto bake)
    ColorGradeGPU.inc               GPU path: device setup + SmartRenderGPU (DirectX + CUDA)
    ColorGrade_Kernel.cu / .chlsl   GF-macro trilinear LUT kernel (nvcc for CUDA, DXC for DirectX)
    ColorGradePiPL.r                PiPL resource (effect discovery + Global OutFlags)
    core/                           C++ port of src/core (the oracle); header-only, dependency-free,
                                    all math in double, narrows to float only where the TS does:
      Mat3.h Rec709.h Vlog.h Lab.h    color math + matrices + LAB
      LogProfile.h Decode.h           profiles (V-Log/Rec.709) + decode-to-Rec.709
      MonotoneCurve.h                 PCHIP tone/shape curves
      Stats.h                         FootageStats + computeStats
      Theme.h Themes.h                Theme type + the 3 shipping themes (data, transcribed)
      Engine.h                        buildTransform (the grade transform)
      BakeLut.h                       bakeLut / bakeGradeLut / bakeDecodeLut -> cg::Lut3D
      Recipe.h                        POD arb-data recipe + <-> Theme/stats + bakeFromRecipe
    lut/CubeLut.h                   ported parseCube + sampleLut (mirrors src/core/lut/cube.ts)
    embedded/EmbeddedLut.h          GENERATED default LUT (teal-orange grade, 17^3)
    Win/ColorGradeFX.vcxproj/.sln   MSBuild project (CPU default; /p:CG_GPU=true builds DirectX + CUDA)
  tests/parity/
    parity_test.cpp                 Phase 1: sampleLut vs TS oracle
    core_parity.cpp                 Phase 2: computeStats / bakeGradeLut / bakeDecodeLut / recipe replay
  scripts/
    build.sh                        WSL -> NTFS mirror -> MSBuild interop build
    gen-embedded-lut.ts             bake the embedded LUT header from the TS engine
    parity-test.ts                  Phase 1 parity gate (local, not in CI)
    core-parity-test.ts             Phase 2 cross-engine golden harness (local, not in CI)
```

## Commands

```bash
npm run native:gen-lut       # regenerate embedded/EmbeddedLut.h from the TS engine
npm run native:parity        # Phase 1 LUT-apply parity gate (needs g++/clang; not in CI)
npm run native:core-parity   # Phase 2 full-core cross-engine golden harness (g++/clang; not in CI)
native/scripts/build.sh Debug          # CPU-only build
native/scripts/build.sh Release --gpu  # CPU + GPU (DirectX + CUDA) build
```

The two parity harnesses are the numerical-correctness evidence and run unattended from WSL;
the C++ build stays un-gated in CI (the local interop build is the evidence path, as in Phase 1).

See **BUILDING.md** for the full toolchain, env vars, permissions, and the captain-assisted
AE runtime verification steps (AE registration, param UI, Auto grade, LUT apply, GPU path).
