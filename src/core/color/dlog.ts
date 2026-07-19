import type { LogProfile } from './types.js';
import { gamutToRec709Matrix, D65, type Chromaticities } from './matrices.js';

/*
 * DJI D-Log / D-Gamut.
 *
 * Curve constants verbatim from DJI's published "White Paper on D-Log and
 * D-Gamut of DJI Cinema Color System". `encode` maps scene-linear reflectance
 * to a normalized D-Log code value; `decode` is its exact algebraic inverse
 * (derived from the same published encode constants rather than the separately
 * rounded published decode constants, so the round-trip is exact). D-Gamut
 * primaries are the colour-science transcription of DJI's published D-Gamut
 * matrix.
 *
 * Published reference code values:
 *   0% -> 0.0929, 18% -> ~0.398658, 90% -> ~0.583393
 */

const LIN_CUT = 0.0078;
const ENCODED_CUT = 6.025 * LIN_CUT + 0.0929; // encode(LIN_CUT)

export function dLogEncode(linear: number): number {
  return linear <= LIN_CUT ? 6.025 * linear + 0.0929 : Math.log10(linear * 0.9892 + 0.0108) * 0.256663 + 0.584555;
}

export function dLogDecode(encoded: number): number {
  return encoded < ENCODED_CUT
    ? (encoded - 0.0929) / 6.025
    : (Math.pow(10, (encoded - 0.584555) / 0.256663) - 0.0108) / 0.9892;
}

// DJI D-Gamut primaries, D65 white.
export const D_GAMUT_CHROMATICITIES: Chromaticities = {
  r: [0.71, 0.31],
  g: [0.21, 0.88],
  b: [0.09, -0.08],
  white: D65,
};

export const DLOG: LogProfile = {
  name: 'D-Log',
  decode: dLogDecode,
  encode: dLogEncode,
  gamutToRec709: gamutToRec709Matrix(D_GAMUT_CHROMATICITIES),
};
