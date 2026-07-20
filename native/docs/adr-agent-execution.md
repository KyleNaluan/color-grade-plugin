# ADR: Agent execution bridge for the native editor

Status: accepted (cg-agent-wiring, 2026-07-19). Supersedes the "agent execution is
not wired into the native editor" stub state noted in the Phase-3/UI-polish notes.

## Context

The native editor's agent surfaces (Critique / Auto-grade / Reference / Batch) were
informational stubs: they described the offline pipeline (`npm run auto-grade`,
`src/agent`, `npm run check-consistency`) but did nothing in-editor. The captain wants
them to actually execute from the editor (verified in AE post-PR-40).

The agent pipeline is TypeScript: the vision critic (`src/agent/critic.ts`), the
rule-based accept/reject guard (`src/core/analysis/gradeGuard.ts`), the auto-grade loop
(`src/agent/loop.ts`), reference-stats measurement (`src/core/analysis/referenceStats.ts`
+ `computeStats`), and cross-clip consistency (`src/core/analysis/crossClipConsistency.ts`).
Per the native epic's discipline, **TS is the correctness oracle** and native code is a
faithful port gated by `native:core-parity`; the model/guard/consistency logic must NOT be
duplicated into C++.

## Decision

**The native editor executes the agent pipeline by spawning a short-lived Node subprocess**
running `scripts/agentBridge.ts`, which reuses `src/agent` + `src/core` verbatim. The editor
writes a request file, spawns the bridge on a worker thread, and reads a response file.

This is the same seam Node already occupies for this repo: every `native:*` parity/headless
test shells out to Node to run the TS oracle. The bridge is that oracle, invoked at runtime
instead of test time.

### Why not port to C++

Porting the Gemini HTTP + structured-output + `gradeGuard` classification + cross-clip
comparator into C++ would (a) duplicate the oracle the whole native epic is defined against,
(b) require a JSON/HTTP/TLS stack in the effect, and (c) create a second place every future
agent change must land. Rejected.

### Wire protocol

A tiny line-based `key value` text (`src/agent/bridgeProtocol.ts` ↔
`native/ColorGradeFX/editor/AgentBridge.h`), the same hand-rolled-parser spirit as the
`.cube` and reference-stats sidecar formats, so the C++ side needs **no JSON library**. The
committed fixtures in `native/tests/fixtures/agent/` are the cross-language contract: both
`tests/unit/agentBridge.test.ts` and `native:agent-test` parse them, so a format drift fails
one side.

- Request: `command`, `mode`, `model`, `profile`, `theme`, `strength`, `skinProtection`,
  `chromaGain`, `rounds`, `frame`, `reference`, `out`, `clip` (repeated), `mock`.
- Response: `status ok|error`, `message`, `defect` (repeated), `verdict`, `bestRound`,
  `accepted`, `stopReason`, `apply <field> <nums>` (repeated), `unmapped` (repeated),
  `cost`, `diverged A | B | reason` (repeated), `pairsCompared`.

The frame the editor sends is a **raw RGBA dump** (`CGF1` magic + int32 w/h/channels + bytes),
because native has no image encoder; the bridge encodes the PNG for Gemini. Reference/batch
input stills are real TIFF/PNG files the bridge decodes (TIFF via `tiff`, PNG via `fast-png`).

### BYOK / key handling

The Gemini API key rides `GEMINI_API_KEY` on the child process only (set right before
`CreateProcess`, single job at a time) - **never** written to the request file, never
committed, never firstmate's. The bridge reads it only from that env var.

### Deployment

The plugin resolves the bridge via env, mirroring `CG_LUT_PATH` / `CG_REF_STATS_PATH`:

- `CG_AGENT_BRIDGE` = absolute path to the runnable bridge (the repo's
  `scripts/agentBridge.ts`, or a prebuilt `.js`/`.mjs`). Unset / not-found → the panel shows
  "agent bridge not configured", never a silent no-op.
- `CG_AGENT_NODE` = launcher (default `node`; auto-upgraded to `npx tsx` when the bridge ends
  in `.ts`). The command runs under `cmd.exe /c` so `npx`/`.cmd` shims and `PATH` resolve, with
  the working directory set to the bridge's folder (so a repo-relative `tsx` finds
  `node_modules`).

Node + the repo are a **dev-box / pre-release** requirement (BYOK, captain-verified), consistent
with `data/cg-distribution-decision.md`. A bundled, Node-free runtime is out of scope for this pass.

## Per-command behaviour

- **Critique**: sends the current graded preview frame (what the user sees) + params. One
  Gemini vision call → named defects + the critic's advisory verdict. Accept/reject is the
  guard's job across rounds, so a single critique reports defects only (stated in the panel).
- **Auto-grade**: sends the decoded-source ("before") frame; the loop re-renders candidate
  params via `buildTransform` (the oracle), the critic names defects each round, and
  `gradeGuard` decides accept/reject (locked design: model names, rules decide). The accepted
  best round's params are translated into the editor's existing `ParamEdit` queue
  (`AgentBridge.h::translateAgentApply`) and applied to the recipe.
  - **Apply mapping** (`src/agent/editorApply.ts`): the editor composes agent edits onto the
    popup theme via USER recipe fields (`Recipe.h::applyEditorOverrides`) - user tints ADD onto
    the theme, user curves REPLACE it, the Chroma Gain slider is a RELATIVE multiplier. So the
    bridge emits tint DELTAS (`proposed - authored`), a chromaGain RATIO (`proposed / authored`),
    and absolute curves. Strength / Skin Protection pass straight through.
  - **Known limitation**: `chromaShape.softLimit` / `chromaShape.vibrance` have no editor
    control, so a non-neutral proposal for them is DISCLOSED in the panel ("Proposed, no editor
    control") rather than applied - nothing is silently dropped (DoD item 5). The engine's auto
    chroma-overshoot guard already tames the worst blowouts before the loop runs.
- **Reference**: the user picks a still (TIFF/PNG); the bridge measures it with the same
  `computeStats` the Auto bake composes and writes the reference-stats sidecar the effect's
  `LoadReferenceStats` already reads. The editor points `CG_REF_STATS_PATH` at the fresh sidecar
  and switches Theme → Reference. The env/sidecar path stays a power-user override. No hand-
  produced sidecar; a genuine measure failure surfaces as a panel error.
- **Batch**: the user picks 2+ same-scene clips; the bridge grades each with the current theme
  and runs `checkSceneConsistency`, returning the drift flags. **Deferred**: harvesting the
  current comp's tagged clips + rendering each through AE (a true native batch view) - the
  file-pick form is the minimal useful version.

## Testing

- Pure seams are headless-tested and CI-safe: `tests/unit/agentBridge.test.ts` (protocol
  round-trip + fixtures + the `editorApply` mapping) and `native:agent-test`
  (`AgentBridge.h` format/parse/translate + the same fixtures), g++/clang, NOT in CI - same
  convention as `native:editor-test`.
- Model calls are kept out of tests: the bridge's `mock 1` flag runs a scripted critic ($0),
  and the unit tests never spawn the subprocess or hit the network.
- The AE panel layer (Win32 subprocess spawn, file dialogs, D3D) is untestable-by-design and
  is **captain-verified in AE** (like PR 40), per AGENTS.md.

## Consequences

- One in-flight agent job per editor window (`agentBusy`); a second click no-ops until it
  finishes. Closing a window mid-job joins the worker (can block up to the 2-min spawn ceiling).
- Two editor windows running jobs at once use per-window temp files (keyed on the instance uid).
- The agent dock stays the single Pro/BYOK seam behind `kAgentDockEnabled`; nothing here couples
  a Pro-able feature into the free core (monetization constraint, `data/cg-monetization-decision.md`).
