import type { LogProfile, Vec3 } from './types.js';
import { mat3MulVec } from './matrices.js';
import { rec709Encode } from './rec709.js';

function clamp01(x: number): number {
  return x < 0 ? 0 : x > 1 ? 1 : x;
}

/**
 * Decode a single encoded RGB pixel through a log profile's `decode()` to
 * display-referred, gamma-encoded Rec.709. The single source of truth for
 * this composition (decode -> gamut -> Rec.709 encode); both bulk footage
 * decoding (analysis/stats.ts) and Decode LUT baking (lut/decodeLut.ts)
 * sample this same function so there is exactly one place the math lives.
 */
export function decodePixelToRec709(rgb: Vec3, profile: LogProfile): Vec3 {
  if (profile.name === 'Rec.709') return rgb;
  const lin = mat3MulVec(profile.gamutToRec709, [
    profile.decode(rgb[0]),
    profile.decode(rgb[1]),
    profile.decode(rgb[2]),
  ]);
  return [
    clamp01(rec709Encode(Math.max(0, lin[0]))),
    clamp01(rec709Encode(Math.max(0, lin[1]))),
    clamp01(rec709Encode(Math.max(0, lin[2]))),
  ];
}
