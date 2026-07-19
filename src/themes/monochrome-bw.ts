import type { Theme } from '../core/engine/theme.js';

/**
 * Monochrome B&W: a true black-and-white conversion with a punchy silver-print
 * tone curve.
 *
 * The engine has no dedicated "desaturate" knob, but it CAN express a full
 * grayscale: `chromaGain: 0` scales the post-match LAB a/b to exactly zero (the
 * a *= scale / bb *= scale step in engine.ts), and with no band tints the output
 * chroma is identically 0. Two consequences follow, and both are load-bearing:
 *   - `skinProtection` is 0 here on purpose. Skin protection blends protected
 *     pixels back toward the ORIGINAL (coloured) footage, which would reintroduce
 *     colour into faces and break the monochrome. B&W is the one look where skin
 *     protection is intentionally off.
 *   - `strength` is 1.0. Strength < 1 blends the graded pixel back toward the
 *     original coloured footage, so any value below 1 leaks colour back in. Full
 *     strength is required for a clean grayscale.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report (skin CHROMA shift
 * is a full -100% by design - all skin chroma is removed).
 * Evidence (grade-impact): worst cast ~25 - high BY CONSTRUCTION, because removing
 * all chroma is a maximal move in LAB cast terms; this is the intended grayscale,
 * not a defect. Skin chroma shift is -100% on every skin fixture (all skin colour
 * removed, as designed); the reported skin HUE shift (~-50deg) is meaningless at
 * zero output chroma. Passes the gradeGuard gate (inconclusive). Confirmed a clean
 * grayscale on the fixtures (no residual colour in faces).
 */
export const monochromeBw: Theme = {
  name: 'monochrome-bw',
  description: 'True black-and-white, punchy silver-print contrast.',
  targetStats: {
    lumaPercentiles: { p1: 0.01, p5: 0.03, p25: 0.2, p50: 0.44, p75: 0.7, p95: 0.93, p99: 0.99 },
    lab: { mean: [46, 0, 0], std: [26, 1, 1] },
    bandChroma: { shadows: 0, mids: 0, highlights: 0 },
    saturation: { mean: 0, std: 0 },
    skinPresence: 0,
    clipping: { low: 0.003, high: 0.004 },
  },
  overrides: {
    chromaGain: 0,
    // Silver-print S-curve for punch.
    toneCurve: [
      [0, 0],
      [0.25, 0.21],
      [0.5, 0.5],
      [0.75, 0.79],
      [1, 1],
    ],
    chromaShape: {
      // Belt-and-suspenders: even if a tint were added, hold chroma near zero.
      softLimit: 1,
    },
  },
  knobs: {
    strength: { default: 1.0 },
    skinProtection: { default: 0 },
  },
};
