/**
 * The native-editor agent execution bridge (issue: cg-agent-wiring).
 *
 * The native AE editor spawns this short-lived Node process to run the agent
 * pipeline it cannot run itself - the vision critic, the rule-based grade guard,
 * the auto-grade loop, reference-stats measurement, and cross-clip consistency
 * all live in TypeScript (`src/agent` + `src/core`, the correctness oracle). It
 * reuses that code verbatim; no model/guard logic is duplicated into C++. See
 * `native/docs/adr-agent-execution.md`.
 *
 * Usage:
 *   tsx scripts/agentBridge.ts <request.txt> <response.txt>
 *
 * The request/response wire format is the tiny line-based `key value` text in
 * `src/agent/bridgeProtocol.ts` (parsed hand-rolled on the C++ side, no JSON dep).
 * The Gemini API key is read ONLY from the GEMINI_API_KEY environment variable
 * (BYOK) - never from the request file, never hardcoded. The frame the editor
 * passes is a raw RGBA dump (see `readFrameDump`), because native has no image
 * encoder; reference/batch input stills are real TIFF/PNG files this decodes.
 *
 * Always writes a response file (status ok|error) and exits 0 when it could write
 * one, so the editor always gets structured feedback and never a silent no-op.
 */
import { readFileSync, writeFileSync } from 'node:fs';
import { basename } from 'node:path';
import { decode as decodePng, encode as encodePng } from 'fast-png';
import { decode as decodeTiff } from 'tiff';
import { PROFILES } from '../src/core/color/index.js';
import { computeStats, decodeToRec709, type FootageStats } from '../src/core/analysis/stats.js';
import { buildTransform } from '../src/core/engine/engine.js';
import { computeGradeImpact } from '../src/core/analysis/gradeImpact.js';
import { writeReferenceStatsFile } from '../src/core/analysis/referenceStats.js';
import {
  checkSceneConsistency,
  type ClipGradeSummary,
} from '../src/core/analysis/crossClipConsistency.js';
import type { Theme } from '../src/core/engine/theme.js';
import type { Vec3 } from '../src/core/color/types.js';
import { THEMES } from '../src/themes/index.js';
import {
  runAutoGradeLoop,
  critiqueFrame,
  costForUsage,
  DEFAULT_MODEL,
  parseAgentRequest,
  formatAgentResponse,
  errorResponse,
  editorApplyFromResult,
  type AgentBridgeRequest,
  type AgentBridgeResponse,
  type AutoGradeParams,
  type CriticProposal,
  type RenderedRound,
} from '../src/agent/index.js';

// --- raw frame dump (native RGBA) -------------------------------------------

interface DecodedFrame {
  width: number;
  height: number;
  /** Interleaved RGB floats in [0,1]. */
  pixels: Float32Array;
}

/**
 * Read the native editor's raw frame dump: a 16-byte header ("CGF1" magic + three
 * little-endian int32 width/height/channels) then tightly-packed 8-bit pixels.
 * Channels is 4 (RGBA, alpha dropped) or 3 (RGB).
 */
function readFrameDump(path: string): DecodedFrame {
  const buf = readFileSync(path);
  if (buf.length < 16 || buf.toString('ascii', 0, 4) !== 'CGF1') {
    throw new Error(`bad frame dump (magic) at ${path}`);
  }
  const width = buf.readInt32LE(4);
  const height = buf.readInt32LE(8);
  const channels = buf.readInt32LE(12);
  if (width <= 0 || height <= 0 || (channels !== 3 && channels !== 4)) {
    throw new Error(`bad frame dump dims ${width}x${height}x${channels}`);
  }
  const need = 16 + width * height * channels;
  if (buf.length < need) throw new Error(`frame dump truncated: ${buf.length} < ${need}`);
  const pixels = new Float32Array(width * height * 3);
  for (let p = 0; p < width * height; p++) {
    for (let c = 0; c < 3; c++) {
      pixels[p * 3 + c] = buf[16 + p * channels + c]! / 255;
    }
  }
  return { width, height, pixels };
}

/** Decode a reference/clip still (TIFF or PNG) into interleaved RGB floats. */
function loadImageFile(path: string): DecodedFrame {
  const lower = path.toLowerCase();
  const bytes = readFileSync(path);
  if (lower.endsWith('.tif') || lower.endsWith('.tiff')) {
    const ifds = decodeTiff(bytes);
    const img = ifds[0];
    if (!img) throw new Error(`no image in ${path}`);
    const { width, height, components, bitsPerSample } = img;
    if (components < 3) throw new Error(`expected RGB(A) TIFF, got ${components} components`);
    const maxVal = bitsPerSample === 16 ? 65535 : 255;
    const src = img.data as Uint16Array | Uint8Array;
    const pixels = new Float32Array(width * height * 3);
    for (let p = 0; p < width * height; p++) {
      for (let c = 0; c < 3; c++) pixels[p * 3 + c] = src[p * components + c]! / maxVal;
    }
    return { width, height, pixels };
  }
  if (lower.endsWith('.png')) {
    const img = decodePng(bytes);
    const comps = img.channels ?? Math.round(img.data.length / (img.width * img.height));
    const maxVal = img.depth === 16 ? 65535 : 255;
    const src = img.data as Uint16Array | Uint8Array;
    const pixels = new Float32Array(img.width * img.height * 3);
    for (let p = 0; p < img.width * img.height; p++) {
      for (let c = 0; c < 3; c++) pixels[p * 3 + c] = src[p * comps + c]! / maxVal;
    }
    return { width: img.width, height: img.height, pixels };
  }
  throw new Error(`unsupported reference format (need .tif/.tiff/.png): ${basename(path)}`);
}

// --- small render helpers (shared with scripts/autoGrade.ts's approach) ------

function decodeFrame(frame: DecodedFrame, profileKey: string): Float32Array {
  const profile = PROFILES[profileKey];
  if (!profile) throw new Error(`unknown profile ${profileKey}`);
  return decodeToRec709(frame.pixels, profile);
}

function pngBase64(decoded: Float32Array, width: number, height: number): string {
  const data = new Uint8Array(width * height * 3);
  for (let i = 0; i < decoded.length; i++) data[i] = Math.round(Math.max(0, Math.min(1, decoded[i]!)) * 255);
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

function statsOnlyTheme(targetStats: FootageStats, name: string): Theme {
  return {
    name,
    description: 'agent auto-grade base (pure stat target)',
    targetStats,
    knobs: { strength: { default: 1 }, skinProtection: { default: 0.5 } },
  };
}

function themeWithOverrides(base: Theme, params: AutoGradeParams): Theme {
  return { ...base, overrides: { ...base.overrides, ...params.overrides } };
}

/** Scripted critic for --mock / mock requests: names cast/blowout defects from measured impact. */
function mockCritic(round: RenderedRound<unknown>): CriticProposal {
  const cast = round.impact.castMagnitude;
  const sh = round.impact.outStats.bandChroma.shadows;
  const hi = round.impact.outStats.bandChroma.highlights;
  const defects: string[] = [];
  const overrides: AutoGradeParams['overrides'] = {};
  if (sh > 20 || hi > 20) {
    defects.push('chroma overshoot / neon blowout in shadows and highlights');
    overrides.chromaGain = 0.3;
    overrides.chromaShape = { softLimit: 22 };
  }
  if (cast > 6) {
    defects.push(`residual color cast (${cast.toFixed(1)} LAB units)`);
    overrides.shadowTint = [5, -5];
    overrides.midtoneTint = [4, -4];
    overrides.highlightTint = [5, -5];
  }
  return {
    critique: defects.length ? `defects present: ${defects.join(', ')}` : 'clean, no defects worth fixing',
    defects,
    proposedOpts: {},
    proposedOverrides: overrides,
    verdict: defects.length === 0 ? 'stop' : 'continue',
    verdictReasoning: 'scripted mock critic',
    clamped: [],
  };
}

// --- command handlers --------------------------------------------------------

function requireKey(): string {
  const key = (process.env.GEMINI_API_KEY ?? '').trim();
  if (!key) throw new Error('no Gemini API key: set it in the panel (passed via GEMINI_API_KEY)');
  return key;
}

async function runCritique(req: AgentBridgeRequest): Promise<AgentBridgeResponse> {
  if (!req.framePath) throw new Error('critique: no frame provided');
  const frame = readFrameDump(req.framePath);
  const graded = frame.pixels; // already the graded frame the user sees
  const mode = req.mode ?? 'correction';
  let referencePngBase64: string | undefined;
  if (mode === 'shot-match' && req.referencePath) {
    const ref = loadImageFile(req.referencePath);
    referencePngBase64 = pngBase64(decodeFrame(ref, req.profile ?? 'rec709'), ref.width, ref.height);
  }
  const currentParams: AutoGradeParams = {
    strength: req.strength,
    skinProtection: req.skinProtection,
    overrides: req.chromaGain !== undefined ? { chromaGain: req.chromaGain } : {},
  };
  if (req.mock) {
    return {
      status: 'ok',
      message: 'mock critique',
      defects: ['residual color cast (mock)', 'mild highlight chroma (mock)'],
      verdict: 'continue',
      apply: [],
      unmapped: [],
      diverged: [],
      cost: 0,
    };
  }
  const key = requireKey();
  const model = req.model ?? DEFAULT_MODEL;
  const res = await critiqueFrame({
    key,
    model,
    pngBase64: pngBase64(graded, frame.width, frame.height),
    referencePngBase64,
    context: { mode, currentParams, round: 0 },
  });
  return {
    status: 'ok',
    message: res.proposal.defects.length
      ? `${res.proposal.defects.length} defect(s) named`
      : 'no defects named',
    defects: res.proposal.defects,
    verdict: res.proposal.verdict,
    apply: [],
    unmapped: [],
    diverged: [],
    cost: costForUsage(model, res.usage),
  };
}

async function runAutograde(req: AgentBridgeRequest): Promise<AgentBridgeResponse> {
  if (!req.framePath) throw new Error('autograde: no frame provided');
  const frame = readFrameDump(req.framePath);
  const decoded = decodeFrame(frame, req.profile ?? 'rec709');
  const mode = req.mode ?? 'correction';

  let baseTheme: Theme;
  let referencePngBase64: string | undefined;
  if (mode === 'shot-match' && req.referencePath) {
    const ref = loadImageFile(req.referencePath);
    const refDecoded = decodeFrame(ref, req.profile ?? 'rec709');
    baseTheme = statsOnlyTheme(computeStats(refDecoded), `ref-${basename(req.referencePath)}`);
    referencePngBase64 = pngBase64(refDecoded, ref.width, ref.height);
  } else if (req.theme && THEMES[req.theme]) {
    baseTheme = THEMES[req.theme]!;
  } else {
    baseTheme = statsOnlyTheme(computeStats(decoded), 'self');
  }

  const baseParams: AutoGradeParams = {
    strength: req.strength ?? baseTheme.knobs.strength.default,
    skinProtection: req.skinProtection ?? baseTheme.knobs.skinProtection.default,
    overrides: { ...baseTheme.overrides },
  };
  const rounds = req.rounds ?? 5;
  const model = req.model ?? DEFAULT_MODEL;
  const key = req.mock ? '' : requireKey();
  let cost = 0;

  const render = (params: AutoGradeParams): RenderedRound<{ pngBase64: string }> => {
    const theme = themeWithOverrides(baseTheme, params);
    const srcStats = computeStats(decoded);
    const transform = buildTransform(srcStats, theme, {
      strength: params.strength,
      skinProtection: params.skinProtection,
    });
    const impact = computeGradeImpact(decoded, transform);
    const out = applyTransformBuf(decoded, transform);
    return { params, impact, frame: { pngBase64: pngBase64(out, frame.width, frame.height) } };
  };

  const critique = async (round: RenderedRound<{ pngBase64: string }>): Promise<CriticProposal> => {
    if (req.mock) return mockCritic(round);
    const impactSummary =
      `cast ${round.impact.castMagnitude.toFixed(1)} LAB units; ` +
      `bandChroma s/m/h ${round.impact.outStats.bandChroma.shadows.toFixed(0)}/${round.impact.outStats.bandChroma.mids.toFixed(0)}/${round.impact.outStats.bandChroma.highlights.toFixed(0)}; ` +
      `skin ${(round.impact.skinWeightTotal * 100).toFixed(1)}%`;
    const res = await critiqueFrame({
      key,
      model,
      pngBase64: round.frame.pngBase64,
      referencePngBase64,
      context: { mode, currentParams: round.params, round: 0, impactSummary },
    });
    cost += costForUsage(model, res.usage);
    return res.proposal;
  };

  const result = await runAutoGradeLoop<{ pngBase64: string }>({ render, critique }, { baseParams, maxRounds: rounds });
  const { apply, unmapped } = editorApplyFromResult(baseTheme, result.best.params, baseParams);

  return {
    status: 'ok',
    message:
      result.best.round === 0
        ? 'baseline kept - no tuned round improved on it'
        : `applied round ${result.best.round}`,
    defects: result.best.proposal.defects,
    verdict: result.best.proposal.verdict,
    bestRound: result.best.round,
    accepted: result.best.round > 0,
    stopReason: result.stopReason,
    apply,
    unmapped,
    cost,
    diverged: [],
  };
}

function runReference(req: AgentBridgeRequest): AgentBridgeResponse {
  if (!req.referencePath) throw new Error('reference: no image provided');
  if (!req.outPath) throw new Error('reference: no output sidecar path provided');
  const ref = loadImageFile(req.referencePath);
  const decoded = decodeFrame(ref, req.profile ?? 'rec709');
  const stats = computeStats(decoded);
  writeFileSync(req.outPath, writeReferenceStatsFile(stats));
  return {
    status: 'ok',
    message: `measured ${basename(req.referencePath)} (${ref.width}x${ref.height}) -> ${basename(req.outPath)}`,
    defects: [],
    apply: [],
    unmapped: [],
    diverged: [],
  };
}

function runBatch(req: AgentBridgeRequest): AgentBridgeResponse {
  const theme = req.theme && THEMES[req.theme] ? THEMES[req.theme]! : undefined;
  if (!theme) throw new Error(`batch: unknown theme "${req.theme ?? ''}" (need a registry key)`);
  const clips = req.clipPaths ?? [];
  if (clips.length < 2) throw new Error('batch: need at least 2 clips to compare');
  const summaries: ClipGradeSummary[] = clips.map((path) => {
    const frame = loadImageFile(path);
    const decoded = decodeFrame(frame, req.profile ?? 'rec709');
    const stats = computeStats(decoded);
    const transform = buildTransform(stats, theme, { strength: req.strength });
    return { clipId: basename(path), impact: computeGradeImpact(decoded, transform) };
  });
  const report = checkSceneConsistency(summaries, undefined, 'editor-batch');
  const pairs = (summaries.length * (summaries.length - 1)) / 2;
  return {
    status: 'ok',
    message:
      report.diverged.length === 0
        ? `${clips.length} clips consistent (${pairs} pair(s))`
        : `${report.diverged.length} of ${pairs} pair(s) diverged`,
    defects: [],
    apply: [],
    unmapped: [],
    diverged: report.diverged.map((d) => ({
      clipA: d.clipA,
      clipB: d.clipB,
      reason: d.reasons.join('; '),
    })),
    pairsCompared: pairs,
  };
}

async function dispatch(req: AgentBridgeRequest): Promise<AgentBridgeResponse> {
  switch (req.command) {
    case 'critique':
      return runCritique(req);
    case 'autograde':
      return runAutograde(req);
    case 'reference':
      return runReference(req);
    case 'batch':
      return runBatch(req);
    default:
      throw new Error(`unknown command "${req.command}"`);
  }
}

async function main(): Promise<void> {
  const [reqPath, respPath] = process.argv.slice(2);
  if (!reqPath || !respPath) {
    console.error('usage: tsx scripts/agentBridge.ts <request.txt> <response.txt>');
    process.exit(2);
  }
  let resp: AgentBridgeResponse;
  try {
    const req = parseAgentRequest(readFileSync(reqPath, 'utf8'));
    resp = await dispatch(req);
  } catch (err) {
    resp = errorResponse(err instanceof Error ? err.message : String(err));
  }
  writeFileSync(respPath, formatAgentResponse(resp));
  // Exit 0 whenever a response file was written; the editor reads status from it.
  process.exit(0);
}

main().catch((err) => {
  console.error(err instanceof Error ? err.message : String(err));
  process.exit(1);
});
