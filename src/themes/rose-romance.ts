import type { Theme } from '../core/engine/theme.js';

/**
 * Rose romance: soft, warm, rosy - a glowy romantic look with lifted blacks, a
 * pink-warm lean and gentle contrast. Distinct from pastel-dream (cool-mint
 * pastel) by its warm rose direction, and from warm-portrait by being softer,
 * lower-contrast and more overtly tinted.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. Rose = positive-a
 * (magenta/pink) with mild positive-b warmth across the band tints; lifted toe +
 * low std for the soft glow.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 11.4 on vlog-outdoor-cool. Skin:
 * vlog-tricky-skin -6.0deg/-1.0%, vlog-cave-warm -2.4deg/-19.0%, vlog-portait-cool
 * -1.5deg/2.6%, rec709-outdoor -10.6deg/3.3%. Passes the gradeGuard gate. Suits
 * weddings, romance and beauty footage.
 */
export const roseRomance: Theme = {
  name: 'rose-romance',
  description: 'Soft warm rosy glow, lifted blacks, gentle contrast.',
  targetStats: {
    lumaPercentiles: { p1: 0.06, p5: 0.1, p25: 0.3, p50: 0.52, p75: 0.73, p95: 0.9, p99: 0.96 },
    lab: { mean: [56, 8, 8], std: [17, 9, 10] },
    bandChroma: { shadows: 9, mids: 14, highlights: 12 },
    saturation: { mean: 0.28, std: 0.14 },
    skinPresence: 0,
    clipping: { low: 0.001, high: 0.002 },
  },
  overrides: {
    shadowTint: [4, 2],
    midtoneTint: [5, 3],
    highlightTint: [6, 6],
    chromaGain: 0.9,
    // Lifted, soft glow.
    toneCurve: [
      [0, 0.05],
      [0.25, 0.3],
      [0.5, 0.52],
      [0.75, 0.74],
      [1, 0.97],
    ],
    chromaShape: {
      byLuma: [
        [0, 0.95],
        [0.5, 1.02],
        [1, 0.95],
      ],
      softLimit: 50,
    },
  },
  knobs: {
    strength: { default: 0.72 },
    skinProtection: { default: 0.8 },
  },
};
