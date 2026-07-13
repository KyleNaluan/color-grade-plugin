import type { Theme } from '../core/engine/theme.js';

/**
 * Classic blockbuster teal-orange: punchy contrast, warm mids/highlights,
 * shadows pushed toward teal. Target stats are hand-estimated for the spike.
 */
export const tealOrange: Theme = {
  name: 'teal-orange',
  description: 'Punchy contrast, warm skin/highlights, teal shadows.',
  targetStats: {
    lumaPercentiles: { p1: 0.02, p5: 0.05, p25: 0.22, p50: 0.45, p75: 0.68, p95: 0.9, p99: 0.97 },
    lab: { mean: [48, 7, 10], std: [23, 14, 16] },
    bandChroma: { shadows: 14, mids: 24, highlights: 13 },
    saturation: { mean: 0.35, std: 0.18 },
    skinPresence: 0,
    clipping: { low: 0.002, high: 0.002 },
  },
  overrides: {
    shadowTint: [-9, -13],
    highlightTint: [3, 7],
    chromaGain: 1.05,
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
        [0.5, 1.1],
        [1, 0.9],
      ],
      vibrance: 0.2,
      softLimit: 60,
    },
  },
  knobs: {
    strength: { default: 0.8 },
    skinProtection: { default: 0.7 },
  },
};
