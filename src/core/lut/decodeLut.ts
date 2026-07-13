import type { LogProfile } from '../color/types.js';
import { decodePixelToRec709 } from '../color/decode.js';
import { bakeLut, type Lut3D } from './cube.js';

/**
 * Bake a Decode LUT (log-encoded -> display-referred Rec.709) from a Log
 * profile's `decode()`. `decodePixelToRec709` is the single source of truth
 * for the decode math (shared with analysis/stats.ts); this just samples it
 * into a grid, it does not re-derive anything.
 *
 * Default grid is 65 (not the theme-grade default of 33): near a log
 * profile's highlight rolloff, decode()'s slope grows steeply and, mixed
 * through the gamut matrix and clamped, produces a sharper ridge than a
 * 33-point grid tracks well - the same reason vendor V-Log/S-Log decode LUTs
 * ship at 65 points.
 */
export function bakeDecodeLut(profile: LogProfile, size = 65): Lut3D {
  return bakeLut((rgb) => decodePixelToRec709(rgb, profile), size, `${profile.name} Decode`);
}
