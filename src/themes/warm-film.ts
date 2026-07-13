import type { Theme } from '../core/engine/theme.js';

/**
 * Warm film look: gently lifted blacks, rolled-off highlights, golden bias,
 * slightly muted chroma. Target stats are hand-estimated for the spike.
 */
export const warmFilm: Theme = {
  name: 'warm-film',
  description: 'Soft contrast, lifted blacks, golden warmth, muted chroma.',
  targetStats: {
    lumaPercentiles: { p1: 0.04, p5: 0.07, p25: 0.26, p50: 0.48, p75: 0.7, p95: 0.86, p99: 0.93 },
    lab: { mean: [52, 6, 13], std: [20, 10, 12] },
    bandChroma: { shadows: 10, mids: 18, highlights: 14 },
    saturation: { mean: 0.3, std: 0.15 },
    skinPresence: 0,
    clipping: { low: 0.001, high: 0.001 },
  },
  overrides: {
    shadowTint: [2, 4],
    highlightTint: [3, 9],
    chromaGain: 0.95,
    // Film print shape: gently lifted toe, soft highlight shoulder.
    toneCurve: [
      [0, 0.02],
      [0.25, 0.27],
      [0.5, 0.51],
      [0.75, 0.73],
      [1, 0.97],
    ],
    chromaShape: {
      // Muted highlights, near-full chroma through the mids.
      byLuma: [
        [0, 0.9],
        [0.5, 1],
        [1, 0.8],
      ],
      // Soft ceiling keeps saturated reds/oranges from going neon.
      softLimit: 55,
    },
  },
  knobs: {
    strength: { default: 0.8 },
    skinProtection: { default: 0.7 },
  },
};
