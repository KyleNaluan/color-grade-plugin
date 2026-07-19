/**
 * The vision critic: the model half of the auto-grade loop. Its ONLY jobs are
 * to NAME the visible defects in a rendered frame and PROPOSE the next
 * parameter set to try. It does not decide better/worse/stop - that is the
 * rule-based guard's job (`src/core/analysis/gradeGuard.ts`), per the split
 * decided in `data/cg-agents-study/report.md` (secs 1b/1f/2a: every model tier
 * tested, free and paid, failed the stop/regression judgment 0/16-0/26, so it
 * is never given that role).
 *
 * The prompt embeds the real `EngineOptions`/`ThemeOverrides` knob space WITH
 * numeric ranges - the fix for the ~100x magnitude confusion m3/t6 found when
 * ranges were absent (study TL;DR #2). `parseCriticProposal` additionally
 * clamps every returned number into its documented range as a rules-side
 * backstop, so a stray out-of-range value can never reach `buildTransform`.
 */
import type { ThemeOverrides } from '../core/engine/theme.js';
import type { AutoGradeParams, FetchLike, GeminiUsage } from './types.js';

/**
 * The knob space, described for the model with numeric ranges. Mirrors
 * `EngineOptions` (strength/skinProtection) and `ThemeOverrides` in
 * `src/core/engine/theme.ts`; keep the ranges here in sync with those
 * docstrings.
 */
export const KNOB_TYPE_SPACE = `The color engine exposes exactly these knobs. All are OPTIONAL; omit a knob to leave it unchanged. Numbers are on the scales stated - do NOT rescale to 0-1.

EngineOptions:
  strength: number         // 0-1. Dilutes the whole grade toward the original footage. 1 = full grade.
  skinProtection: number   // 0-1. How strongly skin-tone pixels are shielded from the grade. 0.5 default.

ThemeOverrides (hand-authored nudges applied after stat matching):
  shadowTint: [a, b]       // LAB tint added in the shadows. Typically 1-20 LAB units; past ~20 = obvious cast.
  midtoneTint: [a, b]      // LAB tint added in the midtones (use for a cast that sits in the mids). 1-20 LAB units.
  highlightTint: [a, b]    // LAB tint added in the highlights. 1-20 LAB units.
  chromaGain: number       // Global LAB-chroma multiplier. 1 = none; 0.2-0.5 mutes/tames blowouts; up to ~1.5 boosts.
  chromaShape.softLimit: number   // Soft ceiling on LAB chroma, in LAB chroma units (same scale as tints), NOT 0-1. 10-40; lower clamps harder.
  chromaShape.vibrance: number    // Nonlinear saturation of low-chroma pixels. -1..1; 0 = none.
  toneCurve: [[x,y],...]          // Master tone curve on 0-1, monotone. e.g. [[0,0],[0.5,0.55],[1,1]].
  channelCurves.r|g|b: [[x,y],...] // Per-channel curves on 0-1 (use to cancel a stubborn RGB cast).

Guidance from prior tuning (data/cg-agent-grade-s7): the two dominant defect fixes are (1) tone-stretch chroma OVERSHOOT ("neon" blowout) -> lower chromaGain to ~0.2-0.4 and add chromaShape.softLimit ~20-26; (2) hue-noise BLOTCHES on near-neutral bright highlights -> also a chromaGain/softLimit constraint (skinProtection does NOT fix these - they are not on skin). Use tints to cancel a color cast; reach for channelCurves only for a cast the band tints cannot reach.`;

/** JSON schema for Gemini structured output. */
export const CRITIC_RESPONSE_SCHEMA = {
  type: 'object',
  properties: {
    critique: { type: 'string' },
    defects: { type: 'array', items: { type: 'string' } },
    proposedOpts: {
      type: 'object',
      properties: {
        strength: { type: 'number' },
        skinProtection: { type: 'number' },
      },
    },
    proposedOverrides: {
      type: 'object',
      properties: {
        shadowTint: { type: 'array', items: { type: 'number' } },
        midtoneTint: { type: 'array', items: { type: 'number' } },
        highlightTint: { type: 'array', items: { type: 'number' } },
        chromaGain: { type: 'number' },
        chromaShape: {
          type: 'object',
          properties: {
            softLimit: { type: 'number' },
            vibrance: { type: 'number' },
          },
        },
        toneCurve: { type: 'array', items: { type: 'array', items: { type: 'number' } } },
        channelCurves: {
          type: 'object',
          properties: {
            r: { type: 'array', items: { type: 'array', items: { type: 'number' } } },
            g: { type: 'array', items: { type: 'array', items: { type: 'number' } } },
            b: { type: 'array', items: { type: 'array', items: { type: 'number' } } },
          },
        },
      },
    },
    verdict: { type: 'string', enum: ['continue', 'stop'] },
    verdictReasoning: { type: 'string' },
  },
  required: ['critique', 'defects', 'proposedOpts', 'proposedOverrides', 'verdict', 'verdictReasoning'],
} as const;

/** Parsed, range-clamped critic output. */
export interface CriticProposal {
  critique: string;
  /** Named visible defects. Its LENGTH feeds the guard as a defect-count signal. */
  defects: string[];
  proposedOpts: { strength?: number; skinProtection?: number };
  proposedOverrides: ThemeOverrides;
  /** The model's advisory continue/stop; the guard, not this, decides the loop. */
  verdict: 'continue' | 'stop';
  verdictReasoning: string;
  /** Values the parser had to clamp back into range (evidence of magnitude confusion). */
  clamped: string[];
}

export interface CriticContext {
  /**
   * What the grade is for. 'correction' = recover a neutral, well-exposed,
   * pleasing image judged by absolute standards; 'shot-match' = match a
   * reference frame's mood (supply `referencePngBase64`).
   */
  mode: 'correction' | 'shot-match';
  /** The parameters that produced the current frame. */
  currentParams: AutoGradeParams;
  /** Round number for context (0 = baseline). */
  round: number;
  /** Optional numeric grade-impact summary of the current frame (extra grounding). */
  impactSummary?: string;
}

export function buildCriticPrompt(ctx: CriticContext): string {
  const modeLine =
    ctx.mode === 'correction'
      ? 'TASK: CORRECTION - recover a neutral, natural, well-exposed and pleasing grade. There is no reference image; judge by absolute colorist standards.'
      : 'TASK: SHOT MATCH - grade the current frame to match the mood of the reference frame shown first (color temperature, contrast, saturation), WITHOUT introducing local artifacts on the current frame.';
  return `You are the visual critic in an agent-in-the-loop color grading system. You look at the current graded frame and (1) NAME every visible defect (color cast, chroma overshoot / "neon" blowout, hue-noise blotches on highlights, crushed/clipped detail, unnatural skin), then (2) PROPOSE the single next parameter set to try that would most reduce those defects. You do NOT decide whether the grade is better or worse than before - a separate rule-based guard does that.

${modeLine}

${KNOB_TYPE_SPACE}

Current parameters that produced the frame below (round ${ctx.round}):
${JSON.stringify(ctx.currentParams, null, 2)}
${ctx.impactSummary ? `\nMeasured grade impact on this frame:\n${ctx.impactSummary}\n` : ''}
Return proposedOpts + proposedOverrides as the FULL values to use next (not deltas): include only the knobs you want to change from the current parameters; omit the rest. Keep every number inside the ranges stated above. If you see no defects worth fixing, return an empty defects list and verdict "stop".`;
}

// --- Range clamping (rules-side backstop against magnitude confusion) -------

function clampNum(v: unknown, lo: number, hi: number, label: string, clamped: string[]): number | undefined {
  if (typeof v !== 'number' || !Number.isFinite(v)) return undefined;
  const c = Math.min(hi, Math.max(lo, v));
  if (c !== v) clamped.push(`${label} ${v}->${c}`);
  return c;
}

function clampTint(v: unknown, label: string, clamped: string[]): [number, number] | undefined {
  if (!Array.isArray(v) || v.length < 2) return undefined;
  const a = clampNum(v[0], -40, 40, `${label}.a`, clamped);
  const b = clampNum(v[1], -40, 40, `${label}.b`, clamped);
  if (a === undefined || b === undefined) return undefined;
  return [a, b];
}

function clampCurve(v: unknown): [number, number][] | undefined {
  if (!Array.isArray(v) || v.length < 2) return undefined;
  const pts: [number, number][] = [];
  for (const p of v) {
    if (Array.isArray(p) && p.length >= 2 && typeof p[0] === 'number' && typeof p[1] === 'number') {
      pts.push([Math.min(1, Math.max(0, p[0])), Math.min(1, Math.max(0, p[1]))]);
    }
  }
  return pts.length >= 2 ? pts : undefined;
}

/**
 * Validate and range-clamp a raw structured-output object into a
 * `CriticProposal`. Clamping every number into its documented range here is the
 * rules-side backstop: even if the model returns a tint of 900 or a chromaGain
 * of 0.001 it can never reach the engine unsanitised, and the clamp is recorded
 * so the loop can surface magnitude confusion.
 */
export function parseCriticProposal(raw: unknown): CriticProposal {
  const clamped: string[] = [];
  const obj = (raw ?? {}) as Record<string, unknown>;
  const rawOpts = (obj.proposedOpts ?? {}) as Record<string, unknown>;
  const rawOv = (obj.proposedOverrides ?? {}) as Record<string, unknown>;
  const rawShape = (rawOv.chromaShape ?? {}) as Record<string, unknown>;
  const rawCh = (rawOv.channelCurves ?? {}) as Record<string, unknown>;

  const proposedOpts: CriticProposal['proposedOpts'] = {};
  const strength = clampNum(rawOpts.strength, 0, 1, 'strength', clamped);
  const skinProtection = clampNum(rawOpts.skinProtection, 0, 1, 'skinProtection', clamped);
  if (strength !== undefined) proposedOpts.strength = strength;
  if (skinProtection !== undefined) proposedOpts.skinProtection = skinProtection;

  const overrides: ThemeOverrides = {};
  const shadowTint = clampTint(rawOv.shadowTint, 'shadowTint', clamped);
  const midtoneTint = clampTint(rawOv.midtoneTint, 'midtoneTint', clamped);
  const highlightTint = clampTint(rawOv.highlightTint, 'highlightTint', clamped);
  const chromaGain = clampNum(rawOv.chromaGain, 0, 3, 'chromaGain', clamped);
  if (shadowTint) overrides.shadowTint = shadowTint;
  if (midtoneTint) overrides.midtoneTint = midtoneTint;
  if (highlightTint) overrides.highlightTint = highlightTint;
  if (chromaGain !== undefined) overrides.chromaGain = chromaGain;

  const softLimit = clampNum(rawShape.softLimit, 0, 128, 'softLimit', clamped);
  const vibrance = clampNum(rawShape.vibrance, -1, 1, 'vibrance', clamped);
  const shape: NonNullable<ThemeOverrides['chromaShape']> = {};
  if (softLimit !== undefined) shape.softLimit = softLimit;
  if (vibrance !== undefined) shape.vibrance = vibrance;
  if (Object.keys(shape).length > 0) overrides.chromaShape = shape;

  const toneCurve = clampCurve(rawOv.toneCurve);
  if (toneCurve) overrides.toneCurve = toneCurve;
  const channelCurves: NonNullable<ThemeOverrides['channelCurves']> = {};
  for (const ch of ['r', 'g', 'b'] as const) {
    const c = clampCurve(rawCh[ch]);
    if (c) channelCurves[ch] = c;
  }
  if (Object.keys(channelCurves).length > 0) overrides.channelCurves = channelCurves;

  const defects = Array.isArray(obj.defects) ? obj.defects.filter((d): d is string => typeof d === 'string') : [];
  const verdict = obj.verdict === 'stop' ? 'stop' : 'continue';

  return {
    critique: typeof obj.critique === 'string' ? obj.critique : '',
    defects,
    proposedOpts,
    proposedOverrides: overrides,
    verdict,
    verdictReasoning: typeof obj.verdictReasoning === 'string' ? obj.verdictReasoning : '',
    clamped,
  };
}

export interface CritiqueRequest {
  key: string;
  model: string;
  /** Base64 PNG of the current graded frame. */
  pngBase64: string;
  /** Base64 PNG of the reference look, for shot-match mode. */
  referencePngBase64?: string;
  context: CriticContext;
  fetchImpl?: FetchLike;
  timeoutMs?: number;
  temperature?: number;
}

export interface CritiqueResponse {
  proposal: CriticProposal;
  usage?: GeminiUsage;
  latencyMs: number;
  model: string;
}

const GEMINI_BASE = 'https://generativelanguage.googleapis.com/v1beta';
const defaultFetch: FetchLike = (url, init) => {
  const reqInit: RequestInit = { method: init.method, headers: init.headers, signal: init.signal };
  if (init.body !== undefined && init.method !== 'GET' && init.method !== 'HEAD') reqInit.body = init.body;
  return fetch(url, reqInit) as unknown as ReturnType<FetchLike>;
};

/** One vision-critic call: current frame (+ optional reference) in, structured proposal out. */
export async function critiqueFrame(req: CritiqueRequest): Promise<CritiqueResponse> {
  const fetchImpl = req.fetchImpl ?? defaultFetch;
  const parts: Array<{ text: string } | { inline_data: { mime_type: string; data: string } }> = [];
  parts.push({ text: buildCriticPrompt(req.context) });
  if (req.context.mode === 'shot-match' && req.referencePngBase64) {
    parts.push({ text: '\nREFERENCE LOOK (match this frame’s mood):' });
    parts.push({ inline_data: { mime_type: 'image/png', data: req.referencePngBase64 } });
  }
  parts.push({ text: `\nCURRENT GRADED FRAME (round ${req.context.round}):` });
  parts.push({ inline_data: { mime_type: 'image/png', data: req.pngBase64 } });

  const url = `${GEMINI_BASE}/models/${req.model}:generateContent?key=${encodeURIComponent(req.key)}`;
  const start = Date.now();
  const res = await fetchImpl(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      contents: [{ parts }],
      generationConfig: {
        responseMimeType: 'application/json',
        responseSchema: CRITIC_RESPONSE_SCHEMA,
        temperature: req.temperature ?? 0.2,
      },
    }),
    signal: timeoutSignal(req.timeoutMs ?? 60000),
  });
  const json = (await res.json()) as {
    error?: { message?: string };
    candidates?: Array<{ content?: { parts?: Array<{ text?: string }> } }>;
    usageMetadata?: GeminiUsage;
  };
  const latencyMs = Date.now() - start;
  if (!res.ok || json.error) {
    throw new Error(`Gemini critique failed: ${json.error?.message ?? `HTTP ${res.status}`}`);
  }
  const text = json.candidates?.[0]?.content?.parts?.find((p) => typeof p.text === 'string')?.text;
  if (!text) throw new Error('Gemini critique returned no text part');
  const proposal = parseCriticProposal(JSON.parse(text));
  return { proposal, usage: json.usageMetadata, latencyMs, model: req.model };
}

function timeoutSignal(ms: number): AbortSignal | undefined {
  try {
    return AbortSignal.timeout(ms);
  } catch {
    return undefined;
  }
}
