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
