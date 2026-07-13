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
import { makeMonotoneCurve, makeShapeCurve, type MonotoneCurve } from './monotoneCurve.js';
import type { CurvePoint, Theme } from './theme.js';

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

  // Damped stat transfer in a/b: the target LAB mean is an attractor, not a
  // destination. The mean shift magnitude is soft-clamped (tanh) so a theme
  // whose color bias opposes the footage's cannot drag the whole image across
  // neutral as one full-distance global cast - the spike's main failure mode.
  const MAX_MEAN_SHIFT = 10; // LAB units
  const dA = tgt.lab.mean[1] - src.lab.mean[1];
  const dB = tgt.lab.mean[2] - src.lab.mean[2];
  const dist = Math.hypot(dA, dB);
  const damp = dist > 1e-6 ? (MAX_MEAN_SHIFT * Math.tanh(dist / MAX_MEAN_SHIFT)) / dist : 1;
  const shiftA = dA * damp;
  const shiftB = dB * damp;
  // Std ratios clamped tighter than before so chroma spread changes stay gentle.
  const kA = clamp(tgt.lab.std[1] / Math.max(tgt.lab.std[1] * 0.1, src.lab.std[1], 1e-3), 0.6, 1.8);
  const kB = clamp(tgt.lab.std[2] / Math.max(tgt.lab.std[2] * 0.1, src.lab.std[2], 1e-3), 0.6, 1.8);

  // Per-tonal-band chroma scale toward target band chroma.
  const bandScale: Vec3 = [
    clamp(tgt.bandChroma.shadows / Math.max(src.bandChroma.shadows, 1), 0.5, 2.0),
    clamp(tgt.bandChroma.mids / Math.max(src.bandChroma.mids, 1), 0.5, 2.0),
    clamp(tgt.bandChroma.highlights / Math.max(src.bandChroma.highlights, 1), 0.5, 2.0),
  ];

  const ov = theme.overrides ?? {};
  const chromaGain = ov.chromaGain ?? 1;

  // Authored curves: monotone per-channel tone shapes, evaluated on the
  // stat-matched signal, then clamped so authored points outside [0,1] cannot
  // push the encoded signal out of range.
  const identityCurve: MonotoneCurve = (x) => x;
  const makeToneOverride = (pts: CurvePoint[] | undefined): MonotoneCurve => {
    if (!pts || pts.length < 2) return identityCurve;
    const c = makeMonotoneCurve(pts.map((p) => p[0]), pts.map((p) => p[1]));
    return (x) => clamp01(c(x));
  };
  const masterCurve = makeToneOverride(ov.toneCurve);
  const rCurve = makeToneOverride(ov.channelCurves?.r);
  const gCurve = makeToneOverride(ov.channelCurves?.g);
  const bCurve = makeToneOverride(ov.channelCurves?.b);

  // Authored chroma shaping.
  const shape = ov.chromaShape ?? {};
  const chromaByLuma: MonotoneCurve =
    shape.byLuma && shape.byLuma.length >= 2
      ? (() => {
          const c = makeShapeCurve(shape.byLuma.map((p) => p[0]), shape.byLuma.map((p) => p[1]));
          return (y) => Math.max(0, c(y));
        })()
      : () => 1;
  const vibrance = shape.vibrance ?? 0;
  // e-folding chroma for the vibrance falloff: low-chroma pixels get the full
  // effect, pixels beyond ~2-3x this are left mostly alone.
  const VIBRANCE_FALLOFF = 25; // LAB chroma units
  const softLimit = shape.softLimit;

  return (rgb: Vec3): Vec3 => {
    const rIn = clamp01(rgb[0]);
    const gIn = clamp01(rgb[1]);
    const bIn = clamp01(rgb[2]);

    // 1. Tone: stat-matching curve per channel (matches luma percentiles,
    //    keeps color ratios), then the authored master + per-channel curves.
    const r1 = rCurve(masterCurve(toneCurve(rIn)));
    const g1 = gCurve(masterCurve(toneCurve(gIn)));
    const b1 = bCurve(masterCurve(toneCurve(bIn)));

    // 2. Color in LAB.
    const labIn = linearRec709ToLab([rec709Decode(rIn), rec709Decode(gIn), rec709Decode(bIn)]);
    const lab = linearRec709ToLab([rec709Decode(r1), rec709Decode(g1), rec709Decode(b1)]);
    let a = (lab[1] - src.lab.mean[1]) * kA + src.lab.mean[1] + shiftA;
    let bb = (lab[2] - src.lab.mean[2]) * kB + src.lab.mean[2] + shiftB;

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

    // 3b. Authored chroma shaping on the final chroma vector.
    let chroma = Math.hypot(a, bb);
    if (chroma > 1e-6) {
      let target = chroma * chromaByLuma(y1);
      if (vibrance !== 0) {
        target *= Math.max(0, 1 + vibrance * Math.exp(-chroma / VIBRANCE_FALLOFF));
      }
      if (softLimit !== undefined && softLimit > 0) {
        target = softLimit * Math.tanh(target / softLimit);
      }
      const k = target / chroma;
      a *= k;
      bb *= k;
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
