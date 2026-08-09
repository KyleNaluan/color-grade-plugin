# color-grade-plugin

An After Effects color-grading plugin built for a personal workflow: grading footage shot on a Lumix S5IIx (mostly V-Log) without giving up real, editable grading tools.

## Overview

The project follows a **Correct then Grade** workflow, the way colorists actually work.
Correct fixes technical issues per clip (log decode, white balance, exposure, contrast) using real Lumetri sliders.
Grade applies a creative look on top, computed by a stat-matching engine and baked into a `.cube` LUT applied via the Apply Color LUT effect, so the result is always a normal, editable After Effects effect stack, never an opaque black box.

The repository holds two implementations sharing one color-science core:

- **`src/`** - the original CEP (Common Extensibility Platform) panel: TypeScript, Vite, and Preact for the UI, a thin ExtendScript bridge for AE DOM operations, and a pure `src/core` engine (no framework/host dependencies, lint-enforced).
- **`native/`** - a from-scratch native C++ After Effects Effect SDK plugin (`ColorGradeFX.aex`), built in six phases: a baked-LUT SmartFX effect, a full C++ port of the TypeScript engine, a Dear ImGui/Win32/D3D11 in-app editor window, live clip preview, waveform/histogram/vectorscope scopes with before/after compare, a manual primary-correction suite (exposure, contrast, highlights/shadows, temperature/tint, saturation), and interactive curves plus DaVinci-style Lift/Gamma/Gain color wheels. Supports both a CPU render path and a GPU path (DirectX + CUDA).

The color engine itself supports 11 published camera log profiles (Panasonic V-Log, Sony S-Log3, Canon C-Log2/C-Log3, ARRI LogC3/LogC4, DJI D-Log, Blackmagic Film Gen5, Fujifilm F-Log/F-Log2, Nikon N-Log) plus Rec.709, a LAB-space statistical look-matching transform, a 24-look theme library, skin-tone-protected grading, and an optional BYOK Gemini-vision "critic" loop for iterative auto-grading (rules decide accept/reject each round, the model only names defects, per an internal study showing every tested model tier misjudged which round was actually better).

## Why it exists / context

Built to solve a specific, recurring problem: grading V-Log Lumix footage in After Effects is slow and repetitive, and existing auto-grade tools are black boxes that wreck skin tones and can't be tweaked with real grading tools afterward.
The full product requirements are in `docs/prd.md`; the original design/scoping notes are in `idea.md`; architectural decisions are recorded as ADRs in `docs/adr/`.
This is a solo side project, developed over roughly 8 days (July 12-19) with heavy use of an AI coding agent for implementation, evidenced by the committed `CLAUDE.md`/`AGENTS.md` project-memory files and the ~28 feature branches tracking each build phase.

## How to run / build

CEP panel (from repo root):

```bash
npm install
npm test          # Vitest
npm run typecheck
npm run lint       # core-purity check
npm run build      # Vite build, assembles an installable CEP extension into dist/
```

Install instructions for the built CEP extension are in `docs/cep-install.md`.

Native AE plugin (Windows-first, built from WSL against Visual Studio 2022 MSBuild):

```bash
native/scripts/build.sh Debug            # CPU-only build
native/scripts/build.sh Release --gpu    # CPU + GPU (DirectX + CUDA)
```

Full toolchain, environment variables, and AE-runtime verification steps are in `native/BUILDING.md`; the native subproject's phase-by-phase layout is documented in `native/README.md`.

Cross-engine correctness harnesses (compare the C++ port against the TypeScript oracle, not run in CI, need g++/clang locally):

```bash
npm run native:parity        # LUT-apply parity
npm run native:core-parity   # full engine parity (stats/bake/recipe)
```

## What it demonstrates technically

- Faithful transcription of 11 published camera log transfer functions and gamut matrices, validated against each manufacturer's documented 0%/18%/90% code values (`tests/unit/logProfiles.test.ts`).
- A from-scratch statistical color-matching engine: LAB-space distance-damped stat transfer, monotone (PCHIP) tone curves, per-tonal-band chroma scaling, and a soft skin-tone-protection wedge in vectorscope chroma space.
- Two independently-built, cross-validated engine implementations (TypeScript and header-only C++) kept in numerical parity by an automated golden-value harness, achieving bit-exact results on most paths and ~1e-4 to 2e-13 tolerance elsewhere.
- A native After Effects Effect SDK plugin written directly against Adobe's C SDK and AEGP suites: SmartFX rendering (CPU + DirectX/CUDA GPU paths), PF arbitrary-data persistence with versioned migration, and a hand-built AEGP idle-hook bridge synchronizing a separate ImGui window with live effect parameters.
- A vendored, compiled-in Dear ImGui UI layer implementing custom widgets (draggable curve editor, color wheels, waveform/vectorscope/histogram scopes) with no runtime dependencies.
- Disciplined architecture: a strictly pure, lint-enforced core module with no framework imports, and a documented seam (`FrameSource`) isolating all pixel-acquisition logic so it can be swapped without touching consumers.
- A rules-based (non-LLM-judged) quality-regression guard for an optional AI-assisted grading loop, informed by an internal study showing vision models are unreliable judges of grading quality even when they can name defects correctly.
