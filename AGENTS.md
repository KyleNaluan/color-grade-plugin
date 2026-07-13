# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Domain vocabulary lives in `CONTEXT.md`; use those terms verbatim. Product context: PRD in issue #1 and `docs/prd.md`; ADRs in `docs/adr/`.
- Layout: `src/core` (pure color math), `src/themes` (data-only looks), `src/panel` (Preact UI), `src/host` (typed bridge boundary + CEP impl), `src/extendscript` (ES3 `host.jsx`, AE DOM ops only, zero color math).
- Pixel access goes through the `FrameSource` interface (`src/host/frameSource.ts`, ADR 0001): comp time in, normalized Float32 pixels out - consumers never learn the acquisition method. v1 backend is render-to-file (`renderFrameSource.ts` + `cepFrameFileIO.ts` + ExtendScript `CG_renderFrame`, 16-bit TIFF primary / PNG fallback, decoders in `frameFile.ts`). Tests use the file-backed `fileFrameSource.ts` fed with fixtures, so pixel consumers run without a host.
- Commands: `npm test` (Vitest), `npm run lint` (core-purity check), `npm run typecheck`, `npm run build` (Vite + `scripts/assemble-cep.mjs` -> installable CEP extension in `dist/`; install steps in `docs/cep-install.md`). Theme-tuning loop: `npm run spike -- <frame.tif> <theme>` (see `scripts/spike.ts` header). CI (`.github/workflows/ci.yml`) runs `npm test`, `npm run lint`, and `npm run typecheck` on Node 22 for pushes and PRs to `main`.
- Themes are data only (target stats + authored overrides + knobs); all look logic lives in `src/core/engine/engine.ts`. Strength 0 must stay exact identity and output bounded in [0,1] - tests enforce both.
- `src/core` purity is enforced by `scripts/check-core-purity.ts` (custom checker, not eslint: typescript-eslint's peer range rejects this repo's TypeScript 7). Core may only import relative modules inside `src/core`.
- Golden tests read local-only fixture frames (`tests/fixtures/frames/`, gitignored personal footage) and must keep skipping cleanly when the directory is empty; full suite counts differ with/without fixtures. Local copies live at `~/projects/color-grade-plugin/tests/fixtures/frames/`. Distinct from those: `tests/fixtures/frame-source/` holds small committed synthetic frames (TIFF + PNG) for the FrameSource/analyze tests - regenerate via `scripts/gen-frame-fixtures.ts`.
- The ExtendScript layer is untestable by design; changes there go through the manual smoke checklist in `docs/cep-install.md`.
- Correct stack (issue #5): `src/core/color/decode.ts` (`decodePixelToRec709`) is the single per-pixel decode-to-Rec.709 composition, shared by `analysis/stats.ts` (bulk footage decode) and `lut/decodeLut.ts` (`bakeDecodeLut`, which bakes it into a Decode LUT). Panel orchestration is `panel/correctStack.ts`; the bridge command is `Bridge.setCorrectProfile`, implemented in ExtendScript as `CG_setCorrectProfile` (matchNames `ADBE Apply Color LUT2`, `ADBE Lumetri`). Decode LUTs default to a 65-point grid, not the theme-grade 33-point default: a log profile's highlight rolloff is steep enough that 33 points under-resolves it (see `bakeDecodeLut`'s doc comment and `tests/unit/decodeLut.test.ts`).
- Apply Color LUT's file-picker property is not officially documented as scriptable; `CG_ensureDecodeLut` sets it via `effect.property(1).setValue(file)` as the best-known approach - verify it actually resolves the LUT file each release via the smoke checklist.

## Maintaining this file

Keep this file for knowledge useful to almost every future agent session in this project.
Do not repeat what the codebase already shows; point to the authoritative file or command instead.
Prefer rewriting or pruning existing entries over appending new ones.
When updating this file, preserve this bar for all agents and keep entries concise.
