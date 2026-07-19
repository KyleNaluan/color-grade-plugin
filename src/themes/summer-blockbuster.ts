import type { Theme } from '../core/engine/theme.js';

/**
 * Summer blockbuster: a brighter, punchier teal-orange variant - more exposure
 * and saturation than the flagship teal-orange, for that big-budget tentpole
 * daytime look. Sibling to teal-orange; kept distinct by a lifted, bolder base.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. Brighter luma
 * target + stronger teal shadow / warm highlight split + a touch more chroma than
 * teal-orange; skin protection matches teal-orange to hold faces.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 13.3 on
 * rec709-outdoor-with-skin-and-overexposed-sky. Skin tracks the shipping
 * teal-orange closely: vlog-tricky-skin -6.9deg/33.1%, vlog-cave-warm 1.3%,
 * vlog-portait-cool -1.8deg/23.0%, rec709-outdoor -12.5deg/31.8%. Passes the
 * gradeGuard gate. Suits action/adventure daytime exteriors.
 */
export const summerBlockbuster: Theme = {
  name: 'summer-blockbuster',
  description: 'Bright, punchy teal-orange - bold tentpole daytime look.',
  targetStats: {
    lumaPercentiles: { p1: 0.02, p5: 0.05, p25: 0.23, p50: 0.48, p75: 0.72, p95: 0.92, p99: 0.98 },
    lab: { mean: [52, 8, 12], std: [24, 15, 17] },
    bandChroma: { shadows: 15, mids: 21, highlights: 14 },
    saturation: { mean: 0.38, std: 0.19 },
    skinPresence: 0,
    clipping: { low: 0.002, high: 0.003 },
  },
  overrides: {
    shadowTint: [-8, -12],
    highlightTint: [5, 9],
    chromaGain: 1.08,
    // Bright, punchy S.
    toneCurve: [
      [0, 0],
      [0.25, 0.22],
      [0.5, 0.51],
      [0.75, 0.8],
      [1, 1],
    ],
    channelCurves: {
      r: [
        [0, 0],
        [0.7, 0.73],
        [1, 1],
      ],
    },
    chromaShape: {
      byLuma: [
        [0, 0.95],
        [0.5, 1.05],
        [1, 0.9],
      ],
      vibrance: 0.14,
      softLimit: 60,
    },
  },
  knobs: {
    strength: { default: 0.82 },
    skinProtection: { default: 0.8 },
  },
};
