import type { LogProfile } from './types.js';
import { mat3Mul, rgbToXyzMatrix, mat3Inv, REC709_TO_XYZ, D65, type Chromaticities } from './matrices.js';

// Constants from the published Panasonic "V-Log/V-Gamut Reference Manual".
const CUT1 = 0.01; // linear-domain breakpoint
const CUT2 = 0.181; // encoded-domain breakpoint (= encode(CUT1))
const B = 0.00873;
const C = 0.241514;
const D = 0.598206;

export function vlogEncode(linear: number): number {
  return linear < CUT1 ? 5.6 * linear + 0.125 : C * Math.log10(linear + B) + D;
}

export function vlogDecode(encoded: number): number {
  return encoded < CUT2 ? (encoded - 0.125) / 5.6 : Math.pow(10, (encoded - D) / C) - B;
}

// V-Gamut primaries from the same manual, D65 white.
export const VGAMUT_CHROMATICITIES: Chromaticities = {
  r: [0.73, 0.28],
  g: [0.165, 0.84],
  b: [0.1, -0.03],
  white: D65,
};

const VGAMUT_TO_XYZ = rgbToXyzMatrix(VGAMUT_CHROMATICITIES);

export const VLOG: LogProfile = {
  name: 'V-Log',
  decode: vlogDecode,
  encode: vlogEncode,
  gamutToRec709: mat3Mul(mat3Inv(REC709_TO_XYZ), VGAMUT_TO_XYZ),
};
