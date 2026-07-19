export * from './types.js';
export * from './matrices.js';
export * from './lab.js';
export * from './vlog.js';
export * from './rec709.js';
export * from './slog3.js';
export * from './canonlog.js';
export * from './arrilogc.js';
export * from './dlog.js';
export * from './filmgen5.js';
export * from './flog.js';
export * from './nlog.js';
export * from './decode.js';

import type { LogProfile } from './types.js';
import { VLOG } from './vlog.js';
import { REC709 } from './rec709.js';
import { SLOG3 } from './slog3.js';
import { CLOG2, CLOG3 } from './canonlog.js';
import { LOGC3, LOGC4 } from './arrilogc.js';
import { DLOG } from './dlog.js';
import { FILM_GEN5 } from './filmgen5.js';
import { FLOG, FLOG2 } from './flog.js';
import { NLOG } from './nlog.js';

export const PROFILES: Record<string, LogProfile> = {
  rec709: REC709,
  vlog: VLOG,
  'sony-slog3': SLOG3,
  'canon-clog2': CLOG2,
  'canon-clog3': CLOG3,
  'arri-logc3': LOGC3,
  'arri-logc4': LOGC4,
  'dji-dlog': DLOG,
  'bmd-filmgen5': FILM_GEN5,
  'fuji-flog': FLOG,
  'fuji-flog2': FLOG2,
  'nikon-nlog': NLOG,
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

/**
 * One entry in the canonical footage-profile catalog: an ordered, UI-facing view
 * of `PROFILES`. This is the oracle the native side mirrors (see native's
 * `core/FootageCatalog.h`), so the AE Effect-Controls flat popup and the editor
 * window's Camera->Profile cascade stay in sync with a single ordering.
 *
 * `index` is the 1-based AE popup position, `key` the `PROFILES` key, `camera`
 * the maker name for the editor cascade grouping (empty for Standard), and
 * `flatLabel` the AE popup label ("Sony - S-Log3"). Ordering (captain-approved,
 * `data/cg-camera-profiles-decision.md`): Standard first, then cameras
 * alphabetical. Free to reorder pre-release (append-only compat NOT required for
 * this param).
 */
export interface FootageProfileEntry {
  index: number;
  key: string;
  camera: string;
  profileLabel: string;
  flatLabel: string;
}

export const FOOTAGE_PROFILES: readonly FootageProfileEntry[] = [
  { index: 1, key: 'rec709', camera: '', profileLabel: 'Standard (Rec.709)', flatLabel: 'Standard (Rec.709)' },
  { index: 2, key: 'arri-logc3', camera: 'ARRI', profileLabel: 'LogC3 (EI800)', flatLabel: 'ARRI - LogC3 (EI800)' },
  { index: 3, key: 'arri-logc4', camera: 'ARRI', profileLabel: 'LogC4', flatLabel: 'ARRI - LogC4' },
  { index: 4, key: 'bmd-filmgen5', camera: 'Blackmagic', profileLabel: 'Film Gen5', flatLabel: 'Blackmagic - Film Gen5' },
  { index: 5, key: 'canon-clog2', camera: 'Canon', profileLabel: 'C-Log2', flatLabel: 'Canon - C-Log2' },
  { index: 6, key: 'canon-clog3', camera: 'Canon', profileLabel: 'C-Log3', flatLabel: 'Canon - C-Log3' },
  { index: 7, key: 'dji-dlog', camera: 'DJI', profileLabel: 'D-Log', flatLabel: 'DJI - D-Log' },
  { index: 8, key: 'fuji-flog', camera: 'Fujifilm', profileLabel: 'F-Log', flatLabel: 'Fujifilm - F-Log' },
  { index: 9, key: 'fuji-flog2', camera: 'Fujifilm', profileLabel: 'F-Log2', flatLabel: 'Fujifilm - F-Log2' },
  { index: 10, key: 'nikon-nlog', camera: 'Nikon', profileLabel: 'N-Log', flatLabel: 'Nikon - N-Log' },
  { index: 11, key: 'vlog', camera: 'Panasonic', profileLabel: 'V-Log', flatLabel: 'Panasonic - V-Log' },
  { index: 12, key: 'sony-slog3', camera: 'Sony', profileLabel: 'S-Log3', flatLabel: 'Sony - S-Log3' },
];
