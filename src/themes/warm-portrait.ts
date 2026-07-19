import type { Theme } from '../core/engine/theme.js';

/**
 * Warm portrait: flattering skin-forward look - soft warm mids, healthy skin
 * tones, gentle contrast, high skin protection so faces stay natural.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. The 0.82 skin
 * protection knob is intentionally the highest of the library - the whole point is
 * flattering, low-shift skin; warmth sits in the highlight/mid tints.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 12.7 on vlog-outdoor-cool (a warm
 * push onto cool footage). Skin, the priority here, stays flattering:
 * vlog-tricky-skin -1.6deg/8.3%, vlog-cave-warm 0.9deg/-14.8%, vlog-portait-cool
 * 1.1deg/8.0%, rec709-outdoor -7.9deg/12.8%. Passes the gradeGuard gate. Suits
 * interviews, portraits and weddings.
 */
export const warmPortrait: Theme = {
  name: 'warm-portrait',
  description: 'Flattering skin, soft warm mids, gentle contrast.',
  targetStats: {
    lumaPercentiles: { p1: 0.03, p5: 0.06, p25: 0.25, p50: 0.48, p75: 0.7, p95: 0.88, p99: 0.95 },
    lab: { mean: [54, 6, 12], std: [19, 10, 12] },
    bandChroma: { shadows: 10, mids: 16, highlights: 13 },
    saturation: { mean: 0.3, std: 0.15 },
    skinPresence: 0,
    clipping: { low: 0.002, high: 0.002 },
  },
  overrides: {
    shadowTint: [2, 2],
    midtoneTint: [2, 4],
    highlightTint: [4, 8],
    chromaGain: 0.98,
    // Soft, gentle - no crushing, flattering rolloff.
    toneCurve: [
      [0, 0.01],
      [0.25, 0.26],
      [0.5, 0.5],
      [0.75, 0.72],
      [1, 0.97],
    ],
    chromaShape: {
      byLuma: [
        [0, 0.95],
        [0.5, 1.03],
        [1, 0.92],
      ],
      softLimit: 55,
    },
  },
  knobs: {
    strength: { default: 0.75 },
    skinProtection: { default: 0.82 },
  },
};
