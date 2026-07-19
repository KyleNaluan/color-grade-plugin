/**
 * Shared types for the agent-in-the-loop auto-grade (issue: cg-agent-loop-v2).
 *
 * This module (`src/agent`) is the reusable seam between the offline batch CLI
 * (`scripts/autoGrade.ts`) and the future BYOK panel UI: it holds the vision
 * critic, the key-validation logic, cost accounting, and the rule-governed
 * loop. It may import from `src/core` (the engine types, the QC guard, the
 * grade-impact measurement) but stays free of Node and CEP APIs so both the CLI
 * and a browser panel can consume it - I/O (reading a key file, loading a TIFF)
 * lives in the caller. Network access is via the injectable `fetch` seam.
 */
import type { ThemeOverrides } from '../core/engine/theme.js';

/** Token accounting returned by the Gemini `generateContent` REST endpoint. */
export interface GeminiUsage {
  promptTokenCount?: number;
  candidatesTokenCount?: number;
  /** Reasoning ("thinking") tokens; billed as output on the paid tier. */
  thoughtsTokenCount?: number;
  totalTokenCount?: number;
}

/**
 * The full parameter set fed to `buildTransform` for one round. `overrides`
 * carries the `ThemeOverrides` the critic tunes; `strength`/`skinProtection`
 * override the base theme's knob defaults. This is exactly the knob space the
 * engine already exposes - the loop invents no new capability.
 */
export interface AutoGradeParams {
  strength?: number;
  skinProtection?: number;
  overrides: ThemeOverrides;
}

/** The minimal `fetch` shape the agent uses; injectable so tests run offline. */
export type FetchLike = (
  url: string,
  init: {
    method: string;
    headers: Record<string, string>;
    /** Request body; omitted for GET/HEAD (Node's fetch rejects a body on those). */
    body?: string;
    signal?: AbortSignal;
  },
) => Promise<{ ok: boolean; status: number; json: () => Promise<unknown> }>;
