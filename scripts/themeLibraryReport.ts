/**
 * Batch theme-library health report.
 *
 * Reuses the exact same core seam as `npm run author` (computeStats ->
 * buildTransform -> computeGradeImpact) but runs every registered theme in one
 * process, so a whole-library sanity scan doesn't pay tsx startup once per theme.
 * Prints a compact per-theme summary: worst cast (excluding the known tungsten
 * outlier vlog-indoor-flat-tricky-skin), count of STRONG (cast>15) frames, and
 * the worst real-skin (>5%) hue/chroma shift - a quick "did I break skin
 * protection?" scan.
 *
 * Usage: npm run theme:health
 * The authoritative accept/reject gate is still `npm run author --baseline`; this
 * is only the fast companion scan. Needs the local-only fixture frames.
 */
import { readdirSync } from 'node:fs';
import { join } from 'node:path';
import { PROFILES } from '../src/core/color/index.js';
import { computeStats, decodeToRec709 } from '../src/core/analysis/stats.js';
import { buildTransform } from '../src/core/engine/engine.js';
import { computeGradeImpact } from '../src/core/analysis/gradeImpact.js';
import { THEMES } from '../src/themes/index.js';
import { loadTiff, downsample } from './lib/loadTiff.js';

const FRAMES_DIR = join('tests', 'fixtures', 'frames');
// The uncorrected-tungsten outlier that drives outsized cast on every theme
// (documented in CLAUDE.md / teal-orange.ts). Excluded from the "worst cast".
const CAST_OUTLIER = 'vlog-indoor-flat-tricky-skin.tif';
// Frames with meaningful real skin presence (>5%), used to judge skin protection.
const REAL_SKIN = new Set([
  'vlog-cave-warm.tif',
  'vlog-tricky-skin.tif',
  'vlog-portait-cool-other-people-in-background.tif',
  'vlog-outdoor-bright.tif',
  'vlog-lowlight.tif',
  'rec709-outdoor.tif',
]);

const THEME_NAMES = Object.keys(THEMES);

function profileFor(file: string) {
  const key = file.startsWith('vlog') ? 'vlog' : 'rec709';
  return PROFILES[key]!;
}

interface Cached { file: string; decoded: Float32Array; srcStats: ReturnType<typeof computeStats>; }

function loadFrames(): Cached[] {
  const files = readdirSync(FRAMES_DIR).filter((f) => /\.tiff?$/i.test(f)).sort();
  return files.map((file) => {
    const frame = downsample(loadTiff(join(FRAMES_DIR, file)));
    const decoded = decodeToRec709(frame.pixels, profileFor(file));
    return { file, decoded, srcStats: computeStats(decoded) };
  });
}

function fmt(x: number, d = 1): string { return x.toFixed(d); }

function report(frames: Cached[]): void {
  console.log('theme'.padEnd(20), 'worstCast', 'STRONG', 'worstSkinHue', 'worstSkinChroma%');
  for (const name of THEME_NAMES) {
    const theme = THEMES[name]!;
    let worstCast = 0, strong = 0, worstHue = 0, worstChroma = 0;
    for (const c of frames) {
      const transform = buildTransform(c.srcStats, theme);
      const impact = computeGradeImpact(c.decoded, transform);
      if (c.file !== CAST_OUTLIER && impact.castMagnitude > worstCast) worstCast = impact.castMagnitude;
      if (impact.castMagnitude > 15) strong++;
      if (REAL_SKIN.has(c.file) && c.srcStats.skinPresence > 0.05) {
        if (Math.abs(impact.skinHueShiftDeg) > Math.abs(worstHue)) worstHue = impact.skinHueShiftDeg;
        if (Math.abs(impact.skinChromaShiftPct) > Math.abs(worstChroma)) worstChroma = impact.skinChromaShiftPct;
      }
    }
    console.log(
      name.padEnd(20),
      fmt(worstCast).padStart(9),
      String(strong).padStart(6),
      fmt(worstHue).padStart(12),
      fmt(worstChroma).padStart(16),
    );
  }
}

report(loadFrames());
