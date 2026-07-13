export type Vec3 = [number, number, number];
export type Mat3 = [Vec3, Vec3, Vec3];

/**
 * A pluggable camera log profile.
 * `decode` maps a single encoded channel value to scene-linear reflectance.
 * `gamutToRec709` converts scene-linear camera-gamut RGB to scene-linear Rec.709 RGB.
 */
export interface LogProfile {
  name: string;
  decode(encoded: number): number;
  encode(linear: number): number;
  gamutToRec709: Mat3;
}
