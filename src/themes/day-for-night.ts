import type { Theme } from '../core/engine/theme.js';

/**
 * Day for night: the classic "shoot in daylight, grade to moonlight" look - deep
 * underexposed base, cool deep-blue cast throughout, crushed shadows. Distinct
 * from winter-blue (bright cool) and cool-noir (moderate) by how dark it goes.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. The pulled-down
 * luma target + toneCurve is what sells "night"; the blue is carried across all
 * three band tints so it reads uniformly rather than as a shadow-only split.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 16.2 on vlog-cave-warm (a warm cave
 * pulled to cool night - a large-but-intended cross-temperature move). Skin goes
 * cool by design (you cannot have warm skin in moonlight): vlog-tricky-skin
 * -4.9deg/-32.9%, vlog-cave-warm -1.4deg/-37.7%, vlog-portait-cool -4.0deg/-14.7%,
 * rec709-outdoor -12.8deg/-27.6%. Raised skinProtection to 0.83 from a stronger
 * draft, which the gradeGuard gate scored an improvement. Most source-dependent
 * look: fully sells on already-dim footage; on bright daylight it reads as a
 * restrained cool dusk rather than full night.
 */
export const dayForNight: Theme = {
  name: 'day-for-night',
  description: 'Deep underexposed moonlight, cool blue cast, crushed shadows.',
  targetStats: {
    lumaPercentiles: { p1: 0.003, p5: 0.015, p25: 0.1, p50: 0.26, p75: 0.48, p95: 0.72, p99: 0.85 },
    lab: { mean: [30, -2, -9], std: [18, 8, 11] },
    bandChroma: { shadows: 8, mids: 11, highlights: 9 },
    saturation: { mean: 0.22, std: 0.12 },
    skinPresence: 0,
    clipping: { low: 0.006, high: 0.001 },
  },
  overrides: {
    shadowTint: [-2, -9],
    midtoneTint: [-2, -6],
    highlightTint: [-1, -4],
    chromaGain: 0.8,
    // Pull the whole image down into moonlight.
    toneCurve: [
      [0, 0],
      [0.25, 0.16],
      [0.5, 0.32],
      [0.75, 0.52],
      [1, 0.78],
    ],
    chromaShape: {
      vibrance: -0.1,
      softLimit: 40,
    },
  },
  knobs: {
    strength: { default: 0.8 },
    skinProtection: { default: 0.83 },
  },
};
