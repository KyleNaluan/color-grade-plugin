import type { Vec3 } from './types.js';
import { mat3MulVec, REC709_TO_XYZ, XYZ_TO_REC709 } from './matrices.js';

// D65 reference white, derived from the same Rec.709 matrix used for RGB->XYZ
// so that RGB white maps exactly to L=100, a=b=0.
const [XN, YN, ZN] = mat3MulVec(REC709_TO_XYZ, [1, 1, 1]);

const EPS = 216 / 24389;
const KAPPA = 24389 / 27;

function fwd(t: number): number {
  return t > EPS ? Math.cbrt(t) : (KAPPA * t + 16) / 116;
}

function inv(f: number): number {
  const f3 = f * f * f;
  return f3 > EPS ? f3 : (116 * f - 16) / KAPPA;
}

export function xyzToLab([x, y, z]: Vec3): Vec3 {
  const fx = fwd(x / XN);
  const fy = fwd(y / YN);
  const fz = fwd(z / ZN);
  return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)];
}

export function labToXyz([l, a, b]: Vec3): Vec3 {
  const fy = (l + 16) / 116;
  const fx = fy + a / 500;
  const fz = fy - b / 200;
  return [inv(fx) * XN, inv(fy) * YN, inv(fz) * ZN];
}

/** Linear Rec.709 RGB -> CIELAB (D65). */
export function linearRec709ToLab(rgb: Vec3): Vec3 {
  return xyzToLab(mat3MulVec(REC709_TO_XYZ, rgb));
}

/** CIELAB (D65) -> linear Rec.709 RGB. */
export function labToLinearRec709(lab: Vec3): Vec3 {
  return mat3MulVec(XYZ_TO_REC709, labToXyz(lab));
}
