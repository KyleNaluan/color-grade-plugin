# PRD: AE Auto Color-Grade Plugin v1.0

Canonical tracker copy: https://github.com/KyleNaluan/color-grade-plugin/issues/1 (labeled ready-for-agent).
This file is the committed mirror; if they diverge, the issue wins.

## Problem Statement

Grading Lumix S5IIx footage in After Effects is slow and repetitive.
V-Log clips need a correct log decode before anything else, technical correction (white balance, exposure, contrast) has to happen per clip, and building a creative look on top means hand-driving Lumetri, curves, and wheels for every project.
Auto-grade tools that exist are black boxes: they produce a finished result that cannot be inspected or tweaked with real grading tools, and they routinely wreck skin tones and clip highlights.
The user wants a fast, trustworthy starting grade that lands in real, editable AE effects, following the Correct then Grade workflow colorists actually use.

## Solution

A CEP panel for After Effects with two tabs, Correct and Grade, backed by a stat-matching auto-grade engine.

The user selects a clip, flags it as V-Log or standard, and the panel builds the Correct stack: a Decode LUT generated from the log profile, then a Lumetri effect with auto-set sliders.
On the Grade tab the user picks a Theme; the engine measures footage stats from rendered frames, computes a transform toward the Theme's target stats, applies the Theme's overrides, and bakes the result into a .cube applied via Apply Color LUT on an adjustment layer.
Everything the panel creates is a Managed effect, visible and editable in vanilla AE.
Knobs (strength, skin-tone protection, plus per-Theme extras) rebake the LUT from cached stats without re-analysis.
Snapshot scopes (waveform, vectorscope, histogram), a before/after toggle, and clipping warnings keep the result honest.
All state lives in a Project-state folder next to the .aep, so a project plus that folder is fully portable to another machine with the plugin.

The engine spike validated the approach: stat matching produces a keepable starting grade when footage and theme are tonally compatible, with cross-temperature matching accepted as a known limitation.
The spike also established that a look's identity must come mostly from authored overrides, with stat matching handling tone and normalization, so theme authoring is budgeted as real work.

## User Stories

1. As an S5IIx shooter, I want to flag a selected clip as V-Log or standard, so that the panel decodes it correctly before any analysis or grading.
2. As an S5IIx shooter, I want the panel to build a Correct stack (Decode LUT then Lumetri with auto-set sliders) on my clip, so that log flatness and technical issues are fixed without manual setup.
3. As a colorist, I want the Correct stack to use real Lumetri sliders, so that I can refine white balance, exposure, and contrast in vanilla AE afterwards.
4. As a colorist, I want to pick a Theme on the Grade tab and get an auto-generated grade, so that I start from a look instead of a blank slate.
5. As a colorist, I want the grade delivered as one baked .cube via Apply Color LUT on an adjustment layer, so that the creative look is a single, portable, editable unit.
6. As a colorist, I want a strength knob (0-100%) on every Theme, so that I can dial the grade back toward identity without regenerating it.
7. As a colorist, I want knob changes to rebake the LUT from cached stats instantly, so that tuning feels interactive and never re-renders frames.
8. As a colorist, I want a re-roll and a reset on the Grade tab, so that I can re-run analysis or return to the ungraded state easily.
9. As someone filming people, I want skin-tone protection to attenuate the grade near the vectorscope skin line, so that Themes never turn faces orange or teal.
10. As someone filming people, I want skin-tone protection to activate only when skin is actually present (above ~2% presence), so that skin-free shots get the full look.
11. As someone filming people, I want a skin protection knob (0-100%, per-theme default ~75%), so that I control the trade-off between look fidelity and skin safety.
12. As a careful grader, I want clipping warnings surfaced by the auto-grade, so that I know when the look crushes shadows or blows highlights.
13. As a careful grader, I want Snapshot scopes (waveform, vectorscope, histogram) available on both tabs, so that I can judge the image objectively while correcting and grading.
14. As a careful grader, I want a before/after toggle in a persistent footer, so that I can compare the graded and ungraded image with one click.
15. As a panel user, I want the footer to always show which clip was analyzed, so that I never apply a grade computed from a different clip's stats.
16. As a panel user, I want the panel to operate on the selected layer in the active comp with the target clip name always shown, so that I always know what I am about to change.
17. As a panel user, I want analysis to sample 5 frames evenly spaced across the clip's in-out range (skipping the first and last ~5%), so that stats reflect the whole shot, not one frame.
18. As a panel user, I want high inter-frame variance flagged to me rather than silently averaged, so that I know when one grade cannot fit the whole clip.
19. As an AE user, I want every effect the panel creates tagged as a Managed effect ([cg] in the display name), so that I can see what the plugin owns in the timeline.
20. As an AE user, I want to edit or delete Managed effects in vanilla AE without breaking the panel, so that the plugin never locks me in.
21. As a project owner, I want all plugin state in a Project-state folder (.colorgrade/ next to the .aep), so that copying the project plus that folder moves everything to another machine.
22. As a project owner, I want grade recipes stored in the Project-state folder keyed by stable layer IDs, so that reopening a project restores full editability of every grade.
23. As a project owner, I want the [cg] tag to serve as a recovery path when layer IDs shift, so that state records can be re-associated instead of orphaned.
24. As a theme author, I want Themes to be data files (target stats, overrides, knobs) with no code, so that new looks can be added without touching the engine.
25. As a theme author, I want a rich overrides layer (tints, curve shapes, chroma shaping), so that a look's identity comes from authored taste, not just stat targets.
26. As a theme author, I want the engine's color transfer distance-damped, so that a Theme applied to tonally opposed footage gives a modest push instead of a garish global cast.
27. As a theme author, I want a CLI evaluation loop (fixture frame in, .cube plus stats printout out), so that I can tune Themes against footage families without launching AE.
28. As a v1 user, I want at least a few tuned, shippable Themes validated against real S5IIx footage families, so that the plugin is useful out of the box.
29. As a new user, I want bundled docs covering usage, LUT export, and project portability, so that I can learn the workflow without leaving the plugin.
30. As a developer, I want all pixel access to go through FrameSource, so that the render-to-file backend can later be swapped for an AEGP native one without touching consumers.
31. As a developer, I want core/ to stay pure (no CEP, Node, or panel imports), lint-enforced, so that all color math remains testable from Node.
32. As a developer, I want the ExtendScript bridge to do AE DOM operations only with zero color math, so that the untestable layer stays as thin as possible.

## Implementation Decisions

- Platform: CEP 12 panel plus a thin ExtendScript bridge; no UXP (AE does not support it). The bridge performs AE DOM operations only and contains zero color math.
- Stack: TypeScript, Vite, Preact for the panel; CEP's Node for file I/O; scopes, curves, and wheels rendered on raw canvas.
- Module layout: core (pure color math, analysis, engine, LUT), themes (data files), panel (UI), host (CEP-side services), extendscript (bridge). Purity of core is lint-enforced.
- Pixel access: render-to-file (16-bit TIFF primary, PNG fallback) behind the FrameSource interface returning normalized Float32 pixels for a comp time. Scopes are Snapshot scopes. An AEGP native backend is an anticipated future swap; nothing may bypass FrameSource (ADR 0001).
- Correct stack per clip: Decode LUT (generated from the Log profile via Apply Color LUT) then Lumetri with auto-set sliders. Grade stack on an adjustment layer: one baked .cube via Apply Color LUT. The two stacks are never mixed (ADR 0002).
- Log profiles are pluggable objects (name, decode, gamut). The registry ships Rec.709 plus 11 camera log profiles (V-Log, S-Log3, C-Log2/3, LogC3/LogC4, D-Log, Film Gen5, F-Log/F-Log2, N-Log; see ADR 0004). The profile's decode function is the single source of truth, consumed by both Decode LUT generation and analysis.
- Theme = data only: target stats plus optional overrides plus exposed Knobs; one shared engine interprets all Themes. Reference-image matching later becomes a Theme whose target stats are computed from a still.
- Engine pipeline: measure footage stats, compute transform toward target stats, apply overrides, scale by Knobs, emit Lumetri slider values and a baked LUT.
- Footage stats schema (shared with target stats): luma histogram and percentiles, LAB means and std-devs, per-Tonal-band chroma (fixed bands 0-0.25 / 0.25-0.7 / 0.7-1.0), saturation distribution, skin presence, clipping percentages.
- Spike-validated engine decisions (working first cut exists with 37 passing tests; refine, do not rewrite): monotone (PCHIP) tone curve matching luma percentiles; distance-damped LAB mean transfer (soft-clamped shift, target stats act as an attractor, not a destination); clamped std ratios; per-band chroma scaling; skin-tone protection as a soft chroma wedge attenuating toward identity.
- Spike verdict encoded as scope: theme authoring and per-Theme tuning against footage families is budgeted, real work; the overrides layer is the main carrier of a look's identity; cross-temperature matching is a known limitation and not a v1 quality gate.
- Sampling: 5 frames evenly spaced across in-out (skip first/last 5%), downsampled to ~960px, averaged; high inter-frame variance is flagged, not silently averaged.
- Project-state folder: .colorgrade/ next to the .aep, holding one state JSON (per-clip records keyed by stable layer IDs, grade recipes) plus the .cube files.
- Managed effects carry a [cg] marker in display names for visibility and ID-shift recovery.
- UI: two tabs (Correct, Grade); strength, reset, re-roll on Grade; persistent footer with before/after toggle, scopes show/hide, and analyzed-clip indicator.
- Bundled user docs ship with the plugin: usage, LUT export, project-portability guide.

## Testing Decisions

Three seams, preferring the highest and fewest; tests assert external behavior at these boundaries, never implementation details behind them.

- Pure-core seam (existing, primary): footage stats in, Transform out, .cube out. Vitest covers color math against published V-Log values and canonical LAB vectors, LUT round-trip property tests (bake, parse, compare with the direct transform), and engine invariants (strength 0 is identity, output bounded, skin protection thresholds). Prior art: the 37 spike tests.
- FrameSource seam (new): all pixel consumers are tested against a file-backed fake FrameSource fed with fixture frames. Golden-fixture tests run on local S5IIx frames and skip cleanly when the frames directory is empty, because fixtures are personal footage and never committed. Prior art: the golden test suite from the spike.
- Panel-bridge seam (new): panel logic is tested against a scripted fake bridge implementing the command boundary. The real ExtendScript layer is covered by a manual smoke checklist per release, not automation.

A good test at any seam feeds realistic inputs (published reference values, fixture frames, recorded bridge scripts) and asserts observable outputs (stats, LUT contents, issued bridge commands), so internals can be refactored freely.

## Out of Scope

- v1.1 features: reference-image matching, standalone LUT export UI, shot matching across clips, look/preset library.
- Stretch: skin-tone corrective pinning, split/wipe compare, grain matching, masking, batch apply.
- Any ML model, anywhere.
- Direct scripting of Lumetri curves and wheels (known unreliable; the LUT path exists precisely to avoid it).
- Cross-temperature grade robustness (warm Theme on cool footage and vice versa) as a quality gate; known limitation, revisit only if it hurts real usage.
- Live (per-frame) scopes; v1 scopes are Snapshot scopes by design.
- AEGP native FrameSource backend (anticipated, not built).
- Metadata-based auto-detection of log footage (manual V-Log/standard toggle instead).

## Further Notes

- Domain vocabulary comes from the project glossary and is used verbatim here: FrameSource, Snapshot scopes, Correct, Grade, Theme, Footage stats, Tonal bands, Target stats, Overrides, Knobs, Skin-tone protection, Project-state folder, Managed effect, Log profile, Decode LUT.
- ADRs 0001-0003 govern pixel access, the sliders/LUT boundary, and the tech stack; issues must not contradict them.
- The spike CLI remains the fastest manual evaluation loop (fixture frame to .cube plus stats printout) until the panel exists, and doubles as the theme-tuning tool.
- Local-only fixtures: the fixture frames directory is gitignored personal footage; any work touching golden tests must preserve the skip-when-empty behavior.
- Issue slicing for the parallel workflow should follow the module seams: core (refine spike), themes (authoring/tuning), panel, host/extendscript bridge, FrameSource backend.
