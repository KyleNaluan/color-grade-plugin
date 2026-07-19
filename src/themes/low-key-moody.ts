import type { Theme } from '../core/engine/theme.js';

/**
 * Low-key moody: dark, crushed shadows, controlled highlights, a neutral-to-warm
 * lean. Distinct from cool-noir (which is blue and desaturated) - this one keeps
 * a warmer, richer midtone.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. Darkness/mood is
 * the pulled-down luma target + crushed toneCurve toe; chroma is only gently
 * muted (chromaGain 0.85) so the mids stay rich, unlike cool-noir.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 9.8 on vlog-cave-warm. Skin is well
 * protected: vlog-tricky-skin -2.7deg/-16.8%, vlog-cave-warm -1.0deg/-24.4%,
 * vlog-portait-cool -0.8deg/-3.0%, rec709-outdoor -9.2deg/-9.1%. Passes the
 * gradeGuard gate. Suits drama, interviews and night interiors.
 */
export const lowKeyMoody: Theme = {
  name: 'low-key-moody',
  description: 'Dark, crushed shadows, controlled highlights, warm-neutral mood.',
  targetStats: {
    lumaPercentiles: { p1: 0.005, p5: 0.02, p25: 0.13, p50: 0.33, p75: 0.58, p95: 0.82, p99: 0.92 },
    lab: { mean: [38, 2, 5], std: [22, 9, 11] },
    bandChroma: { shadows: 8, mids: 13, highlights: 10 },
    saturation: { mean: 0.26, std: 0.14 },
    skinPresence: 0,
    clipping: { low: 0.005, high: 0.002 },
  },
  overrides: {
    shadowTint: [1, 1],
    chromaGain: 0.9,
    // Crushed toe, controlled shoulder - contrast lives down low.
    toneCurve: [
      [0, 0],
      [0.2, 0.13],
      [0.5, 0.42],
      [0.75, 0.66],
      [1, 0.9],
    ],
    chromaShape: {
      // Mute what little chroma survives in the shadows.
      byLuma: [
        [0, 0.75],
        [0.5, 1],
        [1, 0.9],
      ],
      vibrance: -0.1,
      softLimit: 45,
    },
  },
  knobs: {
    strength: { default: 0.78 },
    skinProtection: { default: 0.76 },
  },
};
