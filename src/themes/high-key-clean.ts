import type { Theme } from '../core/engine/theme.js';

/**
 * High-key clean: bright, airy, low-contrast commercial/beauty look - lifted
 * exposure, gentle contrast, clean near-neutral colour with a light chroma trim.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. Brightness is the
 * lifted luma-percentile target; the clean feel is a near-neutral LAB mean + a
 * modest chromaGain trim so nothing reads as a cast.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 10.1 on vlog-cave-warm. Skin hue
 * barely moves: vlog-tricky-skin 0.8deg/-13.9%, vlog-cave-warm 4.4deg/-28.1%,
 * vlog-portait-cool 1.0deg/-4.9%, rec709-outdoor -4.3deg/-11.6%. Passes the
 * gradeGuard gate. Suits beauty, commercial and clean lifestyle footage.
 */
export const highKeyClean: Theme = {
  name: 'high-key-clean',
  description: 'Bright, airy, low-contrast, clean near-neutral colour.',
  targetStats: {
    lumaPercentiles: { p1: 0.06, p5: 0.12, p25: 0.35, p50: 0.58, p75: 0.78, p95: 0.94, p99: 0.99 },
    lab: { mean: [62, 1, 5], std: [18, 8, 9] },
    bandChroma: { shadows: 8, mids: 12, highlights: 10 },
    saturation: { mean: 0.26, std: 0.13 },
    skinPresence: 0,
    clipping: { low: 0.001, high: 0.003 },
  },
  overrides: {
    chromaGain: 0.9,
    // Lifted, bright, soft - airy without blowing the shoulder.
    toneCurve: [
      [0, 0.04],
      [0.25, 0.34],
      [0.5, 0.58],
      [0.75, 0.8],
      [1, 0.99],
    ],
    chromaShape: {
      softLimit: 45,
    },
  },
  knobs: {
    strength: { default: 0.72 },
    skinProtection: { default: 0.8 },
  },
};
