import type { LogProfile } from './types.js';
import { gamutToRec709Matrix, BT2020_CHROMATICITIES } from './matrices.js';

/*
 * Fujifilm F-Log and F-Log2 / F-Gamut.
 *
 * Curve constants verbatim from Fujifilm's published "F-Log Data Sheet" and
 * "F-Log2 Data Sheet". `encode` maps scene-linear reflectance to a normalized
 * code value; `decode` (baked into the Decode LUT) is its inverse. F-Gamut is
 * defined as ITU-R BT.2020, so both profiles reuse the BT.2020 primaries.
 *
 * Published reference code values:
 *   F-Log:  0% -> 0.092864, 18% -> ~0.459324, 90% -> ~0.689526
 *   F-Log2: 0% -> 0.092864, 18% -> ~0.391006, 90% -> ~0.557131
 */

interface FLogConstants {
  cut1: number;
  cut2: number;
  a: number;
  b: number;
  c: number;
  d: number;
  e: number;
  f: number;
}

const F_LOG: FLogConstants = {
  cut1: 0.00089,
  cut2: 0.100537775223865,
  a: 0.555556,
  b: 0.009468,
  c: 0.344676,
  d: 0.790453,
  e: 8.735631,
  f: 0.092864,
};

const F_LOG2: FLogConstants = {
  cut1: 0.000889,
  cut2: 0.100686685370811,
  a: 5.555556,
  b: 0.064829,
  c: 0.245281,
  d: 0.384316,
  e: 8.799461,
  f: 0.092864,
};

function makeEncode(k: FLogConstants): (linear: number) => number {
  return (linear) => (linear < k.cut1 ? k.e * linear + k.f : k.c * Math.log10(k.a * linear + k.b) + k.d);
}

function makeDecode(k: FLogConstants): (encoded: number) => number {
  return (encoded) => (encoded < k.cut2 ? (encoded - k.f) / k.e : Math.pow(10, (encoded - k.d) / k.c) / k.a - k.b / k.a);
}

// F-Gamut = ITU-R BT.2020.
const F_GAMUT_TO_REC709 = gamutToRec709Matrix(BT2020_CHROMATICITIES);

export const flogEncode = makeEncode(F_LOG);
export const flogDecode = makeDecode(F_LOG);
export const flog2Encode = makeEncode(F_LOG2);
export const flog2Decode = makeDecode(F_LOG2);

export const FLOG: LogProfile = {
  name: 'F-Log',
  decode: flogDecode,
  encode: flogEncode,
  gamutToRec709: F_GAMUT_TO_REC709,
};

export const FLOG2: LogProfile = {
  name: 'F-Log2',
  decode: flog2Decode,
  encode: flog2Encode,
  gamutToRec709: F_GAMUT_TO_REC709,
};
