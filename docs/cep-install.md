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

## Smoke checklist (manual, per release)

The ExtendScript layer is not automated; verify by hand:

- [ ] Panel opens and shows the Correct and Grade tabs plus the footer.
- [ ] With no comp open, the footer reads "Target: no active comp".
- [ ] With a comp open and no layer selected, the footer names the comp.
- [ ] Selecting a layer shows its name; selecting a different layer updates it within ~1s.
- [ ] Multi-selecting layers shows the first layer plus a "+N more selected" note.

## Debugging

With the `.debug` file in place and debug mode on, open `http://localhost:8092` in Chrome to inspect the panel.
