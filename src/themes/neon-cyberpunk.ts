import type { Theme } from '../core/engine/theme.js';

/**
 * Neon cyberpunk: high-saturation night look - teal/cyan crushed shadows,
 * magenta-leaning highlights, punchy contrast. The blade-runner split-tone.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report. The split-tone is
 * the opposed shadow (cyan) / highlight (magenta) tints; saturation is pushed
 * (chromaGain 1.15 + vibrance) but the soft ceiling and skin protection keep skin
 * from going neon.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 14.7 on the skin-free
 * vlog-outdoor-bright-flat-2. Skin picks up the split-tone by design:
 * vlog-tricky-skin -9.3deg/14.7%, vlog-cave-warm -6.6deg/-0.8%, vlog-portait-cool
 * -6.3deg/12.4%, rec709-outdoor -18.4deg/18.0%. The ~-18/-19deg on the low-chroma
 * daylight-skin frames is the edge-of-wedge artifact amplified by this strong
 * look; raised skinProtection to 0.85 and trimmed the magenta highlight tint from
 * a stronger draft, which the gradeGuard gate scored an improvement. Best on
 * night/urban/music-video footage, NOT flattering portraits.
 */
export const neonCyberpunk: Theme = {
  name: 'neon-cyberpunk',
  description: 'Teal/cyan shadows, magenta highlights, punchy neon saturation.',
  targetStats: {
    lumaPercentiles: { p1: 0.005, p5: 0.02, p25: 0.14, p50: 0.36, p75: 0.64, p95: 0.9, p99: 0.98 },
    lab: { mean: [42, 4, -4], std: [24, 14, 15] },
    bandChroma: { shadows: 14, mids: 20, highlights: 16 },
    saturation: { mean: 0.4, std: 0.2 },
    skinPresence: 0,
    clipping: { low: 0.006, high: 0.004 },
  },
  overrides: {
    shadowTint: [-5, -9],
    highlightTint: [5, -3],
    chromaGain: 1.1,
    // Punchy S with crushed toe for the night contrast.
    toneCurve: [
      [0, 0],
      [0.2, 0.14],
      [0.5, 0.5],
      [0.8, 0.88],
      [1, 1],
    ],
    chromaShape: {
      byLuma: [
        [0, 0.95],
        [0.5, 1.08],
        [1, 1.0],
      ],
      vibrance: 0.15,
      softLimit: 60,
    },
  },
  knobs: {
    strength: { default: 0.8 },
    skinProtection: { default: 0.85 },
  },
};
