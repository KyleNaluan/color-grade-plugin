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
  /**
   * Manual primary correction (Phase 6a), applied on the decoded gamma-Rec.709
   * signal *ahead* of the theme stages. Omit (or pass {@link NEUTRAL_MANUAL})
   * for the pure theme grade - a neutral manual stage contributes exact identity.
   */
  manual?: ManualGrade;
  /**
   * DaVinci-style Lift/Gamma/Gain wheels (Phase 6c), applied in the manual-primaries
   * slot right after {@link ManualGrade} on the decoded gamma-Rec.709 signal. Omit
   * (or pass {@link NEUTRAL_LGG}) for no wheels - a neutral LGG is exact identity.
   */
  lgg?: LiftGammaGain;
  /**
   * Look Mix (Phase 6a, keyframeable PF param): 0-1, default 1. Blends the
   * theme "look" contribution in and out while keeping the manual correction:
   * 1 = full theme look, 0 = manual-corrected only (theme look removed). Distinct
   * from `strength`, which dilutes the *whole* grade toward the original footage.
   */
  lookMix?: number;
}

/**
 * DaVinci-style Lift/Gamma/Gain color wheels (Phase 6c). Each is a per-channel RGB
 * triple (the wheel's disc color folded with its master ring). The printer-lights
 * model - `out_c = clamp01((gain_c*x + lift_c*(1-x))^(1/gamma_c))` - moves the black
 * point (lift), white point (gain), and bends the midtones (gamma) with the opposite
 * endpoint pinned. Neutral (`lift 0, gamma 1, gain 1`) is exact identity.
 */
export interface LiftGammaGain {
  /** Per-channel lift (black-point offset), 0 = neutral. */
  lift: Vec3;
  /** Per-channel gamma (midtone bend), 1 = neutral. Must be > 0. */
  gamma: Vec3;
  /** Per-channel gain (white-point multiply), 1 = neutral. */
  gain: Vec3;
}

/**
 * Positive floor for per-channel gamma before inverting to `1/gamma`. The
 * wheel UI can compose a non-positive gamma (master min + full color disc),
 * which would flip the exponent and blow a channel toward white; clamping here
 * keeps `1/gamma` finite and positive.
 */
export const LGG_GAMMA_FLOOR = 1e-3;

/** The neutral Lift/Gamma/Gain (exact identity). */
export const NEUTRAL_LGG: LiftGammaGain = {
  lift: [0, 0, 0],
  gamma: [1, 1, 1],
  gain: [1, 1, 1],
};

/**
 * Manual primary correction (Phase 6a). All controls are neutral at
 * {@link NEUTRAL_MANUAL} and contribute exact identity when neutral. Applied on
 * the decoded gamma-Rec.709 signal ahead of the theme stages; exposure
 * multiplies in linear light, temp/tint/saturation/vibrance act in CIELAB.
 */
export interface ManualGrade {
  /** Exposure in stops, -5..+5. Multiplies scene-linear light by 2^EV. 0 = none. */
  exposure: number;
  /** Contrast, -100..+100. S-slope about `pivot` in gamma space. 0 = none. */
  contrast: number;
  /** Contrast pivot in gamma-709 [0,1]; the tone contrast rotates about. */
  pivot: number;
  /** Highlights, -100..+100. Region lift of the upper tonal band. 0 = none. */
  highlights: number;
  /** Shadows, -100..+100. Region lift of the lower tonal band. 0 = none. */
  shadows: number;
  /** Whites, -100..+100. Region lift near the highlight shoulder. 0 = none. */
  whites: number;
  /** Blacks, -100..+100. Region lift near the shadow toe. 0 = none. */
  blacks: number;
  /** Temperature, -100..+100. White-balance along blue<->amber (LAB b bias). 0 = none. */
  temperature: number;
  /** Tint, -100..+100. Green<->magenta (LAB a bias). 0 = none. */
  tint: number;
  /** Saturation, 0..2 (1 = 100%, neutral). Linear LAB chroma multiply. */
  saturation: number;
  /** Vibrance, -100..+100. Nonlinear chroma (low-chroma pixels weighted more). 0 = none. */
  vibrance: number;
}

/** The neutral manual grade: every control at its identity value. */
export const NEUTRAL_MANUAL: ManualGrade = {
  exposure: 0,
  contrast: 0,
  pivot: 0.435,
  highlights: 0,
  shadows: 0,
  whites: 0,
  blacks: 0,
  temperature: 0,
  tint: 0,
  saturation: 1,
  vibrance: 0,
};

// Manual-stage tuning. Ranges are UI ±100 (or 0..2 for saturation); these map the
// UI extreme to an engine-space magnitude. Chosen as the shipping defaults.
const MANUAL_REGION_LIFT = 0.5; // gamma-709 lift at ±100 for hi/sh/wh/bl
const MANUAL_TEMP_MAX = 30; // LAB b units at ±100 (temperature)
const MANUAL_TINT_MAX = 30; // LAB a units at ±100 (tint)
// e-folding chroma for the vibrance falloff (shared by the manual stage and the
// theme's authored vibrance): low-chroma pixels get the full effect.
const VIBRANCE_FALLOFF = 25; // LAB chroma units

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
 * The manual-primaries slot as a whole: manual basics (Phase 6a) followed by the
 * Lift/Gamma/Gain wheels (Phase 6c), applied to one decoded gamma-Rec.709 pixel
 * ahead of the theme stages. `active` is false when both stages are neutral, in
 * which case `apply` is exact identity on its input.
 */
export interface ManualPrimaries {
  /** True when the manual or LGG stage does anything (neutral -> false). */
  readonly active: boolean;
  /** Apply manual basics then LGG wheels to one gamma-Rec.709 pixel. */
  apply(rgb: Vec3): Vec3;
}

/**
 * Precompute the manual-primaries stage (manual basics + LGG wheels) once and
 * return a reusable applier. This is the single source of truth for that stage:
 * {@link buildTransform} folds it in ahead of the theme look, and
 * {@link applyManualPrimaries} exposes it standalone for the "Correct/Basics under
 * a user LUT" composition (decode -> correct/basics -> user LUT), where the same
 * math must run outside a full grade. Each sub-op is gated on being non-neutral so
 * a neutral control contributes exact identity (no lossy decode/encode or LAB
 * round-trip on the neutral path).
 */
export function buildManualPrimaries(manual?: ManualGrade, lgg?: LiftGammaGain): ManualPrimaries {
  // --- Manual primary correction (Phase 6a) -------------------------------
  const m = manual;
  const mExpActive = !!m && m.exposure !== 0;
  const mExpGain = mExpActive ? Math.pow(2, m!.exposure) : 1;
  const mContrastActive = !!m && m.contrast !== 0;
  const mPivot = m?.pivot ?? NEUTRAL_MANUAL.pivot;
  const mContrastSlope = mContrastActive
    ? m!.contrast >= 0
      ? 1 + m!.contrast / 100
      : 1 / (1 - m!.contrast / 100)
    : 1;
  const mRegionActive =
    !!m && (m.highlights !== 0 || m.shadows !== 0 || m.whites !== 0 || m.blacks !== 0);
  const mAmtHi = ((m?.highlights ?? 0) / 100) * MANUAL_REGION_LIFT;
  const mAmtSh = ((m?.shadows ?? 0) / 100) * MANUAL_REGION_LIFT;
  const mAmtWh = ((m?.whites ?? 0) / 100) * MANUAL_REGION_LIFT;
  const mAmtBl = ((m?.blacks ?? 0) / 100) * MANUAL_REGION_LIFT;
  const mColorActive =
    !!m && (m.temperature !== 0 || m.tint !== 0 || m.saturation !== 1 || m.vibrance !== 0);
  const mSat = m?.saturation ?? 1;
  const mVib = (m?.vibrance ?? 0) / 100; // -1..1
  const mTempB = ((m?.temperature ?? 0) / 100) * MANUAL_TEMP_MAX;
  const mTintA = ((m?.tint ?? 0) / 100) * MANUAL_TINT_MAX;
  const manualActive = mExpActive || mContrastActive || mRegionActive || mColorActive;

  // --- Lift/Gamma/Gain wheels (Phase 6c) ----------------------------------
  const lggActive =
    !!lgg &&
    (lgg.lift[0] !== 0 || lgg.lift[1] !== 0 || lgg.lift[2] !== 0 ||
      lgg.gamma[0] !== 1 || lgg.gamma[1] !== 1 || lgg.gamma[2] !== 1 ||
      lgg.gain[0] !== 1 || lgg.gain[1] !== 1 || lgg.gain[2] !== 1);
  const lgLift: Vec3 = lgg ? lgg.lift : [0, 0, 0];
  const lgGain: Vec3 = lgg ? lgg.gain : [1, 1, 1];
  const lgInvGamma: Vec3 = lgg
    ? [
        1 / Math.max(LGG_GAMMA_FLOOR, lgg.gamma[0]),
        1 / Math.max(LGG_GAMMA_FLOOR, lgg.gamma[1]),
        1 / Math.max(LGG_GAMMA_FLOOR, lgg.gamma[2]),
      ]
    : [1, 1, 1];
  const lggChannel = (x: number, lift: number, gain: number, invGamma: number): number => {
    const base = gain * x + lift * (1 - x);
    return clamp01(Math.pow(Math.max(0, base), invGamma));
  };
  const applyLgg = (r: number, g: number, b: number): Vec3 => [
    lggChannel(r, lgLift[0], lgGain[0], lgInvGamma[0]),
    lggChannel(g, lgLift[1], lgGain[1], lgInvGamma[1]),
    lggChannel(b, lgLift[2], lgGain[2], lgInvGamma[2]),
  ];

  // Apply the manual stage on one decoded gamma-Rec.709 pixel. Order: exposure
  // (linear) -> contrast (about pivot) -> region lifts (bandWeights) -> color
  // (LAB temp/tint/saturation/vibrance).
  const applyManual = (r0: number, g0: number, b0: number): Vec3 => {
    let r = r0;
    let g = g0;
    let b = b0;
    if (mExpActive) {
      r = rec709Encode(rec709Decode(r) * mExpGain);
      g = rec709Encode(rec709Decode(g) * mExpGain);
      b = rec709Encode(rec709Decode(b) * mExpGain);
    }
    if (mContrastActive) {
      r = mPivot + (r - mPivot) * mContrastSlope;
      g = mPivot + (g - mPivot) * mContrastSlope;
      b = mPivot + (b - mPivot) * mContrastSlope;
    }
    if (mExpActive || mContrastActive) {
      r = clamp01(r);
      g = clamp01(g);
      b = clamp01(b);
    }
    if (mRegionActive) {
      const y = luma709(r, g, b);
      // Shadows/highlights reuse the feathered band split; blacks/whites add an
      // extra toe/shoulder edge weight so all four controls stay independent.
      const [wsB, , whB] = bandWeights(y);
      const wBlack = 1 - smoothstep(0, 0.2, y);
      const wWhite = smoothstep(0.8, 1, y);
      const dY = mAmtSh * wsB + mAmtHi * whB + mAmtBl * wBlack + mAmtWh * wWhite;
      r = clamp01(r + dY);
      g = clamp01(g + dY);
      b = clamp01(b + dY);
    }
    if (mColorActive) {
      const labm = linearRec709ToLab([rec709Decode(r), rec709Decode(g), rec709Decode(b)]);
      let am = labm[1] * mSat;
      let bm = labm[2] * mSat;
      if (mVib !== 0) {
        const chromam = Math.hypot(am, bm);
        if (chromam > 1e-6) {
          const mult = Math.max(0, 1 + mVib * Math.exp(-chromam / VIBRANCE_FALLOFF));
          am *= mult;
          bm *= mult;
        }
      }
      am += mTintA;
      bm += mTempB;
      const linm = labToLinearRec709([labm[0], am, bm]);
      r = clamp01(rec709Encode(Math.max(0, linm[0])));
      g = clamp01(rec709Encode(Math.max(0, linm[1])));
      b = clamp01(rec709Encode(Math.max(0, linm[2])));
    }
    return [r, g, b];
  };

  const active = manualActive || lggActive;
  const apply = (rgb: Vec3): Vec3 => {
    let r = rgb[0];
    let g = rgb[1];
    let b = rgb[2];
    if (manualActive) [r, g, b] = applyManual(r, g, b);
    if (lggActive) [r, g, b] = applyLgg(r, g, b);
    return [r, g, b];
  };
  return { active, apply };
}

/**
 * Apply the manual-primaries stage (basics + LGG wheels) to one gamma-Rec.709
 * pixel, standalone. Convenience over {@link buildManualPrimaries} for a single
 * pixel; when baking a LUT, build once and reuse {@link ManualPrimaries.apply}.
 */
export function applyManualPrimaries(rgb: Vec3, manual?: ManualGrade, lgg?: LiftGammaGain): Vec3 {
  return buildManualPrimaries(manual, lgg).apply(rgb);
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

  // Automatic stat-matching look: on for every shipping theme, off for the
  // "None / Manual" theme (matchStats === false), where the tone curve, the LAB
  // mean/std transfer, the per-band chroma scale, and the chroma-overshoot guard
  // all collapse to identity so manual grading is the entire look.
  const matchStats = theme.matchStats !== false;

  // Damped stat transfer in a/b: the target LAB mean is an attractor, not a
  // destination. The mean shift magnitude is soft-clamped (tanh) so a theme
  // whose color bias opposes the footage's cannot drag the whole image across
  // neutral as one full-distance global cast - the spike's main failure mode.
  const MAX_MEAN_SHIFT = 10; // LAB units
  const dA = tgt.lab.mean[1] - src.lab.mean[1];
  const dB = tgt.lab.mean[2] - src.lab.mean[2];
  const dist = Math.hypot(dA, dB);
  const damp = dist > 1e-6 ? (MAX_MEAN_SHIFT * Math.tanh(dist / MAX_MEAN_SHIFT)) / dist : 1;

  const identityCurve: MonotoneCurve = (x) => x;
  const toneCurve = matchStats ? buildToneCurve(src, tgt) : identityCurve;
  const shiftA = matchStats ? dA * damp : 0;
  const shiftB = matchStats ? dB * damp : 0;
  // Std ratios clamped tighter than before so chroma spread changes stay gentle.
  const kA = matchStats
    ? clamp(tgt.lab.std[1] / Math.max(tgt.lab.std[1] * 0.1, src.lab.std[1], 1e-3), 0.6, 1.8)
    : 1;
  const kB = matchStats
    ? clamp(tgt.lab.std[2] / Math.max(tgt.lab.std[2] * 0.1, src.lab.std[2], 1e-3), 0.6, 1.8)
    : 1;

  // Per-tonal-band chroma scale toward target band chroma.
  const bandScale: Vec3 = matchStats
    ? [
        clamp(tgt.bandChroma.shadows / Math.max(src.bandChroma.shadows, 1), 0.5, 2.0),
        clamp(tgt.bandChroma.mids / Math.max(src.bandChroma.mids, 1), 0.5, 2.0),
        clamp(tgt.bandChroma.highlights / Math.max(src.bandChroma.highlights, 1), 0.5, 2.0),
      ]
    : [1, 1, 1];

  const ov = theme.overrides ?? {};
  const chromaGain = ov.chromaGain ?? 1;

  // Automatic chroma-overshoot guard: when the tone curve stretches a low-range
  // source toward a much wider target, damp the stat-derived auto chroma
  // amplification (guard.gain, folded into `scale` below) so relocated pixels
  // can't neon-blow-up, and adopt an auto soft ceiling if the theme authored
  // none. Inert (gain 1, no ceiling) for ordinary sources, and by construction
  // when the stat-match is off (a 1:1 stretch keeps the guard inert).
  const guard = matchStats
    ? toneStretchChromaGuard(src, tgt)
    : ({ stretch: 1, severity: 0, gain: 1, autoSoftLimit: undefined } as ChromaGuard);

  // Authored curves: monotone per-channel tone shapes, evaluated on the
  // stat-matched signal, then clamped so authored points outside [0,1] cannot
  // push the encoded signal out of range.
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
  // Authored ceiling wins; otherwise the guard's auto ceiling (undefined = none).
  const softLimit = shape.softLimit ?? guard.autoSoftLimit;

  // --- Manual-primaries slot (Phase 6a basics + Phase 6c LGG wheels) ------
  // Precomputed once via the shared builder (also used standalone by the
  // Correct/Basics-under-LUT path); neutral controls contribute exact identity.
  const primaries = buildManualPrimaries(opts.manual, opts.lgg);
  const correctionActive = primaries.active;

  // Look Mix: blend the theme look in/out over the manual-corrected pixel.
  const lookMix = clamp01(opts.lookMix ?? 1);
  const lookMixActive = lookMix !== 1;

  // Fast identity path: when the theme contributes no look (stat-match off and no
  // authored overrides) and the manual stage is neutral, the whole transform is a
  // clamp - so the "None / Manual" theme with no edits bakes a clean identity LUT.
  const themeLookIsIdentity =
    !matchStats &&
    !ov.shadowTint &&
    !ov.highlightTint &&
    !ov.midtoneTint &&
    chromaGain === 1 &&
    !ov.toneCurve &&
    !ov.channelCurves &&
    !ov.chromaShape;
  if (themeLookIsIdentity && !correctionActive) {
    return (rgb: Vec3): Vec3 => [clamp01(rgb[0]), clamp01(rgb[1]), clamp01(rgb[2])];
  }

  return (rgb: Vec3): Vec3 => {
    const rIn = clamp01(rgb[0]);
    const gIn = clamp01(rgb[1]);
    const bIn = clamp01(rgb[2]);

    // 0. Manual primary correction + LGG wheels on the decoded footage, ahead of
    //    the theme stages. The identity target for Strength/Skin below stays the
    //    ORIGINAL footage (rIn/gIn/bIn), so Strength dilutes the whole grade -
    //    manual + wheels included (decision D3) - and skin is keyed on real footage.
    let rm = rIn;
    let gm = gIn;
    let bm = bIn;
    if (correctionActive) [rm, gm, bm] = primaries.apply([rm, gm, bm]);

    // 1. Tone: stat-matching curve per channel (matches luma percentiles,
    //    keeps color ratios), then the authored master + per-channel curves.
    const r1 = rCurve(masterCurve(toneCurve(rm)));
    const g1 = gCurve(masterCurve(toneCurve(gm)));
    const b1 = bCurve(masterCurve(toneCurve(bm)));

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

    // 3c. Look Mix: fade the theme look toward the manual-corrected pixel
    //     (lookMix 1 = full look, 0 = manual only). Runs before Strength so the
    //     two knobs compose (look mix, then overall grade intensity).
    if (lookMixActive) {
      out = [lerp(rm, out[0], lookMix), lerp(gm, out[1], lookMix), lerp(bm, out[2], lookMix)];
    }

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
