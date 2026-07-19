import type { LogProfile } from './types.js';
import { gamutToRec709Matrix, D65, type Chromaticities } from './matrices.js';

/*
 * ARRI LogC3 (EI 800) / ARRI Wide Gamut 3, and LogC4 / ARRI Wide Gamut 4.
 *
 * LogC3 constants are ARRI's published EI 800 reflectance-normalized parameters
 * (the "ALEXA LogC Curve" white paper form where 18% grey encodes to ~0.391).
 * LogC4 constants are ARRI's published "LogC4 Logarithmic Color Space
 * Specification". `decode` maps a normalized code value [0,1] to scene-linear
 * reflectance (0.18 = 18% grey); `encode` is its inverse.
 *
 * Published reference code values:
 *   LogC3(EI800): 0% -> 0.092809, 18% -> ~0.391007, 90% -> ~0.559430
 *   LogC4:        0% -> 0.0928641, 18% -> ~0.278403, 90% -> ~0.417874
 */

/* --------------------------- LogC3 (EI 800) ---------------------------- */

const C3_CUT = 0.010591;
const C3_A = 5.555556;
const C3_B = 0.052272;
const C3_C = 0.24719;
const C3_D = 0.385537;
const C3_E = 5.367655;
const C3_F = 0.092809;
const C3_ENCODED_CUT = C3_E * C3_CUT + C3_F; // encode(cut), the decode-side breakpoint

export function logC3Encode(linear: number): number {
  return linear > C3_CUT ? C3_C * Math.log10(C3_A * linear + C3_B) + C3_D : C3_E * linear + C3_F;
}

export function logC3Decode(encoded: number): number {
  return encoded > C3_ENCODED_CUT
    ? (Math.pow(10, (encoded - C3_D) / C3_C) - C3_B) / C3_A
    : (encoded - C3_F) / C3_E;
}

// ARRI Wide Gamut 3 primaries, D65 white.
export const AWG3_CHROMATICITIES: Chromaticities = {
  r: [0.684, 0.313],
  g: [0.221, 0.848],
  b: [0.0861, -0.102],
  white: D65,
};

export const LOGC3: LogProfile = {
  name: 'LogC3',
  decode: logC3Decode,
  encode: logC3Encode,
  gamutToRec709: gamutToRec709Matrix(AWG3_CHROMATICITIES),
};

/* -------------------------------- LogC4 -------------------------------- */

const C4_A = (Math.pow(2, 18) - 16) / 117.45;
const C4_B = (1023 - 95) / 1023;
const C4_C = 95 / 1023;
const C4_S = (7 * Math.LN2 * Math.pow(2, 7 - (14 * C4_C) / C4_B)) / (C4_A * C4_B);
const C4_T = (Math.pow(2, (14 * -C4_C) / C4_B + 6) - 64) / C4_A;

export function logC4Encode(linear: number): number {
  return linear >= C4_T
    ? ((Math.log2(C4_A * linear + 64) - 6) / 14) * C4_B + C4_C
    : (linear - C4_T) / C4_S;
}

export function logC4Decode(encoded: number): number {
  return encoded >= 0
    ? (Math.pow(2, (14 * (encoded - C4_C)) / C4_B + 6) - 64) / C4_A
    : encoded * C4_S + C4_T;
}

// ARRI Wide Gamut 4 primaries, D65 white.
export const AWG4_CHROMATICITIES: Chromaticities = {
  r: [0.7347, 0.2653],
  g: [0.1424, 0.8576],
  b: [0.0991, -0.0308],
  white: D65,
};

export const LOGC4: LogProfile = {
  name: 'LogC4',
  decode: logC4Decode,
  encode: logC4Encode,
  gamutToRec709: gamutToRec709Matrix(AWG4_CHROMATICITIES),
};
