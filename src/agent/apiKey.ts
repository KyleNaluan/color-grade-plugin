/**
 * BYOK key handling for the agent features (decision: `data/cg-distribution-decision.md`).
 *
 * Reusable core logic the future panel settings UI will consume directly:
 * - {@link maskKey} renders a key for display without leaking it (view state).
 * - {@link validateGeminiKey} makes a cheap test call that confirms the key
 *   works and reports what it can reach (validate-on-entry / swap).
 *
 * This module never reads a key from disk or env and never hardcodes one - the
 * caller resolves the key (the CLI from a flag/env/file, the panel from its own
 * stored setting) and passes it in. Network is the injectable `fetch` seam so
 * the panel can supply CEP's fetch and tests can run offline.
 */
import type { FetchLike, GeminiUsage } from './types.js';

const GEMINI_BASE = 'https://generativelanguage.googleapis.com/v1beta';

/**
 * Mask a key for display: keep the first 4 and last 4 characters, redact the
 * middle. Short keys are fully redacted rather than partially revealed. Pure,
 * safe to call in any UI (never logs or transmits).
 */
export function maskKey(key: string): string {
  const k = key.trim();
  if (k.length === 0) return '';
  if (k.length <= 10) return '*'.repeat(k.length);
  return `${k.slice(0, 4)}${'*'.repeat(Math.max(4, k.length - 8))}${k.slice(-4)}`;
}

/**
 * Shallow shape check for a Gemini API key. Accepts both the classic
 * `AIza...` keys and the newer AI-Studio `AQ.<...>` express keys. NOT
 * authoritative - only {@link validateGeminiKey} confirms a key actually works.
 */
export function looksLikeGeminiKey(key: string): boolean {
  const k = key.trim();
  return /^AIza[A-Za-z0-9_-]{20,}$/.test(k) || /^AQ\.[A-Za-z0-9_.-]{20,}$/.test(k);
}

export interface GeminiKeyStatus {
  /** The key authenticated and the account is reachable. */
  ok: boolean;
  /**
   * Best-effort capability tier. The REST API exposes no billing flag, so this
   * reflects reachable model quality, not free-vs-paid billing: `paid` only
   * when the paid-grade model is reachable AND a generation probe confirmed it,
   * `free` when only free-grade models are reachable, `unknown` when we could
   * not determine it (e.g. probe skipped). Definitive free-vs-paid still
   * requires the user to have enabled billing on their own key.
   */
  tier: 'free' | 'paid' | 'unknown';
  /** Model ids the key can list (`models/` prefix stripped). */
  availableModels: string[];
  /** A generation probe (`generateContent`) actually returned - the key works for critique, not just listing. */
  generationConfirmed: boolean;
  usage?: GeminiUsage;
  latencyMs: number;
  error?: string;
}

export interface ValidateKeyOptions {
  /** Injected fetch (defaults to global). */
  fetchImpl?: FetchLike;
  /**
   * Also run a one-token generation probe to confirm the key works for
   * `generateContent`, not merely for listing models. On the free tier this is
   * $0. Off by default so validation stays a pure zero-generation listModels call.
   */
  probe?: boolean;
  /** Model to probe with when `probe` is set. */
  probeModel?: string;
  /** Model whose reachability decides the capability `tier` (default paid-grade critic). */
  paidGradeModel?: string;
  timeoutMs?: number;
}

const defaultFetch: FetchLike = (url, init) => {
  const reqInit: RequestInit = { method: init.method, headers: init.headers, signal: init.signal };
  // Node's fetch throws if a GET/HEAD carries a body, even an empty one.
  if (init.body !== undefined && init.method !== 'GET' && init.method !== 'HEAD') reqInit.body = init.body;
  return fetch(url, reqInit) as unknown as ReturnType<FetchLike>;
};

/**
 * Confirm a Gemini key works and report what it can reach. The base check is a
 * zero-cost `listModels` GET (no tokens billed); with `probe` it additionally
 * runs a one-token `generateContent` to prove generation works. Never throws -
 * failures come back as `{ ok: false, error }` so a UI can show them inline.
 */
export async function validateGeminiKey(key: string, opts: ValidateKeyOptions = {}): Promise<GeminiKeyStatus> {
  const fetchImpl = opts.fetchImpl ?? defaultFetch;
  const paidGradeModel = opts.paidGradeModel ?? 'gemini-3-flash-preview';
  const probeModel = opts.probeModel ?? 'gemini-flash-latest';
  const timeoutMs = opts.timeoutMs ?? 30000;
  const trimmed = key.trim();
  const start = Date.now();

  const base: GeminiKeyStatus = {
    ok: false,
    tier: 'unknown',
    availableModels: [],
    generationConfirmed: false,
    latencyMs: 0,
  };

  if (trimmed.length === 0) {
    return { ...base, error: 'empty key', latencyMs: Date.now() - start };
  }

  try {
    const listRes = await fetchImpl(
      `${GEMINI_BASE}/models?key=${encodeURIComponent(trimmed)}&pageSize=200`,
      { method: 'GET', headers: {}, signal: timeoutSignal(timeoutMs) },
    );
    const listJson = (await listRes.json()) as {
      error?: { message?: string };
      models?: Array<{ name?: string }>;
    };
    if (!listRes.ok || listJson.error) {
      return {
        ...base,
        error: listJson.error?.message ?? `listModels HTTP ${listRes.status}`,
        latencyMs: Date.now() - start,
      };
    }
    const availableModels = (listJson.models ?? [])
      .map((m) => (m.name ?? '').replace(/^models\//, ''))
      .filter(Boolean);
    const hasPaidGrade = availableModels.includes(paidGradeModel);

    let generationConfirmed = false;
    let usage: GeminiUsage | undefined;
    if (opts.probe) {
      const probe = await probeGeneration(fetchImpl, trimmed, probeModel, timeoutMs);
      generationConfirmed = probe.ok;
      usage = probe.usage;
      if (!probe.ok) {
        return {
          ...base,
          ok: true,
          availableModels,
          error: `key lists models but generation probe failed: ${probe.error}`,
          latencyMs: Date.now() - start,
        };
      }
    }

    // Capability tier: reachable model quality, not billing (see field doc).
    const tier: GeminiKeyStatus['tier'] = !hasPaidGrade
      ? 'free'
      : opts.probe && generationConfirmed
        ? 'paid'
        : 'unknown';

    return {
      ok: true,
      tier,
      availableModels,
      generationConfirmed,
      usage,
      latencyMs: Date.now() - start,
    };
  } catch (err) {
    return { ...base, error: String(err), latencyMs: Date.now() - start };
  }
}

async function probeGeneration(
  fetchImpl: FetchLike,
  key: string,
  model: string,
  timeoutMs: number,
): Promise<{ ok: boolean; usage?: GeminiUsage; error?: string }> {
  try {
    const res = await fetchImpl(`${GEMINI_BASE}/models/${model}:generateContent?key=${encodeURIComponent(key)}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        contents: [{ parts: [{ text: 'Reply with the single word: ok' }] }],
        generationConfig: { maxOutputTokens: 4, temperature: 0 },
      }),
      signal: timeoutSignal(timeoutMs),
    });
    const json = (await res.json()) as { error?: { message?: string }; usageMetadata?: GeminiUsage };
    if (!res.ok || json.error) return { ok: false, error: json.error?.message ?? `HTTP ${res.status}` };
    return { ok: true, usage: json.usageMetadata };
  } catch (err) {
    return { ok: false, error: String(err) };
  }
}

function timeoutSignal(ms: number): AbortSignal | undefined {
  try {
    return AbortSignal.timeout(ms);
  } catch {
    return undefined;
  }
}
