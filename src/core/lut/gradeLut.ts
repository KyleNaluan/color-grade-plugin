import type { FootageStats } from '../analysis/stats.js';
import type { Theme } from '../engine/theme.js';
import { buildTransform, type EngineOptions } from '../engine/engine.js';
import { bakeLut, type Lut3D } from './cube.js';

/**
 * Bake a grade LUT (corrected/decoded Rec.709 -> graded Rec.709) from measured
 * footage stats and a Theme, via the engine's buildTransform. The Grade stage's
 * single bake entry point and the counterpart to bakeDecodeLut: it samples the
 * pure engine transform into a grid and derives no color math of its own.
 *
 * Domain and range are gamma-encoded Rec.709: the transform assumes the Correct
 * stack (including any Decode LUT) has already run, so callers must feed stats
 * measured on post-Correct (post-decode) pixels.
 *
 * Default grid is 33 (the theme-grade default), not the Decode LUT's 65: a
 * grade transform stays smooth across the cube, unlike a log profile's steep
 * highlight rolloff, so 33 points resolve it well. See bakeDecodeLut's doc
 * comment for the contrasting reason decode LUTs need the finer grid.
 */
export function bakeGradeLut(
  stats: FootageStats,
  theme: Theme,
  opts: EngineOptions = {},
  size = 33,
): Lut3D {
  return bakeLut(buildTransform(stats, theme, opts), size, `${theme.name} grade`);
}
