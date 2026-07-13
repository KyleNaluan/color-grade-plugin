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
- [ ] On a saved project, selecting a layer and checking the Correct tab's
      **V-Log** toggle creates a `.colorgrade/` folder next to the .aep
      containing `decode_<layerId>.cube`, and the layer's Effect Controls show
      **Apply Color LUT [cg]** then **Lumetri Color [cg]**, in that order.
      Confirm the Apply Color LUT effect's Choose LUT field actually resolved
      to the written file (its scriptable file property is not fully
      documented by Adobe - verify by hand each release) and that a flat
      V-Log clip visibly normalizes toward Rec.709.
- [ ] Unchecking V-Log on that layer removes the Apply Color LUT effect and
      leaves Lumetri Color [cg] in place; the .cube file is left on disk
      (still tracked in `.colorgrade/` for a future toggle back).
      Re-checking V-Log re-adds Apply Color LUT immediately above Lumetri.
- [ ] With no project ever saved (no .aep path yet), checking V-Log surfaces
      a clear "save the project" error instead of failing silently.
- [ ] With zero layers selected, the V-Log toggle is disabled.

## Debugging

With the `.debug` file in place and debug mode on, open `http://localhost:8092` in Chrome to inspect the panel.
