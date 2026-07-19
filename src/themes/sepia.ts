import type { Theme } from '../core/engine/theme.js';

/**
 * Sepia: a warm-toned monochrome (antique photographic print). Built exactly like
 * monochrome-bw - `chromaGain: 0` removes ALL of the footage's own colour - and
 * then a single uniform warm tone is painted back in via the band tints. Because
 * the tints are added AFTER the chroma scale (engine.ts step 3), they are the
 * entire colour of the image: the result is a tinted grayscale, i.e. sepia.
 *
 * Like monochrome-bw, `skinProtection` is 0 (protection would leak the original
 * skin colour back over the tone) and `strength` is 1.0 (any lower blends the
 * original colour back in). See monochrome-bw.ts for the full rationale.
 *
 * Authored non-interactively via `npm run author` (scripts/authorTheme.ts) against
 * the 12 fixture families; evidence is the grade-impact report.
 * Evidence (grade-impact; worst cast excludes the tungsten-WB outlier
 * vlog-indoor-flat-tricky-skin): worst cast 16.7 on vlog-city-night - expected,
 * since every pixel is recoloured to one warm tone. Skin is pushed to the sepia
 * tone BY DESIGN (a toned monochrome): vlog-tricky-skin 8.3deg/-31.5%,
 * vlog-cave-warm 2.8deg/-46.3%, vlog-portait-cool 14.4deg/-35.8%, rec709-outdoor
 * 7.6deg/-38.9%. The positive skin-hue "shift" is the tone, not a protection
 * failure. Passes the gradeGuard gate. Confirmed a uniform warm-brown tone on the
 * fixtures.
 */
export const sepia: Theme = {
  name: 'sepia',
  description: 'Warm-toned antique monochrome print.',
  targetStats: {
    lumaPercentiles: { p1: 0.03, p5: 0.06, p25: 0.24, p50: 0.48, p75: 0.72, p95: 0.9, p99: 0.97 },
    lab: { mean: [48, 6, 18], std: [22, 1, 1] },
    bandChroma: { shadows: 0, mids: 0, highlights: 0 },
    saturation: { mean: 0, std: 0 },
    skinPresence: 0,
    clipping: { low: 0.002, high: 0.002 },
  },
  overrides: {
    // chromaGain 0 strips the footage colour; the tints are then the ONLY colour,
    // so the whole frame carries one warm brown tone (uniform across the bands).
    shadowTint: [5, 12],
    midtoneTint: [7, 16],
    highlightTint: [8, 20],
    chromaGain: 0,
    // Gentle warm-print curve, slightly lifted toe.
    toneCurve: [
      [0, 0.03],
      [0.25, 0.27],
      [0.5, 0.5],
      [0.75, 0.73],
      [1, 0.98],
    ],
    chromaShape: {
      // Keep the tone bounded so the brown never oversaturates.
      softLimit: 30,
    },
  },
  knobs: {
    strength: { default: 1.0 },
    skinProtection: { default: 0 },
  },
};
