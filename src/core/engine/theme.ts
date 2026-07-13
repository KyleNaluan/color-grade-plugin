import type { FootageStats } from '../analysis/stats.js';

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
