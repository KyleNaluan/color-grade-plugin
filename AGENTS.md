# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Add durable project-specific notes here as they are discovered through real work.
- Build/test: `npm test` (vitest); typecheck with `./node_modules/.bin/tsc --noEmit` (no npm script). Theme-tuning loop: `npm run spike -- <frame.tif> <theme>` (see `scripts/spike.ts` header).
- Themes are data only (target stats + authored overrides + knobs); all look logic lives in `src/core/engine/engine.ts`. Strength 0 must stay exact identity and output bounded in [0,1] - tests enforce both.
- `tests/fixtures/frames/` holds personal footage, gitignored and never committed; golden tests must keep skipping cleanly when it is empty. Local copies live at `~/projects/color-grade-plugin/tests/fixtures/frames/`.
- Product context: PRD in issue #1 and `docs/prd.md`; ADRs in `docs/adr/`.

## Maintaining this file

Keep this file for knowledge useful to almost every future agent session in this project.
Do not repeat what the codebase already shows; point to the authoritative file or command instead.
Prefer rewriting or pruning existing entries over appending new ones.
When updating this file, preserve this bar for all agents and keep entries concise.
