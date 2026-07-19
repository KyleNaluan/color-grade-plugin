# native/ - Color Grade native After Effects effect (Phase 6b/6c)

The Windows-first native re-platform of Color Grade from a CEP panel to a C++ AE Effect
SDK plugin.

- **Phase 1** (the spine): a registering, draggable **SmartFX** effect that applies a baked
  3D LUT to a layer via the ported trilinear `sampleLut`, on a CPU path and a GPU path
  (DirectX + CUDA).
- **Phase 2** (landed): the full `src/core` color engine ported to C++
  (`ColorGradeFX/core/`), an effect **parameter surface + arb-data** grade recipe that drives
  it (theme popup, strength / skin-protection / chroma-gain knobs, plus the persisted
  measured-stats + typed-knob-space recipe), an in-effect **Auto** bake
  (`buildTransform` -> `bakeLut` natively), and a **cross-engine golden harness** proving the
  C++ core matches the TS oracle to ~1e-4 (achieved: grade/decode/recipe bit-exact, stats 2e-13).

- **Phase 3** (landed): the **editor window** - a native ImGui/Win32/D3D11 window
  opened from an **Open Editor…** button, with Correct/Grade controls wired to the effect's
  params through a pure effect<->window bridge. The **toolkit decision** (native ImGui vs an
  embedded webview reusing Preact) and its rationale live in `docs/adr-editor-ui.md`.

- **Phase 4** (landed): a **live clip preview** inside the editor window - the actual
  decode+graded clip frame, centered + letterboxed, refreshing on timeline scrub and any
  effect-param change. The idle hook checks the frame out downstream of the effect, a bounded
  per-instance LRU cache keeps scrubbing interactive, and the risky logic is isolated in the
  pure `editor/PreviewCache.h` (see `docs/adr-editor-ui.md`, "Phase 4" section).

- **Phase 5** (landed): **in-effect analysis + live scopes + before/after**. The idle
  hook samples several frames upstream of the effect, decodes each to Rec.709 via the footage
  profile, and runs the ported `computeStats` over the union (incremental, one checkout per
  tick), injecting the measured stats over the recipe at render. The editor gains
  waveform/histogram/vectorscope scopes drawn from the graded preview frame, plus an
  After/Before/Split toolbar. Risky logic is isolated in the pure `editor/Analysis.h`,
  `editor/Scopes.h`, and `splitViewGeometry` in `editor/PreviewCache.h`
  (see `docs/adr-editor-ui.md`, "Phase 5" section).

- **Phase 6a** (landed): the **manual grade suite (Basics)** - a manual
  primary-correction stage that runs *ahead* of the theme stages on decoded gamma-Rec.709
  (Exposure, Contrast+pivot, Highlights/Shadows/Whites/Blacks, Temperature/Tint, Saturation,
  Vibrance), every control neutral-gated to exact identity. A new **None (Manual)** theme
  (`matchStats=false`) makes pure manual grading first-class, and **Look Mix** blends the
  theme look over the manual pixel. Three keyframeable Effect Controls params
  (Exposure / Look Mix / Temperature) are appended after the recipe; the editor gains a
  **Basics** tab and the recipe grew a versioned migration (v2->v3) so old saved grades
  survive (see `docs/adr-editor-ui.md`, "Phase 6a" section).

- **Phase 6b/6c** (this milestone): **Curves + Wheels** editor tabs. **6b Curves** is
  UI-only (the engine already evaluated the tone + per-channel R/G/B curves) - an interactive
  ImGui curve widget writing recipe-backed curve state. **6c Wheels** adds a new DaVinci
  **Lift/Gamma/Gain** engine stage (per-channel printer-lights, neutral = exact identity,
  oracle-first then bit-exact port) plus an Adobe-style **3-way** secondary mode that adds no
  engine math (additive band tints + shared LGG-master luminance). All editor look-state
  lives in dedicated **user** recipe fields composed onto the popup theme by the shared
  `cg::core::applyEditorOverrides`; no new keyframeable params; the recipe migration grew a
  v3->v4 arm so old grades survive (see `docs/adr-editor-ui.md`, "Phase 6b/6c" section).

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
      Theme.h Themes.h                Theme type + all 24 registry looks (getTheme-by-name builders, data, transcribed)
      Engine.h                        buildTransform (manual stage + LGG wheels stage + the grade transform + Look Mix)
      BakeLut.h                       bakeLut / bakeGradeLut / bakeDecodeLut -> cg::Lut3D
      Recipe.h                        POD arb-data recipe (+ manual block + editor user fields) + <-> Theme/stats + bakeFromRecipe + applyEditorOverrides + v2/v3->v4 migrateRecipeInto + reference-match theme + stats-sidecar parse
    lut/CubeLut.h                   ported parseCube + sampleLut (mirrors src/core/lut/cube.ts)
    embedded/EmbeddedLut.h          GENERATED default LUT (teal-orange grade, 17^3)
    editor/                         Phase 3-6c editor window (see docs/adr-editor-ui.md)
      EditorBridge.h                  pure effect<->window seam (edit queue + mapping + curve/wheel point logic); headless-tested
      PreviewCache.h                  Phase 4-5 pure preview core (cache/keying/fit/checkin + split geometry); headless-tested
      Analysis.h                      Phase 5 pure analysis core (frame-sampling schedule + incremental job + fingerprint/debounce); headless-tested
      Scopes.h                        Phase 5 pure scope synthesis (waveform/histogram/vectorscope binning -> RGBA8); headless-tested
      EditorWindow.h / .cpp           Win32/D3D11/ImGui host + preview/scope textures + curve/wheel widgets (no-op stubs off Windows)
    Win/ColorGradeFX.vcxproj/.sln   MSBuild project (CPU default; /p:CG_GPU=true builds DirectX + CUDA)
  third_party/imgui/                vendored Dear ImGui v1.91.5 (MIT) + Win32/D3D11 backends
  docs/adr-editor-ui.md             Phase 3 toolkit decision (ImGui vs webview) + bridge design + Phase 4 preview + Phase 5 analysis/scopes + Phase 6a manual grade + Phase 6b/6c curves + wheels
  tests/parity/
    parity_test.cpp                 Phase 1: sampleLut vs TS oracle
    core_parity.cpp                 Phase 2-6c: computeStats / bakeGradeLut / bakeDecodeLut / recipe / manual grade + Look Mix / LGG + editor-override render path / v2/v3->v4 migrate replay / reference-match theme + stats-sidecar round-trip
  tests/editor/
    bridge_test.cpp                 Phase 3-6c: editor<->effect bridge logic + curve/wheel round-trip + far-drag monotone (headless, self-asserting)
    preview_test.cpp                Phase 4: live-preview core (cache/keying/fit/checkin; headless, self-asserting)
    analysis_test.cpp               Phase 5: analysis schedule/job/debounce (headless, self-asserting)
    scopes_test.cpp                 Phase 5: scope binning + image synthesis (headless, self-asserting)
  scripts/
    build.sh                        WSL -> NTFS mirror -> MSBuild interop build (CG_OUT_DIR override for link-verify)
    gen-embedded-lut.ts             bake the embedded LUT header from the TS engine
    parity-test.ts                  Phase 1 parity gate (local, not in CI)
    core-parity-test.ts             Phase 2 cross-engine golden harness (local, not in CI)
    editor-bridge-test.ts           Phase 3 bridge-logic test (local, not in CI)
    preview-test.ts                 Phase 4 live-preview core test (local, not in CI)
    analysis-test.ts                Phase 5 analysis-core test (local, not in CI)
    scopes-test.ts                  Phase 5 scope-synthesis test (local, not in CI)
```

## Commands

```bash
npm run native:gen-lut       # regenerate embedded/EmbeddedLut.h from the TS engine
npm run native:parity        # Phase 1 LUT-apply parity gate (needs g++/clang; not in CI)
npm run native:core-parity   # Phase 2 full-core cross-engine golden harness (g++/clang; not in CI)
npm run native:editor-test   # Phase 3 headless editor<->effect bridge logic (g++/clang; not in CI)
npm run native:preview-test  # Phase 4 headless live-preview core (g++/clang; not in CI)
npm run native:analysis-test # Phase 5 headless analysis core (g++/clang; not in CI)
npm run native:scopes-test   # Phase 5 headless scope synthesis (g++/clang; not in CI)
native/scripts/build.sh Debug          # CPU-only build
native/scripts/build.sh Release --gpu  # CPU + GPU (DirectX + CUDA) build
CG_OUT_DIR='C:\dev\cg-verify-out' native/scripts/build.sh Debug  # link-verify without closing AE
```

The two parity harnesses are the numerical-correctness evidence and run unattended from WSL;
the C++ build stays un-gated in CI (the local interop build is the evidence path, as in Phase 1).

See **BUILDING.md** for the full toolchain, env vars, permissions, and the captain-assisted
AE runtime verification steps (AE registration, param UI, Auto grade, LUT apply, GPU path).
