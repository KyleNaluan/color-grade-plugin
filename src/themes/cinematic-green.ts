import type { Theme } from '../core/engine/theme.js';

/**
 * Cinematic green: the "digital rain" sickly-green look - a green cast pushed
 * through shadows, mids and highlights with reduced overall saturation, evoking
 * fluorescent institutional/thriller grades.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. The green is
 * carried by negative-a tints across all bands (LAB -a = green) plus a slightly
 * green LAB mean; chromaGain 0.7 keeps it desaturated and clinical rather than
 * lurid. Skin protection is a little lower (0.72) because the whole point is that
 * even skin picks up the green pallor.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 12.2 on
 * rec709-outdoor-with-skin-and-overexposed-sky. Skin picks up the green pallor BY
 * DESIGN: vlog-tricky-skin 14.0deg/-12.6%, vlog-cave-warm 12.1deg/-22.7%,
 * vlog-portait-cool 9.0deg/-7.0%, rec709-outdoor 7.4deg/-9.7%. This is the most
 * skin-recolouring of the non-monochrome looks - the whole point of the sickly
 * green - so skinProtection is deliberately lower (0.72). Passes the gradeGuard
 * gate. Best on thriller/institutional/horror footage, NOT flattering portraits.
 */
export const cinematicGreen: Theme = {
  name: 'cinematic-green',
  description: 'Sickly digital-rain green cast, desaturated and clinical.',
  targetStats: {
    lumaPercentiles: { p1: 0.01, p5: 0.03, p25: 0.18, p50: 0.42, p75: 0.68, p95: 0.9, p99: 0.97 },
    lab: { mean: [46, -6, 8], std: [22, 8, 10] },
    bandChroma: { shadows: 8, mids: 12, highlights: 10 },
    saturation: { mean: 0.22, std: 0.12 },
    skinPresence: 0,
    clipping: { low: 0.003, high: 0.003 },
  },
  overrides: {
    shadowTint: [-8, 6],
    midtoneTint: [-6, 8],
    highlightTint: [-4, 10],
    chromaGain: 0.7,
    // Moderate S.
    toneCurve: [
      [0, 0],
      [0.25, 0.22],
      [0.5, 0.48],
      [0.75, 0.73],
      [1, 0.97],
    ],
    chromaShape: {
      vibrance: -0.1,
      softLimit: 40,
    },
  },
  knobs: {
    strength: { default: 0.8 },
    skinProtection: { default: 0.72 },
  },
};
