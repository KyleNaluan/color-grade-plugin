import type { Theme } from '../core/engine/theme.js';

/**
 * Muted teal-orange: the modern, understated cousin of teal-orange - the same
 * complementary shadow/highlight split, but desaturated and lower-contrast for a
 * refined, contemporary streaming look. Kept distinct from teal-orange and
 * summer-blockbuster by its restraint (chromaGain 0.82, gentler curve).
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. Same teal/warm
 * split direction, but muted so it never reads as garish.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 10.9 on vlog-cave-warm. Skin:
 * vlog-tricky-skin -2.8deg/-10.9%, vlog-cave-warm 1.3deg/-25.0%, vlog-portait-cool
 * -0.4deg/-1.5%, rec709-outdoor -10.0deg/-6.0%. Passes the gradeGuard gate.
 * Suits modern drama and streaming-style footage.
 */
export const mutedTealOrange: Theme = {
  name: 'muted-teal-orange',
  description: 'Understated desaturated teal-orange, refined and modern.',
  targetStats: {
    lumaPercentiles: { p1: 0.03, p5: 0.06, p25: 0.24, p50: 0.47, p75: 0.69, p95: 0.87, p99: 0.94 },
    lab: { mean: [50, 4, 6], std: [20, 10, 12] },
    bandChroma: { shadows: 10, mids: 14, highlights: 10 },
    saturation: { mean: 0.26, std: 0.14 },
    skinPresence: 0,
    clipping: { low: 0.002, high: 0.002 },
  },
  overrides: {
    shadowTint: [-5, -8],
    highlightTint: [3, 6],
    chromaGain: 0.82,
    // Gentle S - refined, not punchy.
    toneCurve: [
      [0, 0.01],
      [0.25, 0.25],
      [0.5, 0.49],
      [0.75, 0.72],
      [1, 0.97],
    ],
    chromaShape: {
      vibrance: -0.05,
      softLimit: 45,
    },
  },
  knobs: {
    strength: { default: 0.75 },
    skinProtection: { default: 0.8 },
  },
};
