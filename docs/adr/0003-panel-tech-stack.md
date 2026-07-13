# 0003. Panel stack: TypeScript + Vite + Preact, thin ExtendScript bridge

Date: 2026-07-12
Status: Accepted

## Context

CEP panels are an embedded Chromium with Node.js; the AE side is reached via `CSInterface.evalScript` string calls into an ES3-era ExtendScript engine.
The panel UI is stateful (tabs, per-clip state, knobs) plus canvas-heavy widgets (scopes, curve editor, wheels).
The analysis engine is math-heavy and needs to be testable.

## Decision

- Panel code in TypeScript, bundled with Vite targeting the CEP CEF runtime.
- UI in Preact; scopes/curves/wheels are raw canvas regardless of framework.
- ExtendScript stays a thin, dumb bridge: AE DOM operations only (apply effect, set sliders, queue renders). Zero color math or business logic on the AE side.
- CEP's built-in Node is used for file I/O (reading rendered TIFFs via a decoder lib such as utif, writing .cube LUTs and sidecars).

## Consequences

- All intelligence lives panel-side in modern, typed, unit-testable code.
- The ExtendScript surface is small enough to hand-verify, compensating for its untestability.
- Vanilla-DOM state-syncing rot is avoided at the cost of a small framework dependency.
