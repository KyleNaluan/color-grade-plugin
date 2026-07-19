import type { Theme } from '../core/engine/theme.js';

/**
 * Pastel dream: soft, airy, cool-mint pastel - lifted milky blacks, low
 * saturation, low contrast, a faint cool lean. Distinct from rose-romance
 * (warm pink pastel) by its cool/mint direction.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. Pastel = lifted
 * blacks + low LAB-L std + a strong chroma trim (chromaGain 0.72) so colours read
 * as washed and gentle.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 10.5 on vlog-cave-warm. Skin:
 * vlog-tricky-skin 2.6deg/-23.2%, vlog-cave-warm 4.1deg/-29.5%, vlog-portait-cool
 * 1.1deg/-13.3%, rec709-outdoor -0.4deg/-21.1%. Passes the gradeGuard gate.
 * Suits lifestyle, fashion and soft dreamy footage.
 */
export const pastelDream: Theme = {
  name: 'pastel-dream',
  description: 'Soft cool-mint pastel, lifted blacks, low saturation, airy.',
  targetStats: {
    lumaPercentiles: { p1: 0.08, p5: 0.13, p25: 0.33, p50: 0.54, p75: 0.74, p95: 0.9, p99: 0.96 },
    lab: { mean: [58, -2, 2], std: [16, 7, 8] },
    bandChroma: { shadows: 7, mids: 10, highlights: 8 },
    saturation: { mean: 0.2, std: 0.11 },
    skinPresence: 0,
    clipping: { low: 0.001, high: 0.002 },
  },
  overrides: {
    shadowTint: [-2, 2],
    highlightTint: [-1, 3],
    chromaGain: 0.72,
    // Lifted milky toe, soft shoulder - washed and gentle.
    toneCurve: [
      [0, 0.07],
      [0.25, 0.32],
      [0.5, 0.54],
      [0.75, 0.76],
      [1, 0.96],
    ],
    chromaShape: {
      vibrance: 0.05,
      softLimit: 35,
    },
  },
  knobs: {
    strength: { default: 0.7 },
    skinProtection: { default: 0.78 },
  },
};
