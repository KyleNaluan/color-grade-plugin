# After Effects Auto Color-Grade Plugin — Idea Doc

## One-line summary
An After Effects panel that lets you pick a color theme/look, auto-generates a tweakable grade for your footage (with full access to real grading tools — scopes, curves, wheels), and is built around a Correct → Grade workflow that natively supports V-Log footage from a Lumix S5IIx.

## Primary use case
Personal color-grading workflow for footage shot on a Lumix S5IIx, mostly in V-Log, sometimes standard/Rec.709. The auto-grade should be a fast, tweakable starting point — not a black-box final output.

---

## Decided constraints (research-backed, not open questions)

- **Platform: CEP + ExtendScript**, not UXP. After Effects does not yet support UXP panels (Photoshop/InDesign have it; Premiere is in beta; AE is not there yet as of 2026). CEP 12 is current and stable for this purpose.
- **No external ML model for v1.0/v1.1.** Auto-grade, color matching, shot matching, skin tone protection, and clipping detection are all achievable with classical image analysis (histograms, LAB-space stat matching, published log transfer functions) — not machine learning. The narrow, known input space (one camera, one log profile, a fixed theme set) favors hand-tuned rules over a model, and a model would add real integration cost (bundling an inference runtime or calling an external process) for no clear quality win here.
- **Curves and color wheels are not reliably scriptable via ExtendScript.** Lumetri's curve/wheel controls are opaque custom-UI data blobs. Only plain sliders (exposure, contrast, highlights/shadows/whites/blacks, white balance, saturation) are safely settable via script.
  - **Workaround:** the grading engine (curves, wheels, theme looks) computes its result internally and bakes it into a generated `.cube` LUT, applied via the **Apply Color LUT** effect (a simple, reliable file-path property) — not via direct Lumetri property scripting. Basic correction still goes through real Lumetri sliders.
- **Log handling is a pluggable profile, not hardcoded math.** V-Log has a published transfer function + gamut (V-Gamut), which the analysis engine can decode directly. Structure this as a `{ name, decode(pixel), gamut }`-style profile object so other log formats (S-Log3, C-Log3, F-Log, etc.) can be added later without rearchitecting. **Only V-Log + standard/Rec.709 are implemented now.**
- **Repo: GitHub.**

## Resolved in grilling session (2026-07-12) — see docs/adr/ and CONTEXT.md

- Pixel access: render-to-file (16-bit TIFF) behind a `FrameSource` interface; AEGP native backend is an anticipated future swap (ADR 0001)
- Correct = Lumetri sliders, Grade = baked LUT, sidecar JSON recipe next to every .cube (ADR 0002)
- Stack: TypeScript + Vite + Preact, thin ExtendScript bridge, CEP Node for file I/O (ADR 0003)
- Themes are data: target stats + overrides + knobs; one shared engine; reference matching falls out free
- Testing: Vitest on pure core, golden-fixture tests on checked-in S5IIx frames, LUT round-trip property tests, manual AE smoke checklist
- Directory structure: `src/{core,themes,panel,host,extendscript}` with lint-enforced rule that `core/` is pure (no CEP/Node/panel imports)

---

## Core workflow / architecture

**Correct → Grade**, mirroring how colorists actually work, with persistent (dockable/collapsible) scopes available across both:

1. **Correct tab** — white balance, exposure, contrast, baseline saturation. Fixes technical issues (camera error, log flatness), applied first, ideally per-clip. This is also where log decode happens: flag footage as Standard or V-Log (manual toggle — metadata-based auto-detection is unreliable), decode/normalize before analysis.
2. **Grade tab** — theme picker, auto-grade trigger, curves, color wheels, HSL secondary. Applied on top of corrected footage, often via an adjustment layer across multiple clips.
3. ~~Review tab~~ **Cut (grilling session):** two tabs only. Strength/reset/re-roll live on the Grade tab; a persistent footer bar on both tabs holds before/after toggle, scopes show/hide, and the analyzed-clip indicator. Split/wipe compare stays in stretch.

**Why correct-then-grade matters for auto-grade accuracy:** the theme-mapping rules react to footage stats. If those stats are taken from uncorrected/log footage, the mapping is working off skewed data. Always correct (and log-decode) before analyzing for the grade.

**Auto-grade engine:** sample frames → analyze (histogram, dominant color, dynamic range, skin-tone regions) → rules/lookup table maps theme + stats → Lumetri slider values (Correct) + generated LUT (Grade). Fully tweakable afterward, either through the panel or directly in AE's native Lumetri UI, since it's all written to real effects.

**Scopes:** not exposed live by AE for scripting — built custom (waveform, vectorscope, histogram) by rendering a frame and reading pixel data into canvas.

---

## Scope by phase

### v1.0 — Core build
- CEP panel, ExtendScript bridge, Correct → Grade tabs, persistent custom scopes
- Auto-grade engine (theme-driven, rules-based), fully tweakable after generation
- Log support: V-Log decode as a pluggable profile
- Curves/wheels via generated `.cube` LUT baking + Apply Color LUT
- **Skin tone protection** (constrain auto-grade against the vectorscope skin-tone line) and **clipping/gamut warnings** built into the auto-grade logic itself — treated as core responsible-grading behavior, not bonus features

- **Bundled user docs** — usage + export instructions and a "moving a project to another machine (with the plugin installed)" guide, shipped as files with the plugin (possibly an info tab or website later)

### v1.1 — First additions
- **Reference-image matching** — target a dropped-in still's color stats instead of a preset theme (reuses existing analysis engine)
- **Standalone LUT export (.cube)** — expose the already-baked grade LUT for use outside AE (Premiere, Resolve, etc.)
- **Shot matching across clips** — normalize multiple clips from the same scene to a consistent look before the creative grade is applied
- **Look/preset library** — save theme + parameters (not just the baked LUT) as a reusable, re-editable preset

### Optional / stretch — no fixed timeline
- **Skin-tone corrective pinning** — beyond attenuation, pull skin tones the theme pushed off the vectorscope skin line back toward it; revisit after seeing how well v1 protection performs
- **Before/after compare (split or wipe)**
- **Grain/texture matching**
- **Masking / auto-masking** — real content-aware segmentation is a genuine ML problem (what Roto Brush is built on, or something like SAM as an external process); non-ML fallback is rule-based luma/chroma keying and edge-detection tips. Only place in the whole project where an external model would be a real candidate — and only if this feature gets built.
- **Batch apply across a timeline**

### Explicitly not planned
- No ML model anywhere in v1.0/v1.1
- No dependency on Lumetri curve/wheel scripting (known unreliable)

---

## Interesting future idea (v2+, not in current scope)
Log the delta between auto-grade output and the user's final manual adjustments over time, and use that to nudge future auto-grades toward personal taste — a lightweight personalization loop rather than a general-purpose model.
