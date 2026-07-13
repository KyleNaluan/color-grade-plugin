import type { Theme } from '../core/engine/theme.js';

/**
 * Classic blockbuster teal-orange: punchy contrast, warm mids/highlights,
 * shadows pushed toward teal.
 *
 * Tuned against the 12 S5IIx fixture families via `npm run spike` (issue #6).
 * Evidence (grade-impact report, `scripts/lib/gradeImpact.ts`):
 * - Original mids bandChroma target (24) + chromaGain 1.05 + vibrance 0.2 drove
 *   skin-region chroma shifts up to +47% (vlog-outdoor-bright), +48% (vlog-lowlight)
 *   even with the default 0.7 skin-protection knob. Pulled mids bandChroma to 20,
 *   chromaGain to 1.0, vibrance to 0.12, and the mids chromaByLuma peak from 1.1 to
 *   1.05; skinProtection default raised 0.7 -> 0.78. Re-verified on the named skin
 *   fixtures (chroma shift / hue shift, before -> after): vlog-tricky-skin
 *   34.0%/-8.6deg -> 23.8%/-7.5deg; rec709-outdoor-with-skin-and-overexposed-sky
 *   46.1%/-5.9deg -> 36.0%/-4.8deg; vlog-portait-cool-other-people-in-background
 *   30.2%/-2.7deg -> 20.4%/-2.2deg.
 * - Suits: well-exposed outdoor/daylight footage, neutral-to-cool source stock
 *   (vlog-outdoor-cool, vlog-outdoor-bright, rec709-outdoor) where the punchy
 *   S-curve and warm push read as intentional blockbuster grade.
 * - Known limitation: on already-overexposed footage
 *   (rec709-outdoor-with-skin-and-overexposed-sky, 14.24% highlight clipping
 *   pre-grade) the punchy curve cannot recover blown highlights and leaves
 *   clipping unchanged - acceptable per issue #6, not a regression (clipped
 *   source data is simply gone).
 * - vlog-indoor-flat-tricky-skin is an outlier across all 3 themes (uncorrected
 *   tungsten WB, LAB b=~40 pre-grade) driving the largest cast-magnitude readings;
 *   this is a Correct-stage gap in the fixture, not a theme-tuning problem.
 */
export const tealOrange: Theme = {
  name: 'teal-orange',
  description: 'Punchy contrast, warm skin/highlights, teal shadows.',
  targetStats: {
    lumaPercentiles: { p1: 0.02, p5: 0.05, p25: 0.22, p50: 0.45, p75: 0.68, p95: 0.9, p99: 0.97 },
    lab: { mean: [48, 7, 10], std: [23, 14, 16] },
    bandChroma: { shadows: 14, mids: 20, highlights: 13 },
    saturation: { mean: 0.35, std: 0.18 },
    skinPresence: 0,
    clipping: { low: 0.002, high: 0.002 },
  },
  overrides: {
    shadowTint: [-9, -13],
    highlightTint: [3, 7],
    chromaGain: 1.0,
    // Punchy S-curve on top of the stat match for the blockbuster contrast.
    toneCurve: [
      [0, 0],
      [0.25, 0.21],
      [0.5, 0.5],
      [0.75, 0.79],
      [1, 1],
    ],
    // Slight extra warmth in the red channel highlights.
    channelCurves: {
      r: [
        [0, 0],
        [0.7, 0.72],
        [1, 1],
      ],
    },
    chromaShape: {
      // Chroma peaks in the mids where skin and key subjects live.
      byLuma: [
        [0, 0.95],
        [0.5, 1.05],
        [1, 0.9],
      ],
      vibrance: 0.12,
      softLimit: 60,
    },
  },
  knobs: {
    strength: { default: 0.8 },
    skinProtection: { default: 0.78 },
  },
};
