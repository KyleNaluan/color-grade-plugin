import type { Mat3, Vec3 } from './types.js';

export function mat3MulVec(m: Mat3, v: Vec3): Vec3 {
  return [
    m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
    m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
    m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2],
  ];
}

export function mat3Mul(a: Mat3, b: Mat3): Mat3 {
  const r: number[][] = [[], [], []];
  for (let i = 0; i < 3; i++)
    for (let j = 0; j < 3; j++)
      r[i]![j] = a[i]![0]! * b[0][j]! + a[i]![1]! * b[1][j]! + a[i]![2]! * b[2][j]!;
  return r as Mat3;
}

export function mat3Inv(m: Mat3): Mat3 {
  const [a, b, c] = m[0];
  const [d, e, f] = m[1];
  const [g, h, i] = m[2];
  const A = e * i - f * h;
  const B = -(d * i - f * g);
  const C = d * h - e * g;
  const det = a * A + b * B + c * C;
  if (Math.abs(det) < 1e-12) throw new Error('singular matrix');
  const s = 1 / det;
  return [
    [A * s, -(b * i - c * h) * s, (b * f - c * e) * s],
    [B * s, (a * i - c * g) * s, -(a * f - c * d) * s],
    [C * s, -(a * h - b * g) * s, (a * e - b * d) * s],
  ];
}

export interface Chromaticities {
  r: [number, number];
  g: [number, number];
  b: [number, number];
  white: [number, number];
}

/** RGB (linear) -> XYZ matrix from primaries + white point. */
export function rgbToXyzMatrix(c: Chromaticities): Mat3 {
  const xyzOf = ([x, y]: [number, number]): Vec3 => [x / y, 1, (1 - x - y) / y];
  const P: Mat3 = [
    [xyzOf(c.r)[0], xyzOf(c.g)[0], xyzOf(c.b)[0]],
    [xyzOf(c.r)[1], xyzOf(c.g)[1], xyzOf(c.b)[1]],
    [xyzOf(c.r)[2], xyzOf(c.g)[2], xyzOf(c.b)[2]],
  ];
  const w = xyzOf(c.white);
  const s = mat3MulVec(mat3Inv(P), w);
  return [
    [P[0][0] * s[0], P[0][1] * s[1], P[0][2] * s[2]],
    [P[1][0] * s[0], P[1][1] * s[1], P[1][2] * s[2]],
    [P[2][0] * s[0], P[2][1] * s[1], P[2][2] * s[2]],
  ];
}

export const D65: [number, number] = [0.3127, 0.329];

export const REC709_CHROMATICITIES: Chromaticities = {
  r: [0.64, 0.33],
  g: [0.3, 0.6],
  b: [0.15, 0.06],
  white: D65,
};

export const REC709_TO_XYZ = rgbToXyzMatrix(REC709_CHROMATICITIES);
export const XYZ_TO_REC709 = mat3Inv(REC709_TO_XYZ);
