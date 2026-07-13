# Ubiquitous Language

## FrameSource

The single interface through which all pixel consumers (analysis, scopes, LUT baking) obtain rendered frame pixels.
Returns normalized float pixel data for a given comp time.
Consumers never know or care how the pixels were acquired.
See ADR 0001.

## Snapshot scopes

Scopes (waveform, vectorscope, histogram) that refresh on demand or on scrub-stop, rather than live per-frame.
The v1 scope model, a consequence of the render-to-file FrameSource backend.

## Correct

The first stage of the workflow: fixing technical issues (white balance, exposure, contrast, baseline saturation) and log-decoding footage, applied per-clip via real Lumetri sliders.
Always happens before analysis and grading so the auto-grade sees clean stats.

## Grade

The second stage: the creative look (theme, curves, wheels, HSL secondary), applied on top of corrected footage, baked into a generated .cube LUT applied via the Apply Color LUT effect.

## Theme

A data-only definition of a look: target stats, optional overrides, and exposed knobs.
Themes contain no code; one shared engine interprets them all.
A reference image is just a theme whose target stats are computed on the spot from the dropped-in still.

## Footage stats

The measured color statistics of a clip, sharing the exact schema of target stats so the engine matches stat-to-stat.
Comprises luma histogram and percentiles, LAB means/std-devs, per-tonal-band chroma means, saturation distribution, skin-tone presence, and clipping percentages.
Computed from 5 frames evenly spaced across the clip's in-out range (skipping the first/last ~5%), downsampled, averaged, with high inter-frame variance flagged to the user instead of silently averaged.

## Tonal bands

Shadows, mids, and highlights, carved by fixed luma thresholds (0-0.25, 0.25-0.7, 0.7-1.0) on the decoded Rec.709 signal.

## Target stats

The color statistics profile a theme aims footage toward: LAB distribution, contrast, saturation, and hue biases.
The engine computes the transform that moves measured footage stats toward the target stats.

## Overrides

Hand-authored artistic nudges a theme applies after stat matching (e.g. fixed curve shapes, shadow tints) - taste the target stats cannot express.

## Knobs

The tuning parameters a theme exposes to the user, each with a range and default.
Every theme gets a global strength knob (0-100%, interpolating the transform toward identity); themes may expose a few of their own.
Adjusting a knob recomputes the transform and rebakes the LUT from cached stats - no re-analysis.

## Skin-tone protection

A LUT-construction step: within a soft-edged wedge around the vectorscope skin line in chroma space, the theme transform is attenuated toward identity.
Activates only when skin presence exceeds a threshold (~2% of pixels near the skin line), and is user-tunable via a protection knob (0-100%, default ~70%; 0% yields the pure theme).
Corrective pinning (pulling drifted skin tones back to the skin line) is deliberately deferred as a possible future addition.

## Project-state folder

A `.colorgrade/` folder next to the .aep holding one JSON state file (per-clip records keyed by stable layer IDs, grade recipes) plus the baked .cube files.
The single thing that must travel with a project for the panel to restore full editability.
Subsumes the earlier per-LUT sidecar idea: recipes live in the project-state file, .cube files beside it.

## Managed effect

An effect the panel created and owns, tagged with a marker in its display name (e.g. `Lumetri Color [cg]`).
The marker makes panel ownership visible in AE's timeline and serves as the recovery path for re-associating effects with state records if layer IDs shift.

## Log profile

A pluggable object describing a camera log format: name, decode transfer function, and gamut.
V-Log (V-Gamut) and standard/Rec.709 are the only implemented profiles for now.
The profile's decode function is the single source of truth for log math, consumed both by the analysis engine and by Decode LUT generation.

## Decode LUT

A .cube LUT generated from a log profile's decode function, applied via Apply Color LUT as the first effect in a clip's Correct stack.
Normalizes log footage to Rec.709 before Lumetri corrections and the creative grade.
Analysis pixels sampled after correction are therefore already decoded; the analysis engine only needs to know whether decode has been applied yet.
