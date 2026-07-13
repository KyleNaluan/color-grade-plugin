import type { Theme } from '../core/engine/theme.js';

/**
 * Cool moody look: deep shadows, desaturated, blue-leaning throughout.
 * Target stats are hand-estimated for the spike.
 */
export const coolNoir: Theme = {
  name: 'cool-noir',
  description: 'Deep shadows, desaturated, cool blue cast.',
  targetStats: {
    lumaPercentiles: { p1: 0.01, p5: 0.03, p25: 0.15, p50: 0.36, p75: 0.6, p95: 0.85, p99: 0.94 },
    lab: { mean: [40, 0, -7], std: [24, 8, 10] },
    bandChroma: { shadows: 8, mids: 12, highlights: 8 },
    saturation: { mean: 0.22, std: 0.12 },
    skinPresence: 0,
    clipping: { low: 0.004, high: 0.001 },
  },
  overrides: {
    shadowTint: [0, -7],
    chromaGain: 0.85,
  },
  knobs: {
    strength: { default: 0.75 },
    skinProtection: { default: 0.7 },
  },
};
