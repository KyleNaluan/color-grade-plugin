import type { Vec3 } from '../color/types.js';
import { linearRec709ToLab, labToLinearRec709 } from '../color/lab.js';
import { rec709Decode, rec709Encode } from '../color/rec709.js';
import {
  luma709,
  skinWedgeWeight,
  SHADOW_MID_SPLIT,
  MID_HIGHLIGHT_SPLIT,
  type FootageStats,
} from '../analysis/stats.js';
import { makeMonotoneCurve, type MonotoneCurve } from './monotoneCurve.js';
import type { Theme } from './theme.js';

/**
 * A grade transform: maps one gamma-encoded Rec.709 RGB pixel to another.
 * This is what gets baked into the grade .cube.
 */
export type Transform = (rgb: Vec3) => Vec3;

export interface EngineOptions {
  /** 0-1; overrides the theme knob default. */
  strength?: number;
  /** 0-1; overrides the theme knob default. */
  skinProtection?: number;
}

const SKIN_PRESENCE_THRESHOLD = 0.02;

function clamp01(x: number): number {
  return x < 0 ? 0 : x > 1 ? 1 : x;
}

function clamp(x: number, lo: number, hi: number): number {
  return x < lo ? lo : x > hi ? hi : x;
}

function lerp(a: number, b: number, t: number): number {
  return a + (b - a) * t;
}

/** Smooth 0->1 ramp between edges. */
function smoothstep(e0: number, e1: number, x: number): number {
  const t = clamp01((x - e0) / (e1 - e0));
  return t * t * (3 - 2 * t);
}

/** Soft band membership weights [shadows, mids, highlights] with feathered edges. */
export function bandWeights(y: number): Vec3 {
  const feather = 0.08;
  const s = 1 - smoothstep(SHADOW_MID_SPLIT - feather, SHADOW_MID_SPLIT + feather, y);
  const h = smoothstep(MID_HIGHLIGHT_SPLIT - feather, MID_HIGHLIGHT_SPLIT + feather, y);
  return [s, Math.max(0, 1 - s - h), h];
}

function buildToneCurve(src: FootageStats, tgt: FootageStats): MonotoneCurve {
  const sp = src.lumaPercentiles;
  const tp = tgt.lumaPercentiles;
  return makeMonotoneCurve(
    [0, sp.p5, sp.p25, sp.p50, sp.p75, sp.p95, 1],
    [0, tp.p5, tp.p25, tp.p50, tp.p75, tp.p95, 1],
  );
}

/**
 * Build the grade transform: measured footage stats -> transform toward the
 * theme's target stats, scaled by knobs, with skin-tone protection.
 *
 * Domain and range are gamma-encoded Rec.709 (i.e. the transform assumes the
 * Correct stack - including any Decode LUT - has already run).
 */
export function buildTransform(src: FootageStats, theme: Theme, opts: EngineOptions = {}): Transform {
  const tgt = theme.targetStats;
  const strength = clamp01(opts.strength ?? theme.knobs.strength.default);
  const protection = clamp01(opts.skinProtection ?? theme.knobs.skinProtection.default);
  const skinActive = src.skinPresence > SKIN_PRESENCE_THRESHOLD;

  const toneCurve = buildToneCurve(src, tgt);

  // Reinhard-style stat transfer in a/b, with std ratios clamped so degenerate
  // (near-neutral) footage cannot explode chroma.
  const kA = clamp(tgt.lab.std[1] / Math.max(tgt.lab.std[1] * 0.1, src.lab.std[1], 1e-3), 0.4, 2.2);
  const kB = clamp(tgt.lab.std[2] / Math.max(tgt.lab.std[2] * 0.1, src.lab.std[2], 1e-3), 0.4, 2.2);

  // Per-tonal-band chroma scale toward target band chroma.
  const bandScale: Vec3 = [
    clamp(tgt.bandChroma.shadows / Math.max(src.bandChroma.shadows, 1), 0.5, 2.0),
    clamp(tgt.bandChroma.mids / Math.max(src.bandChroma.mids, 1), 0.5, 2.0),
    clamp(tgt.bandChroma.highlights / Math.max(src.bandChroma.highlights, 1), 0.5, 2.0),
  ];

  const ov = theme.overrides ?? {};
  const chromaGain = ov.chromaGain ?? 1;

  return (rgb: Vec3): Vec3 => {
    const rIn = clamp01(rgb[0]);
    const gIn = clamp01(rgb[1]);
    const bIn = clamp01(rgb[2]);

    // 1. Tone: master curve per channel (matches luma percentiles, keeps color ratios).
    const r1 = toneCurve(rIn);
    const g1 = toneCurve(gIn);
    const b1 = toneCurve(bIn);

    // 2. Color in LAB.
    const labIn = linearRec709ToLab([rec709Decode(rIn), rec709Decode(gIn), rec709Decode(bIn)]);
    const lab = linearRec709ToLab([rec709Decode(r1), rec709Decode(g1), rec709Decode(b1)]);
    let a = (lab[1] - src.lab.mean[1]) * kA + tgt.lab.mean[1];
    let bb = (lab[2] - src.lab.mean[2]) * kB + tgt.lab.mean[2];

    // 3. Per-band chroma scaling + overrides.
    const y1 = luma709(r1, g1, b1);
    const [ws, wm, wh] = bandWeights(y1);
    const scale = (ws * bandScale[0] + wm * bandScale[1] + wh * bandScale[2]) * chromaGain;
    a *= scale;
    bb *= scale;
    if (ov.shadowTint) {
      a += ov.shadowTint[0] * ws;
      bb += ov.shadowTint[1] * ws;
    }
    if (ov.highlightTint) {
      a += ov.highlightTint[0] * wh;
      bb += ov.highlightTint[1] * wh;
    }

    const linOut = labToLinearRec709([lab[0], a, bb]);
    let out: Vec3 = [
      clamp01(rec709Encode(Math.max(0, linOut[0]))),
      clamp01(rec709Encode(Math.max(0, linOut[1]))),
      clamp01(rec709Encode(Math.max(0, linOut[2]))),
    ];

    // 4. Strength: interpolate toward identity.
    // 5. Skin-tone protection: within the soft chroma wedge around the skin
    //    line, attenuate further toward identity (only if skin is present).
    let identityMix = 1 - strength;
    if (skinActive && protection > 0) {
      const w = skinWedgeWeight(labIn[0], labIn[1], labIn[2]);
      identityMix = Math.max(identityMix, 1 - strength * (1 - protection * w));
    }
    if (identityMix > 0) {
      out = [lerp(out[0], rIn, identityMix), lerp(out[1], gIn, identityMix), lerp(out[2], bIn, identityMix)];
    }
    return out;
  };
}
