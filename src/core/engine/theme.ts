import type { FootageStats } from '../analysis/stats.js';

/**
 * A curve control point [x, y]. Curves are data only; the engine interpolates
 * them with shape-preserving PCHIP (no overshoot between control points).
 */
export type CurvePoint = [number, number];

/**
 * Authored chroma shaping applied after stat matching and band scaling.
 * All optional; all identity when omitted.
 */
export interface ChromaShape {
  /**
   * Chroma multiplier as a function of output luma: control points
   * [luma 0-1, multiplier]. Need not be monotone (e.g. peak in the mids).
   */
  byLuma?: CurvePoint[];
  /**
   * Nonlinear saturation: boosts (or, negative, mutes) low-chroma pixels more
   * than already-saturated ones. Sensible range about -1..1; 0 = none.
   */
  vibrance?: number;
  /**
   * Soft ceiling on LAB chroma (tanh compression). Chroma asymptotically
   * approaches this value instead of growing unbounded. Omit for no limit.
   */
  softLimit?: number;
}

/**
 * Hand-authored artistic nudges applied after stat matching - taste the target
 * stats cannot express. All optional.
 */
export interface ThemeOverrides {
  /**
   * LAB [a, b] tint added in the shadows band (weighted by band membership).
   * Typically 1-20 LAB units; values much past ~20 read as an obvious color cast.
   */
  shadowTint?: [number, number];
  /**
   * LAB [a, b] tint added in the highlights band (weighted by band membership).
   * Typically 1-20 LAB units; values much past ~20 read as an obvious color cast.
   */
  highlightTint?: [number, number];
  /**
   * LAB [a, b] tint added in the midtones band (weighted by band membership,
   * same feathered shadow/mid/highlight split as shadowTint/highlightTint).
   * Use this to cancel a cast that sits mostly in the midtones - shadowTint and
   * highlightTint alone cannot reach it, since their band weights fall off
   * toward zero exactly where the midtone band peaks.
   * Typically 1-20 LAB units; values much past ~20 read as an obvious color cast.
   */
  midtoneTint?: [number, number];
  /** Global multiplier on LAB chroma after stat matching (1 = none). */
  chromaGain?: number;
  /**
   * Authored master tone curve on gamma-encoded [0,1], applied per channel
   * after the stat-matching tone curve. Control points [x, y]; interpolated
   * monotonically, so it cannot introduce tonal reversals.
   */
  toneCurve?: CurvePoint[];
  /** Optional authored per-channel curves, applied after the master curve. */
  channelCurves?: { r?: CurvePoint[]; g?: CurvePoint[]; b?: CurvePoint[] };
  /** Authored chroma shaping (see ChromaShape). */
  chromaShape?: ChromaShape;
}

export interface ThemeKnobs {
  /** 0-1, interpolates the whole transform toward identity. */
  strength: { default: number };
  /** 0-1, skin-tone protection amount. */
  skinProtection: { default: number };
}

/** A Theme is data only: target stats + optional overrides + exposed knobs. */
export interface Theme {
  name: string;
  description: string;
  targetStats: FootageStats;
  overrides?: ThemeOverrides;
  knobs: ThemeKnobs;
  /**
   * Whether the engine runs its automatic stat-matching look (tone curve toward
   * the target percentiles, LAB mean/std transfer, per-band chroma scaling, and
   * the chroma-overshoot guard). Default (undefined) is `true` for every shipping
   * theme. The "None / Manual" theme sets it `false` so those stages contribute
   * exact identity and manual grading (Phase 6a) is the entire look - no
   * stat-match staleness interaction. Authored overrides, if any, still apply.
   */
  matchStats?: boolean;
}
