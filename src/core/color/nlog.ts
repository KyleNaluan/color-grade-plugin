import type { LogProfile } from './types.js';
import { gamutToRec709Matrix, BT2020_CHROMATICITIES } from './matrices.js';

/*
 * Nikon N-Log / N-Gamut.
 *
 * Curve constants verbatim from Nikon's published "N-Log Specification
 * Document" (10-bit code values normalized to [0,1]). N-Log is a hybrid: a cube
 * root below mid-tones and a natural log above. `encode` maps scene-linear
 * reflectance to a normalized code value; `decode` (baked into the Decode LUT)
 * is its inverse. N-Gamut is defined as ITU-R BT.2020.
 *
 * Published reference code values:
 *   0% -> ~0.124381, 18% -> ~0.363674, 90% -> ~0.589634
 */

const A = 650 / 1023;
const C = 150 / 1023;
const D = 619 / 1023;
const LIN_CUT = 0.328;
const ENCODED_CUT = 452 / 1023;

export function nLogEncode(linear: number): number {
  return linear < LIN_CUT ? A * Math.cbrt(linear + 0.0075) : C * Math.log(linear) + D;
}

export function nLogDecode(encoded: number): number {
  return encoded < ENCODED_CUT ? Math.pow(encoded / A, 3) - 0.0075 : Math.exp((encoded - D) / C);
}

// N-Gamut = ITU-R BT.2020.
export const NLOG: LogProfile = {
  name: 'N-Log',
  decode: nLogDecode,
  encode: nLogEncode,
  gamutToRec709: gamutToRec709Matrix(BT2020_CHROMATICITIES),
};
