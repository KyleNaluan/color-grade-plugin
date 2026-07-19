import type { Theme } from '../core/engine/theme.js';

/**
 * Warm film look: gently lifted blacks, rolled-off highlights, golden bias,
 * slightly muted chroma.
 *
 * Tuned against the 12 S5IIx fixture families via `npm run spike` (issue #6).
 * Evidence (grade-impact report, `src/core/analysis/gradeImpact.ts`):
 * - Already well-behaved on the named skin fixtures at baseline (skin hue shift
 *   under 2deg everywhere); bumped skinProtection default 0.7 -> 0.75 for margin.
 *   Chroma shift / hue shift, before -> after: vlog-tricky-skin 7.8%/-1.1deg ->
 *   7.3%/-1.0deg; vlog-portait-cool-other-people-in-background 10.7%/2.0deg ->
 *   9.3%/1.7deg.
 * - rec709-outdoor-with-skin-and-overexposed-sky shows a large skin chroma-shift
 *   percentage (~39%) but a near-zero hue shift (~0.1deg): the source is nearly
 *   desaturated (mids bandChroma ~5 pre-grade, overexposed sky bleaching color
 *   out), so a modest absolute chroma injection toward the target reads as a
 *   large relative percentage. Not a garish cast - direction is stable, only
 *   magnitude grows off a near-zero base.
 * - Suits: warm/neutral, gently-exposed footage (vlog-cave-warm, vlog-bright-flat,
 *   rec709-outdoor) where soft contrast and golden bias read as intentional
 *   film character. Cross-temperature push onto cool footage (vlog-outdoor-cool)
 *   stays a modest push (cast magnitude ~12.5 LAB units) per the accepted
 *   known limitation.
 * - vlog-indoor-flat-tricky-skin is an outlier across all 3 themes (uncorrected
 *   tungsten WB); see teal-orange.ts note.
 */
export const warmFilm: Theme = {
  name: 'warm-film',
  description: 'Soft contrast, lifted blacks, golden warmth, muted chroma.',
  targetStats: {
    lumaPercentiles: { p1: 0.04, p5: 0.07, p25: 0.26, p50: 0.48, p75: 0.7, p95: 0.86, p99: 0.93 },
    lab: { mean: [52, 6, 13], std: [20, 10, 12] },
    bandChroma: { shadows: 10, mids: 18, highlights: 14 },
    saturation: { mean: 0.3, std: 0.15 },
    skinPresence: 0,
    clipping: { low: 0.001, high: 0.001 },
  },
  overrides: {
    shadowTint: [2, 4],
    highlightTint: [3, 9],
    chromaGain: 0.95,
    // Film print shape: gently lifted toe, soft highlight shoulder.
    toneCurve: [
      [0, 0.02],
      [0.25, 0.27],
      [0.5, 0.51],
      [0.75, 0.73],
      [1, 0.97],
    ],
    chromaShape: {
      // Muted highlights, near-full chroma through the mids.
      byLuma: [
        [0, 0.9],
        [0.5, 1],
        [1, 0.8],
      ],
      // Soft ceiling keeps saturated reds/oranges from going neon.
      softLimit: 55,
    },
  },
  knobs: {
    strength: { default: 0.8 },
    skinProtection: { default: 0.75 },
  },
};
