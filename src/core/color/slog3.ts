import type { LogProfile } from './types.js';
import { gamutToRec709Matrix, D65, type Chromaticities } from './matrices.js';

/*
 * Sony S-Log3 / S-Gamut3.Cine.
 *
 * Curve constants verbatim from Sony's "Technical Summary for S-Gamut3.Cine/
 * S-Log3 and S-Gamut3/S-Log3" (the published S-Log3 transfer function). `decode`
 * maps a normalized S-Log3 code value [0,1] to scene-linear reflectance (0.18 =
 * 18% grey); `encode` is its inverse. Published reference code values:
 *   0% reflectance -> 95/1023  = 0.0928641
 *   18% grey       -> 420/1023 = 0.4105572
 *   90% white      -> ~0.584419
 */

const BREAK_ENCODED = 171.2102946929 / 1023; // encoded-domain linear/log breakpoint

export function slog3Encode(linear: number): number {
  return linear >= 0.01125 // linear-domain breakpoint
    ? (420 + Math.log10((linear + 0.01) / (0.18 + 0.01)) * 261.5) / 1023
    : (linear * (171.2102946929 - 95) / 0.01125 + 95) / 1023;
}

export function slog3Decode(encoded: number): number {
  return encoded >= BREAK_ENCODED
    ? Math.pow(10, (encoded * 1023 - 420) / 261.5) * (0.18 + 0.01) - 0.01
    : (encoded * 1023 - 95) * 0.01125 / (171.2102946929 - 95);
}

// S-Gamut3.Cine primaries, D65 white (published Sony chromaticities).
export const SGAMUT3_CINE_CHROMATICITIES: Chromaticities = {
  r: [0.766, 0.275],
  g: [0.225, 0.8],
  b: [0.089, -0.087],
  white: D65,
};

export const SLOG3: LogProfile = {
  name: 'S-Log3',
  decode: slog3Decode,
  encode: slog3Encode,
  gamutToRec709: gamutToRec709Matrix(SGAMUT3_CINE_CHROMATICITIES),
};
