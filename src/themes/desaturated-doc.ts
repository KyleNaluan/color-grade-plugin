import type { Theme } from '../core/engine/theme.js';

/**
 * Desaturated documentary: muted, natural, neutral - a restrained non-fiction
 * look that pulls colour back without adding a cast. Distinct from bleach-bypass
 * (which is high-contrast and harsh) by staying gentle and true-to-life.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. Near-neutral LAB
 * mean + chromaGain 0.75 + mild negative vibrance mute colour evenly; the tone
 * curve stays gentle so it never reads as "graded".
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 9.9 on vlog-cave-warm - the lowest of
 * the whole library, as befits a restrained look. Skin: vlog-tricky-skin
 * -0.6deg/-16.8%, vlog-cave-warm 1.7deg/-26.2%, vlog-portait-cool 0.5deg/-6.7%,
 * rec709-outdoor -5.0deg/-13.0%. Passes the gradeGuard gate. Suits documentary,
 * news and interview footage.
 */
export const desaturatedDoc: Theme = {
  name: 'desaturated-doc',
  description: 'Muted, natural, neutral documentary colour.',
  targetStats: {
    lumaPercentiles: { p1: 0.02, p5: 0.05, p25: 0.23, p50: 0.46, p75: 0.68, p95: 0.87, p99: 0.95 },
    lab: { mean: [48, 1, 5], std: [20, 8, 10] },
    bandChroma: { shadows: 8, mids: 12, highlights: 10 },
    saturation: { mean: 0.22, std: 0.12 },
    skinPresence: 0,
    clipping: { low: 0.002, high: 0.002 },
  },
  overrides: {
    chromaGain: 0.75,
    // Gentle, natural - barely there.
    toneCurve: [
      [0, 0.01],
      [0.25, 0.25],
      [0.5, 0.48],
      [0.75, 0.71],
      [1, 0.97],
    ],
    chromaShape: {
      vibrance: -0.1,
      softLimit: 40,
    },
  },
  knobs: {
    strength: { default: 0.7 },
    skinProtection: { default: 0.8 },
  },
};
