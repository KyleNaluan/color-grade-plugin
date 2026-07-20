# cg-agent-fixes-v4 - fix round on PR #41 (native editor agent surfaces)

Four defects the captain found in the AE verification round, root-caused from code
(cannot run AE on Linux/WSL) and fixed on `fm/cg-agent-wiring`. Offline gates green.

## What changed, by issue

### 1. Agent functions dead - "agent bridge not configured - set CG_AGENT_BRIDGE…"
**Root cause:** the bridge path came ONLY from the `CG_AGENT_BRIDGE` env var, which the
AE panel process (launched from Explorer) never inherits from a shell.

**Fix:** the plug-in now resolves the bridge from a per-user **settings file**
`%APPDATA%\ColorGradeFX\agent.cfg`, with env kept as an override. Precedence
(`AgentBridge.h::chooseBridgePath`, each candidate validated to exist):
`CG_AGENT_BRIDGE` env → `bridge=` in the settings file → a bridge next to the `.aex`.
Launcher: `CG_AGENT_NODE` → `node=` → default `node` (auto `npx tsx` for `.ts`).

**Captain action (one-time, replaces the per-session env export):**
```
npm run native:agent-config      # run from the Node checkout with node_modules
```
It writes `bridge=`/`node=` into the settings file (Windows path form even under WSL),
preserving any saved key. Then restart AE. Files: `scripts/writeAgentConfig.ts`,
`ResolveAgentBridge`/`ResolveAgentLauncher` in `EditorWindow.cpp`, pure logic + tests in
`AgentBridge.h` / `agent_bridge_test.cpp`.

**Re-test:** every agent button runs (no "not configured"). With nothing configured, the
error now reads "agent bridge not found - run 'npm run native:agent-config'…".

### 2. Reference image selection does nothing
**Root cause:** the reference measure also spawns the bridge, so it died on the same
unconfigured-bridge cause as #1 (the rest of the chain is code-correct: pick → bridge
`computeStats` → sidecar → `CG_REF_STATS_PATH` env + Theme→Reference → visible grade).
The reference-stats sidecar format round-trips TS↔C++ (whitespace-separated 21 numbers).

**Fix:** #1 unblocks it; the dock auto-opens and shows Working…/"Reference measured".
**Re-test:** Reference tab (or Correct tab) → Pick reference image → after "Reference
measured", Theme flips to Reference Match and the clip takes the still's look. If it still
shows nothing, note whether the dock showed "Working…"/"Reference measured" or an error -
that distinguishes a spawn failure from a same-look (near-identity) measurement.

### 3. API key not persisted
**Fix:** the key is stored **DPAPI-encrypted** (`CryptProtectData`, user-scoped) as
`apiKeyEnc=` in the same `agent.cfg` - loaded on window open (`LoadPersistedKey`), written on
Save (`PersistKey`), deleted on Remove. Never plaintext, never logged, never committed.
**Re-test:** enter key → close/reopen editor and restart AE → key already set (masked);
Remove clears it permanently.

### 4. AE crashes on closing the window after using these features (MOST SERIOUS)
**Root cause:** `WindowImpl` owns the agent worker as a `std::thread` member. The user-close
reap path (`ReapFinishedLocked`) did `delete w` WITHOUT joining `agentThread` - a joinable
`std::thread` destructor calls `std::terminate()` (and the worker holds a raw `w`). Using an
agent feature makes the thread joinable, so closing the window afterward crashed AE.
(`DestroyWindowImplLocked` already joined it; the reap path did not.)

**Fix:** both teardown paths funnel through one `StopAgentWorkLocked(w)` helper:
`TerminateJobObject` the bridge tree (so the worker's 120s wait returns at once), then join
`agentThread`, before `delete w`. Files: `EditorWindow.cpp` (`StopAgentWorkLocked`,
`ReapFinishedLocked`, `DestroyWindowImplLocked`).
**Re-test:** after exercising the agent features, close the editor window - no crash. Also
close it MID-JOB (while "Working…") - the tree is killed and it closes cleanly.

## Notes / limits
- All Win32/DPAPI/teardown code is MSVC-only and captain-verified; it does NOT compile in the
  headless g++ tests. The pure parts (config parse/format, bridge precedence) ARE headless-
  tested (`npm run native:agent-test`). Build all 4 configs before AE verify.
- shell32/crypt32 link via `#pragma comment(lib,…)` in `EditorWindow.cpp` - no vcxproj change.
- Storing the bridge path under WSL: `writeAgentConfig.ts` writes the Windows path form so
  Windows Node can launch it; the bridge checkout must have `node_modules` (tsx must resolve).

## Offline gates run (green)
`npm run native:agent-test`, `npm test` (326), `npm run lint`, `npm run typecheck`.
Native `.aex` build + AE runtime are the captain's next round.
