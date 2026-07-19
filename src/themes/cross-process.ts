import type { Theme } from '../core/engine/theme.js';

/**
 * Cross process: the film "wrong chemistry" look - cyan-blue crushed shadows,
 * yellow-green highlights, high contrast and boosted chroma. A split-tone in the
 * opposite direction from teal-orange (cool shadows, but green-yellow rather than
 * warm highlights), so it stays distinct from the teal-orange family.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. The characteristic
 * clash is cyan shadowTint vs yellow-green highlightTint; the soft ceiling + skin
 * protection keep skin from turning green under the highlight tint.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 9.7 on
 * rec709-outdoor-with-skin-and-overexposed-sky. Skin: vlog-tricky-skin
 * -0.3deg/13.2%, vlog-cave-warm -0.3deg/-5.5%, vlog-portait-cool -0.9deg/14.0%,
 * rec709-outdoor -10.2deg/15.6%. Passes the gradeGuard gate. Suits music-video,
 * fashion and experimental footage.
 */
export const crossProcess: Theme = {
  name: 'cross-process',
  description: 'Cyan shadows, yellow-green highlights, high contrast, boosted chroma.',
  targetStats: {
    lumaPercentiles: { p1: 0.005, p5: 0.02, p25: 0.16, p50: 0.42, p75: 0.72, p95: 0.94, p99: 0.99 },
    lab: { mean: [48, -2, 6], std: [26, 13, 14] },
    bandChroma: { shadows: 12, mids: 16, highlights: 14 },
    saturation: { mean: 0.34, std: 0.18 },
    skinPresence: 0,
    clipping: { low: 0.005, high: 0.004 },
  },
  overrides: {
    shadowTint: [-6, -10],
    highlightTint: [-4, 14],
    chromaGain: 1.05,
    // Strong S for the cross-processed contrast.
    toneCurve: [
      [0, 0],
      [0.2, 0.14],
      [0.5, 0.5],
      [0.8, 0.88],
      [1, 1],
    ],
    chromaShape: {
      vibrance: 0.1,
      softLimit: 55,
    },
  },
  knobs: {
    strength: { default: 0.8 },
    skinProtection: { default: 0.78 },
  },
};
