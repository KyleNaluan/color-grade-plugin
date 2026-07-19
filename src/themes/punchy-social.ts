import type { Theme } from '../core/engine/theme.js';

/**
 * Punchy social: bright, vivid, high-energy vlog/social look - lifted exposure,
 * strong saturation and vibrance, snappy contrast. The "pops on a phone screen"
 * grade. Distinct from summer-blockbuster (which is a teal/orange split) by being
 * a clean, un-split all-round saturation lift.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. Vibrance leans on
 * low-chroma pixels so it energises without wrecking already-saturated colour;
 * skin protection + the soft ceiling hold faces against the saturation push.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 10.2 on
 * rec709-outdoor-with-skin-and-overexposed-sky. Skin: vlog-tricky-skin
 * -2.2deg/24.3%, vlog-cave-warm 1.9deg/-4.9%, vlog-portait-cool -0.4deg/20.0%,
 * rec709-outdoor -10.6deg/26.8%. The ~-11deg on the daylight-skin frames is the
 * same edge-of-wedge artifact teal-orange shows; the skin chroma boost is bounded
 * by the soft ceiling + protection. Passes the gradeGuard gate. Suits vlogs,
 * social and travel footage.
 */
export const punchySocial: Theme = {
  name: 'punchy-social',
  description: 'Bright, vivid, high-energy social/vlog pop.',
  targetStats: {
    lumaPercentiles: { p1: 0.02, p5: 0.05, p25: 0.24, p50: 0.5, p75: 0.74, p95: 0.94, p99: 0.99 },
    lab: { mean: [54, 3, 8], std: [24, 14, 15] },
    bandChroma: { shadows: 13, mids: 20, highlights: 15 },
    saturation: { mean: 0.4, std: 0.2 },
    skinPresence: 0,
    clipping: { low: 0.002, high: 0.003 },
  },
  overrides: {
    chromaGain: 1.12,
    // Bright, snappy S.
    toneCurve: [
      [0, 0],
      [0.25, 0.23],
      [0.5, 0.51],
      [0.75, 0.79],
      [1, 1],
    ],
    chromaShape: {
      byLuma: [
        [0, 0.95],
        [0.5, 1.05],
        [1, 0.95],
      ],
      vibrance: 0.2,
      softLimit: 60,
    },
  },
  knobs: {
    strength: { default: 0.82 },
    skinProtection: { default: 0.8 },
  },
};
