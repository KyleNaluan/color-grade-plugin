# PRD Handoff - AE Auto Color-Grade Plugin

Purpose: everything a PRD-writing pass (e.g. `/to-prd`) needs, condensed from the 2026-07-12 grilling session.
Read this file first, then the pointed-to files for depth.

## Source files

- `idea.md` - scope by phase (v1.0 / v1.1 / stretch), workflow rationale, decided constraints. The primary requirements source.
- `CONTEXT.md` - the glossary. Use these terms verbatim in the PRD (FrameSource, Theme, Knobs, Decode LUT, Project-state folder, Managed effect, etc.).
- `docs/adr/0001-frame-source-abstraction.md` - pixel access decision.
- `docs/adr/0002-correct-sliders-grade-lut.md` - the sliders/LUT boundary.
- `docs/adr/0003-panel-tech-stack.md` - tech stack.

## Condensed decisions

### Architecture

- Platform: CEP 12 + thin ExtendScript bridge (no UXP; AE does not support it). Bridge does AE DOM operations only, zero color math.
- Stack: TypeScript + Vite + Preact; CEP's Node for file I/O; scopes/curves/wheels are raw canvas.
- Directory: `src/{core,themes,panel,host,extendscript}`; `core/` is pure (no CEP/Node/panel imports), lint-enforced.
- Pixel access: render-to-file (16-bit TIFF primary, PNG fallback) behind `FrameSource(time) -> normalized Float32 pixels`. Scopes are snapshot (on-demand), not live. AEGP native backend is an anticipated future swap - nothing may bypass FrameSource.

### Color pipeline

- Correct stack per clip: Decode LUT (generated from the log profile, via Apply Color LUT) then Lumetri with auto-set sliders. Fully editable in vanilla AE.
- Grade stack on adjustment layer: one baked .cube via Apply Color LUT. All creative work lives there. Never mix the two.
- Log profiles are pluggable `{name, decode, gamut}`; V-Log + Rec.709 only for now. `decode()` is the single source of truth (bakes Decode LUT, informs analysis).

### Auto-grade engine

- Theme = data only: target stats + optional artistic overrides + exposed knobs (every theme gets strength 0-100%). One shared engine.
- Pipeline: measure footage stats -> compute transform toward target stats -> apply overrides -> scale by knobs -> emit sliders + baked LUT.
- Footage stats (same schema as target stats): luma histogram/percentiles, LAB means/std-devs, per-tonal-band chroma (fixed bands 0-0.25/0.25-0.7/0.7-1.0), saturation distribution, skin presence, clipping %.
- Sampling: 5 frames evenly spaced across in-out (skip first/last 5%), downsampled (~960px), averaged; high inter-frame variance is flagged to the user, not silently averaged.
- Skin-tone protection: soft chroma wedge around the vectorscope skin line attenuates the transform toward identity; activates only above ~2% skin presence; knob 0-100% (default ~70%). Corrective pinning deferred to stretch.
- Reference-image matching (v1.1) = a theme with target stats computed from a dropped-in still; falls out of the same engine.

### State and portability

- Project-state folder: `.colorgrade/` next to the .aep - one state JSON (per-clip records keyed by stable layer IDs, grade recipes) plus the .cube files. Copying this folder = full portability to another machine with the plugin.
- Managed effects tagged `[cg]` in display names (visibility + recovery path if layer IDs shift).
- Selection model: panel operates on the selected layer in the active comp; target clip name always shown.

### UI

- Two tabs: Correct, Grade (Review tab cut). Strength/reset/re-roll on Grade tab.
- Persistent footer on both tabs: before/after toggle (enables/disables grade layer), scopes show/hide, analyzed-clip indicator.
- Bundled user docs in v1.0: usage, LUT/sidecar export, project-portability guide (files shipped with plugin; maybe info tab/website later).

### Testing

- Vitest on pure core (color math vs published V-Log values, LAB canonical vectors).
- Golden-fixture tests on checked-in S5IIx frames (V-Log + standard).
- LUT round-trip property tests (bake -> parse -> compare against direct transform).
- ExtendScript layer: manual smoke checklist per release, not automated.

## Gate before writing the PRD

An engine spike is running first: `core/` only, run from Node against exported S5IIx frames - does stat-matching produce a keepable starting grade?

Record the outcome here before the PRD pass:

- [ ] Spike result: (fill in - if grades look generic, expand the overrides layer and treat theme authoring as a bigger workstream in the PRD; if good, themes stay thin data files)
