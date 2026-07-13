# Installing the panel in After Effects (development)

The build emits a complete CEP 12 extension in `dist/`:

```
dist/
  CSXS/manifest.xml   CEP 12 manifest (AEFT 24.0+)
  .debug              remote-debug port mapping (Chrome DevTools on :8092)
  host/host.jsx       ExtendScript bridge (AE DOM queries only)
  panel/              Vite-built panel (index.html + assets)
```

## Steps

1. Build the extension:

   ```
   npm install
   npm run build
   ```

2. Enable unsigned extensions (once per machine).
   CEP 12 uses the `CSXS.12` key:

   - Windows: add a string value `PlayerDebugMode` = `1` under `HKEY_CURRENT_USER\Software\Adobe\CSXS.12`.
   - macOS: `defaults write com.adobe.CSXS.12 PlayerDebugMode 1`.

3. Link `dist/` into the CEP extensions folder as `com.kylenaluan.colorgrade`:

   - Windows: `%APPDATA%\Adobe\CEP\extensions\com.kylenaluan.colorgrade` (junction: `mklink /J <target> <repo>\dist`).
   - macOS: `~/Library/Application Support/Adobe/CEP/extensions/com.kylenaluan.colorgrade` (`ln -s <repo>/dist <target>`).

4. Start After Effects (2024 / 24.0 or later) and open Window > Extensions > Color Grade.

## Frame analysis prerequisite: a 16-bit TIFF output template

The "Analyze frame" action renders the current frame to a temporary file through
the render queue (ADR 0001). Its primary path applies an output-module template
named **`CG 16-bit TIFF`**, so that template must exist in the AE install:

1. Add any comp to the Render Queue, open its Output Module settings.
2. Set Format = **TIFF**, Channels = RGB, Depth = **Trillions of Colors (16 bpc)**.
3. Save Output Module template as `CG 16-bit TIFF` (the exact name in
   `src/extendscript/host.jsx`, `CG_TIFF_TEMPLATE`).

If the template is missing or fails, the panel automatically falls back to an
8-bit PNG (`saveFrameToPng`), which is coarser but keeps analysis working.

## Smoke checklist (manual, per release)

The ExtendScript layer is not automated; verify by hand:

- [ ] Panel opens and shows the Correct and Grade tabs plus the footer.
- [ ] With no comp open, the footer reads "Target: no active comp".
- [ ] With a comp open and no layer selected, the footer names the comp.
- [ ] Selecting a layer shows its name; selecting a different layer updates it within ~1s.
- [ ] Multi-selecting layers shows the first layer plus a "+N more selected" note.
- [ ] With a layer selected, "Analyze frame" renders and shows Footage stats
      (luma percentiles, LAB, band chroma, saturation, skin, clipping) for the
      current frame; the footer shows "Analyzed: current frame".
- [ ] Moving the current-time indicator and re-analyzing updates the stats.
- [ ] The 16-bit TIFF path leaves no `cg_frame_*.tif`/`.png` files in the OS temp
      folder afterward (temporary renders are cleaned up).
- [ ] Renaming/removing the `CG 16-bit TIFF` template still lets analysis run via
      the PNG fallback, and repeated failing analyzes leave no orphaned
      `cg_frame_*.tif` files in the OS temp folder.
- [ ] The Correct tab has a **single** footage-profile selector (Footage
      dropdown: Rec.709 / V-Log / ...) plus an **Apply correction** button - no
      separate "V-Log" checkbox. The dropdown is the one source of truth: it
      picks the profile the **Analyze frame** step decodes by AND the profile
      the Correct stack applies.
- [ ] On a saved project, selecting a layer, choosing **V-Log** in the Footage
      dropdown, and clicking **Apply correction** creates a `.colorgrade/`
      folder next to the .aep containing `decode_<layerId>.cube` (the ~7MB
      65-point Decode LUT), and the layer's Effect Controls show **Apply Color
      LUT [cg]** then **Lumetri Color [cg]**, in that order. This is the
      authoritative check for the `cep.fs.writeFile` err-1 fix: confirm the
      Decode LUT actually writes (no "Correct stack failed: cep.fs.writeFile
      failed (err 1) ..." error surfaces) and the stack applies. Confirm the
      Apply Color LUT effect's Choose LUT field actually resolved to the written
      file (its scriptable file property is not fully documented by Adobe -
      verify by hand each release) and that a flat V-Log clip visibly
      normalizes toward Rec.709.
- [ ] Switching the Footage dropdown back to **Rec.709** (standard) and clicking
      **Apply correction** removes the Apply Color LUT effect and leaves Lumetri
      Color [cg] in place; the .cube file is left on disk (still tracked in
      `.colorgrade/` for a future re-apply). Choosing **V-Log** and applying
      again re-adds Apply Color LUT immediately above Lumetri.
- [ ] With no project ever saved (no .aep path yet), applying a V-Log
      correction surfaces a clear "save the project" error instead of failing
      silently.
- [ ] Selecting a different layer resets the Footage dropdown to Rec.709 (the
      selector is per-clip; it never carries a prior clip's format over).
- [ ] With zero layers selected, the Footage dropdown and Apply correction
      button are disabled.
- [ ] On a saved project, selecting a clip on the **Grade** tab, picking a
      Theme, and clicking **Apply grade** creates a Managed **Grade [cg]**
      adjustment layer at the top of the comp with an **Apply Color LUT [cg]**
      effect, and the picture visibly takes on the Theme's look. The
      `.colorgrade/` folder gains `grade_<layerId>.cube` and its recipe
      `grade_<layerId>.json` (theme, knobs, measured stats). Confirm the Apply
      Color LUT effect's Choose LUT field actually resolved to the written
      `.cube` (scriptable file property, verify by hand each release).
- [ ] The graded clip's stats are measured **after** its Correct stack: choose
      **V-Log** in the Footage dropdown and **Apply correction** first, then
      Grade - the analyzed stats and resulting look reflect the decoded
      (Rec.709) footage, not the raw log signal.
- [ ] Re-applying a grade (same or different Theme) reuses the one Grade [cg]
      adjustment layer and repoints its Apply Color LUT, rather than stacking a
      second adjustment layer.
- [ ] On a **re-grade**, the analyzed stats reflect the post-Correct (ungraded)
      pixels: re-applying the *same* Theme does not compound or darken the look,
      because the Grade [cg] layer is disabled during the analysis render and
      re-enabled afterward.
- [ ] A re-grade with new `.cube` contents at the same `grade_<layerId>.cube`
      path visually refreshes the look (Apply Color LUT may cache the LUT by
      path - confirm the new grade actually takes effect on screen).
- [ ] Applying a grade leaves no `cg_*.cube`/`cg_*.json` scratch files in the
      CEP `userData` folder afterward (staged temp files are cleaned up).
- [ ] With no project ever saved, Apply grade surfaces a clear "save the
      project" error instead of failing silently. With zero layers selected,
      the Apply grade button is disabled.

## Debugging

With the `.debug` file in place and debug mode on, open `http://localhost:8092` in Chrome to inspect the panel.
