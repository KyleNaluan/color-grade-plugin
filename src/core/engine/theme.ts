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
  /** LAB [a, b] tint added in the shadows band (weighted by band membership). */
  shadowTint?: [number, number];
  /** LAB [a, b] tint added in the highlights band. */
  highlightTint?: [number, number];
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
}
