import type { Theme } from '../core/engine/theme.js';
import { tealOrange } from './teal-orange.js';
import { warmFilm } from './warm-film.js';
import { coolNoir } from './cool-noir.js';
import { noneManual } from './none-manual.js';

export const THEMES: Record<string, Theme> = {
  'teal-orange': tealOrange,
  'warm-film': warmFilm,
  'cool-noir': coolNoir,
  'none-manual': noneManual,
};
