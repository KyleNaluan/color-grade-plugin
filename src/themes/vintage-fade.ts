import type { Theme } from '../core/engine/theme.js';

/**
 * Vintage fade: faded photochemical print - milky lifted blacks, low contrast,
 * muted chroma, a faint warm-green cast in the shadows.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. The "faded" feel
 * is the lifted p1/p5 target (blacks never reach 0) + a low LAB-L std (low
 * contrast); muting is chromaGain 0.8 + a soft ceiling.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin, a documented fixture gap - see teal-orange.ts):
 * worst cast 11.5 on vlog-cave-warm. Skin stays safe under the gentle
 * desaturation: vlog-tricky-skin 0.8deg/-21.5%, vlog-cave-warm 3.9deg/-32.8%,
 * vlog-portait-cool 1.4deg/-10.1%, rec709-outdoor -2.7deg/-17.7%. Passes the
 * gradeGuard gate. Suits nostalgic/retro and Super-8-style footage.
 */
export const vintageFade: Theme = {
  name: 'vintage-fade',
  description: 'Milky lifted blacks, low contrast, muted chroma, faded print.',
  targetStats: {
    lumaPercentiles: { p1: 0.08, p5: 0.12, p25: 0.3, p50: 0.5, p75: 0.68, p95: 0.82, p99: 0.9 },
    lab: { mean: [54, 2, 8], std: [16, 8, 10] },
    bandChroma: { shadows: 8, mids: 12, highlights: 10 },
    saturation: { mean: 0.24, std: 0.13 },
    skinPresence: 0,
    clipping: { low: 0.001, high: 0.001 },
  },
  overrides: {
    shadowTint: [-2, 4],
    highlightTint: [2, 6],
    chromaGain: 0.8,
    // Lifted milky toe, rolled highlights, low overall contrast.
    toneCurve: [
      [0, 0.08],
      [0.25, 0.31],
      [0.5, 0.5],
      [0.75, 0.68],
      [1, 0.92],
    ],
    chromaShape: {
      vibrance: -0.1,
      softLimit: 45,
    },
  },
  knobs: {
    strength: { default: 0.75 },
    skinProtection: { default: 0.78 },
  },
};
