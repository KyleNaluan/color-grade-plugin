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

/** Result of the automatic chroma-overshoot guard (see {@link toneStretchChromaGuard}). */
export interface ChromaGuard {
  /** output/input luma-range ratio (p5..p95) that drives the guard; ~1 for ordinary sources. */
  stretch: number;
  /** 0 below the knee (guard inert) rising to 1; how hard the guard engages. */
  severity: number;
  /** Multiplier (<=1) folded into the automatic per-band chroma amplification. 1 = inert. */
  gain: number;
  /**
   * LAB-chroma soft ceiling to install when the theme authored none, or `undefined`
   * when the guard is inert (so ordinary sources keep their exact prior behavior).
   */
  autoSoftLimit: number | undefined;
}

// Guard tuning. The knee is set above any ordinary source's stretch (a normal
// clip's luma range is within ~1.5x of the target's) so the guard is exactly
// inert - and therefore output-identical - for ordinary footage; full strength
// by ~3x, the regime the scout report's Experiment A blowout lives in.
const STRETCH_KNEE = 1.5;
const STRETCH_FULL = 3.0;
const GUARD_GAIN_FLOOR = 0.2; // auto amplification is damped no further than this
const AUTO_CEIL_LOOSE = 6; // multiple of target band chroma near the knee
const AUTO_CEIL_TIGHT = 3; // multiple of target band chroma at full severity

/**
 * Automatic chroma-overshoot guard.
 *
 * A large tone-curve stretch - target luma range far wider than the source's,
 * e.g. stat-matching a low-dynamic-range or degraded clip toward a full-range
 * target - pushes originally mid-luma pixels out into the shadow/highlight
 * output bands. There the automatic per-band chroma scale (`bandScale`, up to
 * 2x) and the per-pixel LAB std-ratio gain (`kA`/`kB`, up to 1.8x) - both fit to
 * the *source's* per-band stats, which are near-empty in those bands - compound
 * and explode chroma far past the target (the "neon blowout", scout report
 * Experiment A). The stretch ratio (output luma range / input luma range) is a
 * principled proxy for how much relocation, and thus overshoot, will occur.
 *
 * In the large-stretch safety regime the guard's gain bounds the *total*
 * auto-driven per-band chroma amplification - which includes any authored
 * chromaGain, since gain folds into the same `scale` product - by design: this is
 * exactly the blowout-prevention regime. The auto soft ceiling, by contrast, is
 * only adopted when the theme authored no chromaShape.softLimit, so a hand-set
 * ceiling always wins. Outside the guard's engaged regime hand-authored intent is
 * untouched.
 */
export function toneStretchChromaGuard(src: FootageStats, tgt: FootageStats): ChromaGuard {
  const srcRange = Math.max(src.lumaPercentiles.p95 - src.lumaPercentiles.p5, 1e-3);
  const tgtRange = Math.max(tgt.lumaPercentiles.p95 - tgt.lumaPercentiles.p5, 1e-3);
  const stretch = tgtRange / srcRange;
  const severity = smoothstep(STRETCH_KNEE, STRETCH_FULL, stretch);
  const gain = lerp(1, GUARD_GAIN_FLOOR, severity);
  const tgtBandChromaMax = Math.max(tgt.bandChroma.shadows, tgt.bandChroma.mids, tgt.bandChroma.highlights);
  const autoSoftLimit =
    severity > 0 && tgtBandChromaMax > 0
      ? tgtBandChromaMax * lerp(AUTO_CEIL_LOOSE, AUTO_CEIL_TIGHT, severity)
      : undefined;
  return { stretch, severity, gain, autoSoftLimit };
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

  // Automatic chroma-overshoot guard: when the tone curve stretches a low-range
  // source toward a much wider target, damp the stat-derived auto chroma
  // amplification (guard.gain, folded into `scale` below) so relocated pixels
  // can't neon-blow-up, and adopt an auto soft ceiling if the theme authored
  // none. Inert (gain 1, no ceiling) for ordinary sources.
  const guard = toneStretchChromaGuard(src, tgt);

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
  // Authored ceiling wins; otherwise the guard's auto ceiling (undefined = none).
  const softLimit = shape.softLimit ?? guard.autoSoftLimit;

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
    // guard.gain bounds the total auto-driven amplification here, including the
    // authored chromaGain, when a large stretch would otherwise neon-blow-up.
    const scale = (ws * bandScale[0] + wm * bandScale[1] + wh * bandScale[2]) * chromaGain * guard.gain;
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
    if (ov.midtoneTint) {
      a += ov.midtoneTint[0] * wm;
      bb += ov.midtoneTint[1] * wm;
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
