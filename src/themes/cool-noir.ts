import type { Theme } from '../core/engine/theme.js';

/**
 * Cool moody look: deep shadows, desaturated, blue-leaning throughout.
 *
 * Tuned against the 12 S5IIx fixture families via `npm run spike` (issue #6).
 * Evidence (grade-impact report, `scripts/lib/gradeImpact.ts`):
 * - Best-behaved of the 3 themes at baseline: highlight clipping drops to ~0%
 *   on nearly every fixture (the restrained toneCurve shoulder does its job),
 *   and skin hue shift stayed under 5deg on all fixtures with meaningful skin
 *   presence. Trimmed vibrance -0.25 -> -0.2 and bumped skinProtection default
 *   0.7 -> 0.75 for margin/parity with the other two themes. Chroma shift / hue
 *   shift, before -> after: vlog-tricky-skin -25.4%/-4.3deg -> -23.0%/-4.1deg;
 *   vlog-portait-cool-other-people-in-background -13.2%/-3.5deg -> -11.1%/-3.2deg.
 * - rec709-outdoor-with-skin-and-overexposed-sky reports a larger skin hue
 *   shift (~11deg) but the source's skin-classified pixels sit right at the
 *   chroma>=6 detector floor on this near-desaturated, overexposed frame -
 *   hue angle is inherently noisy at near-zero chroma. Chroma shift itself
 *   stays small (~5%), so this reads as measurement noise, not a real cast.
 * - Suits: low-key, moody, or already-cool footage (vlog-lowlight,
 *   vlog-city-night, vlog-outdoor-cool) where the crushed shadows and blue
 *   lean read as intentional; safest of the 3 themes on already-bright or
 *   overexposed footage since it pulls highlights down rather than pushing
 *   contrast up.
 * - vlog-indoor-flat-tricky-skin is an outlier across all 3 themes (uncorrected
 *   tungsten WB); see teal-orange.ts note.
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
    // Crushed toe, restrained highlights: the noir contrast lives down low.
    toneCurve: [
      [0, 0],
      [0.2, 0.15],
      [0.5, 0.48],
      [1, 0.96],
    ],
    chromaShape: {
      // Mute what little chroma survives, hardest in the shadows.
      byLuma: [
        [0, 0.7],
        [0.5, 0.95],
        [1, 0.85],
      ],
      vibrance: -0.2,
      softLimit: 40,
    },
  },
  knobs: {
    strength: { default: 0.75 },
    skinProtection: { default: 0.75 },
  },
};
