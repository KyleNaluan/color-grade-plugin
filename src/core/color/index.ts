export * from './types.js';
export * from './matrices.js';
export * from './lab.js';
export * from './vlog.js';
export * from './rec709.js';
export * from './decode.js';

import type { LogProfile } from './types.js';
import { VLOG } from './vlog.js';
import { REC709 } from './rec709.js';

export const PROFILES: Record<string, LogProfile> = {
  vlog: VLOG,
  rec709: REC709,
};

/**
 * The key of the standard footage profile - footage already display-referred
 * Rec.709, which needs no Correct Decode LUT. Every other key in `PROFILES` is
 * a log format that must be decoded to Rec.709 by a baked Decode LUT.
 */
export const STANDARD_PROFILE_KEY = 'rec709';

/**
 * Whether a footage-profile key denotes log footage (i.e. the Correct stage
 * must bake and apply that profile's Decode LUT). Standard/Rec.709 footage is
 * the sole exception. Keyed off `STANDARD_PROFILE_KEY` so adding more log
 * profiles to `PROFILES` needs no change here.
 */
export function isLogProfile(key: string): boolean {
  return key !== STANDARD_PROFILE_KEY;
}
