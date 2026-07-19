import type { Theme } from '../core/engine/theme.js';

/**
 * Bleach bypass: silver-retention look - high contrast, crushed blacks, harsh
 * highlights, heavily reduced chroma (the desaturated, metallic war-film grade).
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. Desaturation comes
 * from a low chromaGain plus low band-chroma targets and a mild negative vibrance;
 * the tone contrast comes from the high LAB-L std target + a hard S-curve.
 *
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 12.1 on vlog-cave-warm. Because the
 * look desaturates, skin hue barely moves and skin chroma drops as intended:
 * vlog-tricky-skin -1.7deg/-26.5%, vlog-cave-warm 0.1deg/-33.3%, vlog-portait-cool
 * -0.6deg/-14.0%, rec709-outdoor -5.0deg/-24.3%. Passes the gradeGuard gate.
 * Suits gritty/war-film and high-contrast dramatic footage.
 */
export const bleachBypass: Theme = {
  name: 'bleach-bypass',
  description: 'High contrast, crushed blacks, harsh highlights, silver desaturation.',
  targetStats: {
    lumaPercentiles: { p1: 0.005, p5: 0.02, p25: 0.16, p50: 0.42, p75: 0.72, p95: 0.94, p99: 0.99 },
    lab: { mean: [46, 2, 4], std: [28, 7, 8] },
    bandChroma: { shadows: 6, mids: 9, highlights: 6 },
    saturation: { mean: 0.16, std: 0.1 },
    skinPresence: 0,
    clipping: { low: 0.006, high: 0.006 },
  },
  overrides: {
    chromaGain: 0.55,
    // Hard S: crush the toe, drive the shoulder for the harsh silver highlights.
    toneCurve: [
      [0, 0],
      [0.2, 0.13],
      [0.5, 0.5],
      [0.8, 0.9],
      [1, 1],
    ],
    chromaShape: {
      vibrance: -0.2,
      softLimit: 30,
    },
  },
  knobs: {
    strength: { default: 0.8 },
    skinProtection: { default: 0.72 },
  },
};
