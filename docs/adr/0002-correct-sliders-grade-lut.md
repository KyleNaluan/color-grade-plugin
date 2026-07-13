# 0002. Correct = Lumetri sliders, Grade = baked LUT, never mixed

Date: 2026-07-12
Status: Accepted

## Context

Lumetri's curves and wheels are not reliably scriptable, so the creative grade must be baked into a generated .cube LUT applied via Apply Color LUT.
Plain Lumetri sliders are scriptable.
The boundary between the two determines what the user can tweak natively in AE versus only through the panel.

## Decision

- Correct stack (per clip): Decode LUT, then Lumetri with auto-set sliders (exposure, contrast, white balance, whites/blacks, baseline saturation). Fully editable in vanilla AE.
- Grade stack (adjustment layer): a single Apply Color LUT with the baked theme transform. All creative work (hue biases, curves, wheels, overrides) lives in the LUT. No Lumetri on the grade layer.
- Every baked .cube gets a sidecar JSON written next to it, capturing the full recipe (theme, knobs, curve edits) so the grade is always re-editable through the panel.
- LUTs and sidecars are stored in a predictable per-project location and treated as project assets that must travel with the project.

## Consequences

- The grade LUT is directly portable (v1.1 standalone export is this file).
- Shot matching naturally lives in the Correct stack.
- Without the panel, a grade still renders correctly but is frozen: re-editing the recipe requires the panel plus the sidecar. Accepted, since this is a single-user tool.
- The plugin ships with user documentation covering usage, LUT/sidecar export, and how to move a project to another machine that also has the plugin (bundled docs now; possibly a website later).
