/**
 * Offline batch auto-grade CLI - the agent-in-the-loop's first surface
 * (issue: cg-agent-loop-v2; the opt-in editor button comes later with the UI
 * overhaul). Spike-style: frame in -> render current grade -> vision critic
 * NAMES defects and proposes a knob delta -> re-render -> the rule-based guard
 * decides better/worse/stop. RULES decide; the model only names.
 *
 * Usage:
 *   npm run auto-grade -- <frame.tif> <theme|stats-only> [options]
 *
 * Options:
 *   --profile vlog|rec709   Decode profile (default: from filename prefix).
 *   --model <id>            Critic model (default gemini-flash-latest, free tier;
 *                           use gemini-3-flash-preview for the paid tier).
 *   --key <k>               API key literal. OR:
 *   --key-file <path>       Read the key from a file. OR set env GEMINI_API_KEY.
 *   --reference <frame.tif> Shot-match mode: target = this frame's stats.
 *   --degrade               Correction mode: synthetically damage the frame and
 *                           target its own pristine stats (reproduces s7 Exp A).
 *   --rounds <n>            Max tuned rounds after baseline (default 5; s7: 4-6).
 *   --strength <0..1>       Base strength (default from theme).
 *   --out <dir>            Output dir (default out/auto-grade).
 *   --mock                 Use a scripted critic (no API, $0) for a dry run.
 *   --validate-key         Just validate the key (reports reachability) and exit.
 *
 * The key is read only from --key / --key-file / GEMINI_API_KEY, never hardcoded.
 */
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { basename, join } from 'node:path';
import { encode as encodePng } from 'fast-png';
import { PROFILES } from '../src/core/color/index.js';
import { computeStats, decodeToRec709, type FootageStats } from '../src/core/analysis/stats.js';
import { buildTransform } from '../src/core/engine/engine.js';
import { computeGradeImpact } from '../src/core/analysis/gradeImpact.js';
import type { Theme } from '../src/core/engine/theme.js';
import type { Vec3 } from '../src/core/color/types.js';
import { THEMES } from '../src/themes/index.js';
import { loadTiff, downsample, type FrameBuffer } from './lib/loadTiff.js';
import {
  runAutoGradeLoop,
  critiqueFrame,
  validateGeminiKey,
  parseCriticProposal,
  costForUsage,
  estimateLoopCost,
  isPaidModel,
  maskKey,
  DEFAULT_MODEL,
  type AutoGradeParams,
  type CriticProposal,
  type RenderedRound,
} from '../src/agent/index.js';

function arg(flag: string): string | undefined {
  const i = process.argv.indexOf(flag);
  return i >= 0 ? process.argv[i + 1] : undefined;
}
function has(flag: string): boolean {
  return process.argv.includes(flag);
}

// --- key resolution (flag / file / env; never hardcoded) --------------------
function resolveKey(): string {
  const literal = arg('--key');
  if (literal) return literal.trim();
  const file = arg('--key-file');
  if (file) return readFileSync(file, 'utf8').trim();
  const env = process.env.GEMINI_API_KEY;
  if (env) return env.trim();
  return '';
}

// --- small render helpers (kept local; scripts may use Node/fast-png) --------
function decodedToPngBase64(decoded: Float32Array, width: number, height: number): string {
  const data = new Uint8Array(width * height * 3);
  for (let i = 0; i < decoded.length; i++) {
    data[i] = Math.round(Math.max(0, Math.min(1, decoded[i]!)) * 255);
  }
  return Buffer.from(encodePng({ width, height, data, channels: 3, depth: 8 })).toString('base64');
}

function applyTransformBuf(decoded: Float32Array, transform: (rgb: Vec3) => Vec3): Float32Array {
  const out = new Float32Array(decoded.length);
  for (let i = 0; i < decoded.length; i += 3) {
    const [r, g, b] = transform([decoded[i]!, decoded[i + 1]!, decoded[i + 2]!]);
    out[i] = r;
    out[i + 1] = g;
    out[i + 2] = b;
  }
  return out;
}

/** Degrade a decoded buffer (exposure/contrast/lift/cast) to synthesize s7 Exp A's damaged source. */
function degrade(decoded: Float32Array): Float32Array {
  const expMul = Math.pow(2, -0.9);
  const out = new Float32Array(decoded.length);
  for (let i = 0; i < decoded.length; i += 3) {
    for (let c = 0; c < 3; c++) {
      let v = decoded[i + c]! * expMul;
      v = (v - 0.5) * 0.7 + 0.5;
      v = v * (1 - 0.1) + 0.1;
      out[i + c] = v;
    }
    out[i] = Math.max(0, Math.min(1, out[i]! + 0));
    out[i + 1] = Math.max(0, Math.min(1, out[i + 1]! + 0.05));
    out[i + 2] = Math.max(0, Math.min(1, out[i + 2]! - 0.05));
  }
  return out;
}

function statsOnlyTheme(targetStats: FootageStats, name: string): Theme {
  return {
    name,
    description: 'agent auto-grade base (pure stat target)',
    targetStats,
    knobs: { strength: { default: 1 }, skinProtection: { default: 0.5 } },
  };
}

function themeWithOverrides(base: Theme, params: AutoGradeParams): Theme {
  return {
    ...base,
    overrides: { ...base.overrides, ...params.overrides },
  };
}

function decodeFrame(frame: FrameBuffer, profileKey: string): Float32Array {
  const profile = PROFILES[profileKey];
  if (!profile) throw new Error(`unknown profile ${profileKey}`);
  return decodeToRec709(frame.pixels, profile);
}

// --- scripted critic for --mock (mirrors the s7 defect->fix playbook) --------
function mockCritic(round: RenderedRound<unknown>): CriticProposal {
  const cast = round.impact.castMagnitude;
  const shadows = round.impact.outStats.bandChroma.shadows;
  const highlights = round.impact.outStats.bandChroma.highlights;
  const blown = shadows > 20 || highlights > 20;
  const defects: string[] = [];
  const overrides: AutoGradeParams['overrides'] = {};
  if (blown) {
    defects.push('chroma overshoot / neon blowout in shadows and highlights');
    overrides.chromaGain = 0.25;
    overrides.chromaShape = { softLimit: 22 };
  }
  if (cast > 6) {
    defects.push(`residual color cast (${cast.toFixed(1)} LAB units)`);
    overrides.shadowTint = [5, -5];
    overrides.highlightTint = [5, -5];
    overrides.midtoneTint = [4, -4];
  }
  const verdict = defects.length === 0 ? 'stop' : 'continue';
  return parseCriticProposal({
    critique: defects.length ? `defects present: ${defects.join(', ')}` : 'clean, no defects worth fixing',
    defects,
    proposedOpts: {},
    proposedOverrides: overrides,
    verdict,
    verdictReasoning: 'scripted mock critic',
  });
}

async function main(): Promise<void> {
  const [framePath, themeArg] = process.argv.slice(2).filter((a) => !a.startsWith('--'));
  const model = arg('--model') ?? DEFAULT_MODEL;
  const key = resolveKey();
  const mock = has('--mock');

  if (has('--validate-key')) {
    if (!key) fail('no key: pass --key, --key-file, or set GEMINI_API_KEY');
    console.log(`validating key ${maskKey(key)} ...`);
    const status = await validateGeminiKey(key, { probe: true, probeModel: model });
    console.log(JSON.stringify({ ...status, availableModels: `${status.availableModels.length} models` }, null, 2));
    process.exit(status.ok ? 0 : 1);
  }

  if (!framePath) fail('usage: npm run auto-grade -- <frame.tif> <theme|stats-only> [options]');
  const referencePath = arg('--reference');
  const useDegrade = has('--degrade');
  const rounds = arg('--rounds') ? parseInt(arg('--rounds')!, 10) : 5;
  const baseStrength = arg('--strength') ? parseFloat(arg('--strength')!) : undefined;
  const outDir = arg('--out') ?? 'out/auto-grade';
  mkdirSync(outDir, { recursive: true });

  const stem = basename(framePath).replace(/\.tiff?$/i, '');
  const profileName = arg('--profile') ?? (stem.startsWith('vlog') ? 'vlog' : 'rec709');

  const frame = downsample(loadTiff(framePath));
  let decoded = decodeFrame(frame, profileName);

  // Determine base theme + mode.
  let baseTheme: Theme;
  let mode: 'correction' | 'shot-match';
  let referencePngBase64: string | undefined;
  if (referencePath) {
    mode = 'shot-match';
    const refFrame = downsample(loadTiff(referencePath));
    const refProfile = arg('--profile') ?? (basename(referencePath).startsWith('vlog') ? 'vlog' : 'rec709');
    const refDecoded = decodeFrame(refFrame, refProfile);
    baseTheme = statsOnlyTheme(computeStats(refDecoded), `ref-${basename(referencePath)}`);
    referencePngBase64 = decodedToPngBase64(refDecoded, refFrame.width, refFrame.height);
  } else if (useDegrade) {
    mode = 'correction';
    const pristine = computeStats(decoded);
    decoded = degrade(decoded);
    baseTheme = statsOnlyTheme(pristine, `correct-${stem}`);
  } else {
    mode = 'correction';
    const t = themeArg && THEMES[themeArg] ? THEMES[themeArg] : themeArg === 'stats-only' ? undefined : THEMES[themeArg ?? ''];
    if (!t && themeArg !== 'stats-only') {
      fail(`unknown theme "${themeArg}". themes: ${Object.keys(THEMES).join(', ')}, or stats-only`);
    }
    baseTheme = t ?? statsOnlyTheme(computeStats(decoded), `self-${stem}`);
  }

  const width = frame.width;
  const height = frame.height;
  const baseParams: AutoGradeParams = {
    strength: baseStrength ?? baseTheme.knobs.strength.default,
    skinProtection: baseTheme.knobs.skinProtection.default,
    overrides: { ...baseTheme.overrides },
  };

  // Spend accounting.
  let totalCost = 0;
  let calls = 0;
  const HARD_CAP = 5.0;
  if (!mock && isPaidModel(model)) {
    const est = estimateLoopCost(model, rounds);
    console.log(`model ${model} is PAID; estimated loop cost ~$${est.toFixed(4)} (${rounds + 1} calls)`);
    if (est > HARD_CAP) fail(`estimated cost $${est.toFixed(4)} exceeds hard cap $${HARD_CAP}`);
  }
  if (!mock && !key) fail('no key: pass --key, --key-file, or set GEMINI_API_KEY (or use --mock)');

  console.log(`frame:   ${framePath} (${width}x${height})`);
  console.log(`profile: ${profileName}   mode: ${mode}   base: ${baseTheme.name}`);
  console.log(`model:   ${mock ? 'MOCK (scripted, $0)' : model}   rounds: ${rounds}`);

  const savePng = (base64: string, label: string): string => {
    const p = join(outDir, `${stem}__${label}.png`);
    writeFileSync(p, Buffer.from(base64, 'base64'));
    return p;
  };

  const render = (params: AutoGradeParams): RenderedRound<{ pngBase64: string }> => {
    const theme = themeWithOverrides(baseTheme, params);
    const srcStats = computeStats(decoded);
    const transform = buildTransform(srcStats, theme, {
      strength: params.strength,
      skinProtection: params.skinProtection,
    });
    const outDecoded = applyTransformBuf(decoded, transform);
    const impact = computeGradeImpact(decoded, transform);
    const pngBase64 = decodedToPngBase64(outDecoded, width, height);
    return { params, impact, frame: { pngBase64 } };
  };

  const critique = async (round: RenderedRound<{ pngBase64: string }>): Promise<CriticProposal> => {
    if (mock) return mockCritic(round);
    const impactSummary =
      `cast ${round.impact.castMagnitude.toFixed(1)} LAB units; ` +
      `bandChroma s/m/h ${round.impact.outStats.bandChroma.shadows.toFixed(0)}/${round.impact.outStats.bandChroma.mids.toFixed(0)}/${round.impact.outStats.bandChroma.highlights.toFixed(0)}; ` +
      `skin ${(round.impact.skinWeightTotal * 100).toFixed(1)}%`;
    const res = await critiqueFrame({
      key,
      model,
      pngBase64: round.frame.pngBase64,
      referencePngBase64,
      context: { mode, currentParams: round.params, round: calls, impactSummary },
    });
    calls += 1;
    const cost = costForUsage(model, res.usage);
    totalCost += cost;
    if (totalCost > HARD_CAP) fail(`spend cap hit: $${totalCost.toFixed(4)} > $${HARD_CAP}`);
    return res.proposal;
  };

  const result = await runAutoGradeLoop<{ pngBase64: string }>({ render, critique }, { baseParams, maxRounds: rounds });

  // Report.
  console.log(`\n=== rounds ===`);
  for (const r of result.rounds) {
    savePng(r.frame.pngBase64, `round${r.round}`);
    const verdict = r.comparison ? r.comparison.verdict : 'baseline';
    const acc = r.round === 0 ? '' : r.accepted ? ' [accepted]' : ' [REJECTED]';
    console.log(
      `round ${r.round}: cast ${r.impact.castMagnitude.toFixed(2)}  ` +
        `bandChroma ${r.impact.outStats.bandChroma.shadows.toFixed(0)}/${r.impact.outStats.bandChroma.mids.toFixed(0)}/${r.impact.outStats.bandChroma.highlights.toFixed(0)}  ` +
        `defects[${r.proposal.defects.length}]  -> ${verdict}${acc}`,
    );
    if (r.proposal.defects.length) console.log(`         defects: ${r.proposal.defects.join('; ')}`);
    if (r.proposal.clamped.length) console.log(`         clamped: ${r.proposal.clamped.join(', ')}`);
    if (r.comparison) console.log(`         guard: ${r.comparison.reasons.join('; ')}`);
  }
  console.log(`\nstop: ${result.stopReason}`);
  console.log(`best: round ${result.best.round} (cast ${result.best.impact.castMagnitude.toFixed(2)})`);

  const summary = {
    frame: framePath,
    mode,
    baseTheme: baseTheme.name,
    model: mock ? 'mock' : model,
    stopReason: result.stopReason,
    bestRound: result.best.round,
    bestParams: result.best.params,
    rounds: result.rounds.map((r) => ({
      round: r.round,
      params: r.params,
      castMagnitude: r.impact.castMagnitude,
      bandChroma: r.impact.outStats.bandChroma,
      skinHueShiftDeg: r.impact.skinHueShiftDeg,
      skinChromaShiftPct: r.impact.skinChromaShiftPct,
      defects: r.proposal.defects,
      clamped: r.proposal.clamped,
      verdict: r.comparison?.verdict ?? 'baseline',
      accepted: r.accepted,
    })),
    spend: { model: mock ? 'mock' : model, calls, estimatedUsd: Number(totalCost.toFixed(6)), paid: !mock && isPaidModel(model) },
  };
  const summaryPath = join(outDir, `${stem}__summary.json`);
  writeFileSync(summaryPath, JSON.stringify(summary, null, 2));
  console.log(`\nspend: ${calls} call(s), estimated $${totalCost.toFixed(4)} (${mock ? 'mock, $0' : isPaidModel(model) ? 'PAID' : 'free tier, $0'})`);
  console.log(`wrote ${summaryPath}`);
}

function fail(msg: string): never {
  console.error(msg);
  process.exit(1);
}

main().catch((err) => {
  console.error(err instanceof Error ? err.message : String(err));
  process.exit(1);
});
