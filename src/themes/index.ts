import type { Theme } from '../core/engine/theme.js';
import { tealOrange } from './teal-orange.js';
import { warmFilm } from './warm-film.js';
import { coolNoir } from './cool-noir.js';
import { noneManual } from './none-manual.js';
import { goldenHour } from './golden-hour.js';
import { bleachBypass } from './bleach-bypass.js';
import { vintageFade } from './vintage-fade.js';
import { highKeyClean } from './high-key-clean.js';
import { lowKeyMoody } from './low-key-moody.js';
import { winterBlue } from './winter-blue.js';
import { warmPortrait } from './warm-portrait.js';
import { pastelDream } from './pastel-dream.js';
import { neonCyberpunk } from './neon-cyberpunk.js';
import { dayForNight } from './day-for-night.js';
import { autumn } from './autumn.js';
import { summerBlockbuster } from './summer-blockbuster.js';
import { mutedTealOrange } from './muted-teal-orange.js';
import { monochromeBw } from './monochrome-bw.js';
import { sepia } from './sepia.js';
import { cinematicGreen } from './cinematic-green.js';
import { desaturatedDoc } from './desaturated-doc.js';
import { punchySocial } from './punchy-social.js';
import { crossProcess } from './cross-process.js';
import { roseRomance } from './rose-romance.js';

export const THEMES: Record<string, Theme> = {
  // Original shipping set (issue #6).
  'teal-orange': tealOrange,
  'warm-film': warmFilm,
  'cool-noir': coolNoir,
  'none-manual': noneManual,
  // Expanded curated library (fm/cg-theme-library). Authored via the
  // `npm run author` gradeGuard pipeline; evidence is in each theme's header.
  'golden-hour': goldenHour,
  'bleach-bypass': bleachBypass,
  'vintage-fade': vintageFade,
  'high-key-clean': highKeyClean,
  'low-key-moody': lowKeyMoody,
  'winter-blue': winterBlue,
  'warm-portrait': warmPortrait,
  'pastel-dream': pastelDream,
  'neon-cyberpunk': neonCyberpunk,
  'day-for-night': dayForNight,
  autumn: autumn,
  'summer-blockbuster': summerBlockbuster,
  'muted-teal-orange': mutedTealOrange,
  'monochrome-bw': monochromeBw,
  sepia: sepia,
  'cinematic-green': cinematicGreen,
  'desaturated-doc': desaturatedDoc,
  'punchy-social': punchySocial,
  'cross-process': crossProcess,
  'rose-romance': roseRomance,
};
