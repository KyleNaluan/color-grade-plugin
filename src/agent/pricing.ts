/**
 * Gemini per-token pricing and cost accounting for the auto-grade critic.
 *
 * Prices are per 1M tokens, standard paid tier, <=200k-token prompt. Verified
 * live at 2026-07-19 against https://ai.google.dev/gemini-api/docs/pricing:
 * `gemini-3-flash-preview` = $0.50 in (text/image) / $3.00 out (thinking
 * tokens billed as output); the free tier (`gemini-flash-latest`) is billed
 * "Free of charge". These match the numbers the paid-critic scout used
 * (`data/cg-critic-paid-t6/harness/spend.mjs`).
 */
import type { GeminiUsage } from './types.js';

export interface ModelPricing {
  /** USD per 1M input tokens. */
  input: number;
  /** USD per 1M output tokens (includes thinking/thoughts tokens). */
  output: number;
  /** Whether this model is billed at all on the tier we call it with. */
  free: boolean;
}

/**
 * Known models. `gemini-flash-latest` is the default free-tier critic; billing
 * only applies on the user's own key if they opt into a paid model. Unknown
 * models cost $0 in our accounting (we never over-report spend we can't price).
 */
export const PRICING: Record<string, ModelPricing> = {
  'gemini-flash-latest': { input: 0, output: 0, free: true },
  'gemini-flash-lite-latest': { input: 0, output: 0, free: true },
  'gemini-3-flash-preview': { input: 0.5, output: 3.0, free: false },
  'gemini-3.1-pro-preview': { input: 2.0, output: 12.0, free: false },
};

export const DEFAULT_MODEL = 'gemini-flash-latest';
export const PAID_MODEL = 'gemini-3-flash-preview';

/** True if a model is billed on the tier we call it with (paid model on a billed key). */
export function isPaidModel(model: string): boolean {
  return PRICING[model] ? !PRICING[model].free : false;
}

/** USD cost of one call's token usage; $0 for free/unknown models. */
export function costForUsage(model: string, usage: GeminiUsage | undefined): number {
  const p = PRICING[model];
  if (!p || p.free || !usage) return 0;
  const inputTok = usage.promptTokenCount ?? 0;
  const outputTok = (usage.candidatesTokenCount ?? 0) + (usage.thoughtsTokenCount ?? 0);
  return (inputTok / 1e6) * p.input + (outputTok / 1e6) * p.output;
}

/**
 * A priori cost estimate for a full loop before running it, so the CLI/panel
 * can warn on a paid model. One image-bearing critic call is dominated by the
 * downsampled frame's tokens; `~1100` in / `~350` out per call is a
 * deliberately conservative envelope from the t6 measured usage.
 */
export function estimateLoopCost(model: string, rounds: number): number {
  const p = PRICING[model];
  if (!p || p.free) return 0;
  const perCallIn = 1100;
  const perCallOut = 350;
  // rounds + 1: the baseline frame is critiqued too.
  const calls = rounds + 1;
  return calls * ((perCallIn / 1e6) * p.input + (perCallOut / 1e6) * p.output);
}
