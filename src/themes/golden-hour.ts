import type { Theme } from '../core/engine/theme.js';

/**
 * Golden hour: warm amber glow, softly lifted warm shadows, gently rolled
 * highlights - the low, honeyed sun look.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report
 * (`src/core/analysis/gradeImpact.ts`). The warmth lives in the highlight/mid
 * tints (amber b-bias) rather than the damped LAB mean shift, so the cast stays
 * intentional without a global colour drift.
 *
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin, a documented fixture gap - see teal-orange.ts):
 * worst cast 13.9 on vlog-outdoor-cool (a warm push onto cool footage, the same
 * intentional cross-temperature move warm-film documents at ~12.5). Skin hue /
 * chroma shift on the skin fixtures stays gentle: vlog-tricky-skin -1.2deg/14.5%,
 * vlog-cave-warm 2.0deg/-12.4%, vlog-portait-cool 1.9deg/13.4%, rec709-outdoor
 * -8.1deg/20.4%. Pulled back from a stronger initial draft (worst cast ~20, 8
 * STRONG frames) by halving the warm mean-b (20->11) and the band tints; the
 * gradeGuard gate accepts it. Suits warm/neutral daylight; on cool sources the
 * warm push is large-but-intended.
 */
export const goldenHour: Theme = {
  name: 'golden-hour',
  description: 'Warm amber glow, lifted warm shadows, soft honeyed highlights.',
  targetStats: {
    lumaPercentiles: { p1: 0.03, p5: 0.06, p25: 0.25, p50: 0.47, p75: 0.69, p95: 0.88, p99: 0.95 },
    lab: { mean: [54, 4, 11], std: [20, 11, 14] },
    bandChroma: { shadows: 12, mids: 18, highlights: 16 },
    saturation: { mean: 0.32, std: 0.16 },
    skinPresence: 0,
    clipping: { low: 0.002, high: 0.002 },
  },
  overrides: {
    shadowTint: [2, 3],
    midtoneTint: [2, 4],
    highlightTint: [3, 7],
    chromaGain: 1.0,
    // Soft lifted toe, gentle highlight shoulder for the glow.
    toneCurve: [
      [0, 0.02],
      [0.25, 0.27],
      [0.5, 0.51],
      [0.75, 0.73],
      [1, 0.97],
    ],
    chromaShape: {
      byLuma: [
        [0, 0.9],
        [0.5, 1.05],
        [1, 0.95],
      ],
      vibrance: 0.08,
      softLimit: 60,
    },
  },
  knobs: {
    strength: { default: 0.8 },
    skinProtection: { default: 0.8 },
  },
};
