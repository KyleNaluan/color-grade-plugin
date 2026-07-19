import type { Theme } from '../core/engine/theme.js';

/**
 * "None / Manual" - no automatic stat-matched look (Phase 6a).
 *
 * `matchStats: false` turns off the engine's tone-curve stat match, LAB mean/std
 * transfer, per-band chroma scaling, and the chroma-overshoot guard, so this
 * theme contributes exact identity on its own. Manual primary correction
 * (exposure/contrast/hi-sh-wh-bl/temp-tint/saturation/vibrance) is then the
 * entire grade, with no stat-match staleness interaction: this is the
 * first-class path for fully manual grading.
 *
 * `targetStats` are unused while `matchStats` is false (they are only carried in
 * the persisted recipe); they hold plausible neutral placeholders. Strength
 * defaults to 100% so a manual grade shows at full intensity out of the box;
 * skin protection stays on so manual saturation pushes are protected in the skin
 * wedge like every other theme (decision D3).
 */
export const noneManual: Theme = {
  name: 'none-manual',
  description: 'No automatic look - manual grading only.',
  matchStats: false,
  targetStats: {
    lumaPercentiles: { p1: 0.02, p5: 0.05, p25: 0.25, p50: 0.5, p75: 0.75, p95: 0.95, p99: 0.98 },
    lab: { mean: [50, 0, 0], std: [22, 10, 10] },
    bandChroma: { shadows: 10, mids: 12, highlights: 10 },
    saturation: { mean: 0.3, std: 0.15 },
    skinPresence: 0,
    clipping: { low: 0.002, high: 0.002 },
  },
  knobs: {
    strength: { default: 1 },
    skinProtection: { default: 0.75 },
  },
};
