# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Domain vocabulary lives in `CONTEXT.md`; use those terms verbatim. Product context: PRD in issue #1 and `docs/prd.md`; ADRs in `docs/adr/`.
- Layout: `src/core` (pure color math), `src/themes` (data-only looks), `src/panel` (Preact UI), `src/host` (typed bridge boundary + CEP impl), `src/extendscript` (ES3 `host.jsx`, AE DOM ops only, zero color math).
- Commands: `npm test` (Vitest), `npm run lint` (core-purity check), `npm run typecheck`, `npm run build` (Vite + `scripts/assemble-cep.mjs` -> installable CEP extension in `dist/`; install steps in `docs/cep-install.md`). Theme-tuning loop: `npm run spike -- <frame.tif> <theme>` (see `scripts/spike.ts` header).
- Themes are data only (target stats + authored overrides + knobs); all look logic lives in `src/core/engine/engine.ts`. Strength 0 must stay exact identity and output bounded in [0,1] - tests enforce both.
- `src/core` purity is enforced by `scripts/check-core-purity.ts` (custom checker, not eslint: typescript-eslint's peer range rejects this repo's TypeScript 7). Core may only import relative modules inside `src/core`.
- Golden tests read local-only fixture frames (`tests/fixtures/frames/`, gitignored personal footage) and must keep skipping cleanly when the directory is empty; full suite counts differ with/without fixtures. Local copies live at `~/projects/color-grade-plugin/tests/fixtures/frames/`.
- The ExtendScript layer is untestable by design; changes there go through the manual smoke checklist in `docs/cep-install.md`.

## Maintaining this file

Keep this file for knowledge useful to almost every future agent session in this project.
Do not repeat what the codebase already shows; point to the authoritative file or command instead.
Prefer rewriting or pruning existing entries over appending new ones.
When updating this file, preserve this bar for all agents and keep entries concise.
