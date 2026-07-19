import type { Theme } from '../core/engine/theme.js';

/**
 * Autumn: warm, earthy, rich - golds, oranges and reds with deep, saturated
 * mids. A fall-foliage / harvest look distinct from golden-hour (which is a
 * softer, glowier sun) by its richer, more contrasted mids.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. Warmth is carried
 * by all three band tints (amber b + red a) plus a modest chroma boost; the soft
 * ceiling and skin protection keep skin from over-warming.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 15.6 on the skin-free
 * vlog-outdoor-bright-flat-2 (where even the shipping teal-orange hits ~12.6).
 * Skin: vlog-tricky-skin -3.0deg/25.7%, vlog-cave-warm -0.9deg/-1.7%,
 * vlog-portait-cool 0.3deg/18.2%, rec709-outdoor -9.4deg/27.7%. Reduced from a
 * warmer initial draft; the gradeGuard gate accepts it. Suits fall exteriors,
 * foliage and harvest scenes.
 */
export const autumn: Theme = {
  name: 'autumn',
  description: 'Warm earthy golds and reds, rich saturated mids.',
  targetStats: {
    lumaPercentiles: { p1: 0.02, p5: 0.04, p25: 0.2, p50: 0.44, p75: 0.68, p95: 0.88, p99: 0.96 },
    lab: { mean: [50, 7, 14], std: [21, 12, 15] },
    bandChroma: { shadows: 12, mids: 20, highlights: 16 },
    saturation: { mean: 0.36, std: 0.18 },
    skinPresence: 0,
    clipping: { low: 0.003, high: 0.002 },
  },
  overrides: {
    shadowTint: [3, 4],
    midtoneTint: [3, 5],
    highlightTint: [4, 8],
    chromaGain: 1.0,
    // Rich S for contrasted, deep foliage mids.
    toneCurve: [
      [0, 0],
      [0.25, 0.22],
      [0.5, 0.49],
      [0.75, 0.74],
      [1, 0.97],
    ],
    chromaShape: {
      byLuma: [
        [0, 0.95],
        [0.5, 1.05],
        [1, 0.92],
      ],
      vibrance: 0.1,
      softLimit: 60,
    },
  },
  knobs: {
    strength: { default: 0.8 },
    skinProtection: { default: 0.8 },
  },
};
