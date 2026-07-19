import type { LogProfile } from './types.js';
import { gamutToRec709Matrix, D65, type Chromaticities } from './matrices.js';

/*
 * Canon C-Log2 and C-Log3 / Cinema Gamut.
 *
 * Curve constants verbatim from Canon's published "Canon Log Gamma Curves"
 * white paper (the v1.2 normalized-code-value / studio-swing constants, matching
 * the colour-science transcription). Canon's log operates on scene-linear
 * exposure = reflectance / 0.9, so `decode` applies the log inverse then scales
 * by 0.9 to return reflectance (0.18 = 18% grey); `encode` divides by 0.9 first.
 *
 * Published reference code values (normalized [0,1]):
 *   C-Log2: 0% -> 0.0928641, 18% -> ~0.398236, 90% -> ~0.562313
 *   C-Log3: 0% -> 0.1251222, 18% -> ~0.3433894 (Canon's documented mid grey),
 *           90% -> ~0.5644615
 */

/* ------------------------------- C-Log2 -------------------------------- */

export function canonLog2Decode(encoded: number): number {
  const raw =
    encoded < 0.092864125
      ? -(Math.pow(10, (0.092864125 - encoded) / 0.24136077) - 1) / 87.09937546
      : (Math.pow(10, (encoded - 0.092864125) / 0.24136077) - 1) / 87.09937546;
  return raw * 0.9;
}

export function canonLog2Encode(linear: number): number {
  const u = linear / 0.9; // reflectance -> scene-linear exposure factor
  return u < 0
    ? 0.092864125 - 0.24136077 * Math.log10(1 - u * 87.09937546)
    : 0.092864125 + 0.24136077 * Math.log10(1 + u * 87.09937546);
}

/* ------------------------------- C-Log3 -------------------------------- */

export function canonLog3Decode(encoded: number): number {
  let raw: number;
  if (encoded < 0.097465473) {
    raw = -(Math.pow(10, (0.12783901 - encoded) / 0.36726845) - 1) / 14.98325;
  } else if (encoded <= 0.15277891) {
    raw = (encoded - 0.12512219) / 1.9754798;
  } else {
    raw = (Math.pow(10, (encoded - 0.12240537) / 0.36726845) - 1) / 14.98325;
  }
  return raw * 0.9;
}

export function canonLog3Encode(linear: number): number {
  const u = linear / 0.9;
  if (u < -0.014) return 0.12783901 - 0.36726845 * Math.log10(1 - u * 14.98325);
  if (u <= 0.014) return 1.9754798 * u + 0.12512219;
  return 0.12240537 + 0.36726845 * Math.log10(1 + u * 14.98325);
}

// Canon Cinema Gamut primaries, D65 white (published Canon chromaticities).
export const CINEMA_GAMUT_CHROMATICITIES: Chromaticities = {
  r: [0.74, 0.27],
  g: [0.17, 1.14],
  b: [0.08, -0.1],
  white: D65,
};

const CINEMA_GAMUT_TO_REC709 = gamutToRec709Matrix(CINEMA_GAMUT_CHROMATICITIES);

export const CLOG2: LogProfile = {
  name: 'C-Log2',
  decode: canonLog2Decode,
  encode: canonLog2Encode,
  gamutToRec709: CINEMA_GAMUT_TO_REC709,
};

export const CLOG3: LogProfile = {
  name: 'C-Log3',
  decode: canonLog3Decode,
  encode: canonLog3Encode,
  gamutToRec709: CINEMA_GAMUT_TO_REC709,
};
