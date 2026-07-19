import type { LogProfile } from './types.js';
import { gamutToRec709Matrix, D65, type Chromaticities } from './matrices.js';

/*
 * Blackmagic Film Generation 5 / Blackmagic Wide Gamut (Gen 5).
 *
 * Curve constants verbatim from Blackmagic Design's "Blackmagic Generation 5
 * Color Science" white paper (as transcribed by colour-science). The log region
 * uses a natural log. `encode` maps scene-linear reflectance to a normalized
 * code value; `decode` is its inverse and is what the Decode LUT bakes.
 *
 * Published reference code values:
 *   0% -> ~0.0924658, 18% -> ~0.383546, 90% -> ~0.521382
 */

const A = 0.08692876065491224;
const B = 0.005494072432257808;
const C = 0.5300133392291939;
const D = 8.283605932402494;
const E = 0.09246575342465753;
const LIN_CUT = 0.005;
const ENCODED_CUT = D * LIN_CUT + E; // encode(LIN_CUT)

export function filmGen5Encode(linear: number): number {
  return linear < LIN_CUT ? D * linear + E : A * Math.log(linear + B) + C;
}

export function filmGen5Decode(encoded: number): number {
  return encoded < ENCODED_CUT ? (encoded - E) / D : Math.exp((encoded - C) / A) - B;
}

// Blackmagic Wide Gamut (Gen 5) primaries, D65 white.
export const BMD_WIDE_GAMUT_GEN5_CHROMATICITIES: Chromaticities = {
  r: [0.7177215, 0.3171181],
  g: [0.228041, 0.861569],
  b: [0.1005841, -0.0820452],
  white: D65,
};

export const FILM_GEN5: LogProfile = {
  name: 'Blackmagic Film Gen5',
  decode: filmGen5Decode,
  encode: filmGen5Encode,
  gamutToRec709: gamutToRec709Matrix(BMD_WIDE_GAMUT_GEN5_CHROMATICITIES),
};
