import type { Theme } from '../core/engine/theme.js';

/**
 * Winter blue: cool, clean, crisp steel-blue look with a bright, well-exposed
 * base. Distinct from cool-noir (dark, crushed, desaturated) and day-for-night
 * (very dark deep blue) - this stays bright and legible.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. The cool cast is
 * carried by the shadow/highlight blue tints, not a global mean drift; the base
 * exposure stays neutral/bright.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 12.2 on vlog-cave-warm. Skin:
 * vlog-tricky-skin -3.3deg/-17.6%, vlog-cave-warm 0.4deg/-27.0%, vlog-portait-cool
 * -2.3deg/-7.8%, rec709-outdoor -11.9deg/-13.8%. The ~-12deg on the daylight
 * frames is the same edge-of-skin-wedge artifact the shipping cool-noir/teal-orange
 * show (partial-weight pixels; see CLAUDE.md), not a protection failure. Passes the
 * gradeGuard gate. Suits winter exteriors and cool, clean daylight.
 */
export const winterBlue: Theme = {
  name: 'winter-blue',
  description: 'Cool, clean, crisp steel-blue with a bright base.',
  targetStats: {
    lumaPercentiles: { p1: 0.02, p5: 0.05, p25: 0.22, p50: 0.46, p75: 0.7, p95: 0.9, p99: 0.97 },
    lab: { mean: [50, -3, -8], std: [22, 9, 11] },
    bandChroma: { shadows: 8, mids: 12, highlights: 10 },
    saturation: { mean: 0.24, std: 0.13 },
    skinPresence: 0,
    clipping: { low: 0.003, high: 0.002 },
  },
  overrides: {
    shadowTint: [-2, -8],
    highlightTint: [-2, -6],
    chromaGain: 0.85,
    // Crisp, mild S - clean contrast without crushing.
    toneCurve: [
      [0, 0.01],
      [0.25, 0.23],
      [0.5, 0.49],
      [0.75, 0.74],
      [1, 0.98],
    ],
    chromaShape: {
      softLimit: 45,
    },
  },
  knobs: {
    strength: { default: 0.78 },
    skinProtection: { default: 0.8 },
  },
};
