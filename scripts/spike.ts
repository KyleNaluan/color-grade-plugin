/**
 * Engine spike CLI.
 *
 * Usage:
 *   npm run spike -- <frame.tif> <theme> [--profile vlog|rec709] [--strength 0.8] [--out dir]
 *
 * The profile defaults from the filename prefix (vlog-* / rec709-*).
 * Outputs into --out (default out/):
 *   <frame>__<theme>-grade.cube    grade LUT (input: corrected/decoded Rec.709)
 *   vlog-decode.cube               decode LUT (V-Log -> Rec.709), when profile is vlog
 *   <frame>__<theme>-combined.cube convenience LUT (decode + grade in one), vlog only
 * plus a stats printout.
 */
import { mkdirSync, writeFileSync } from 'node:fs';
import { basename, join } from 'node:path';
import { PROFILES } from '../src/core/color/index.js';
import { computeStats, decodeToRec709, type FootageStats } from '../src/core/analysis/stats.js';
import { buildTransform } from '../src/core/engine/engine.js';
import { bakeLut, writeCube } from '../src/core/lut/cube.js';
import { THEMES } from '../src/themes/index.js';
import { loadTiff, downsample } from './lib/loadTiff.js';
import type { Vec3 } from '../src/core/color/types.js';
import { mat3MulVec } from '../src/core/color/matrices.js';
import { rec709Encode } from '../src/core/color/rec709.js';

function arg(flag: string): string | undefined {
  const i = process.argv.indexOf(flag);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

const [framePath, themeName] = process.argv.slice(2);
if (!framePath || !themeName || !THEMES[themeName]) {
  console.error(`usage: npm run spike -- <frame.tif> <theme> [--profile vlog|rec709] [--strength 0..1] [--out dir]`);
  console.error(`themes: ${Object.keys(THEMES).join(', ')}`);
  process.exit(1);
}
const theme = THEMES[themeName]!;
const stem = basename(framePath).replace(/\.tiff?$/i, '');
const profileName = arg('--profile') ?? (stem.startsWith('vlog') ? 'vlog' : 'rec709');
const profile = PROFILES[profileName];
if (!profile) {
  console.error(`unknown profile ${profileName}`);
  process.exit(1);
}
const strength = arg('--strength') ? parseFloat(arg('--strength')!) : undefined;
const outDir = arg('--out') ?? 'out';
mkdirSync(outDir, { recursive: true });

console.log(`frame:   ${framePath}`);
console.log(`profile: ${profile.name}`);
console.log(`theme:   ${theme.name}${strength !== undefined ? ` (strength ${strength})` : ''}`);

const frame = downsample(loadTiff(framePath));
const decoded = decodeToRec709(frame.pixels, profile);
const stats = computeStats(decoded);

printStats('footage stats (post-decode Rec.709)', stats);
printStats('theme target stats', theme.targetStats);

const transform = buildTransform(stats, theme, { strength });

const gradeLut = bakeLut(transform, 33, `${theme.name} grade for ${stem}`);
const gradePath = join(outDir, `${stem}__${theme.name}-grade.cube`);
writeFileSync(gradePath, writeCube(gradeLut));
console.log(`\nwrote ${gradePath}`);

if (profile.name !== 'Rec.709') {
  const decodeTransform = (rgb: Vec3): Vec3 => {
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
  };
  const decodePath = join(outDir, `${profileName}-decode.cube`);
  writeFileSync(decodePath, writeCube(bakeLut(decodeTransform, 33, `${profile.name} -> Rec.709 decode`)));
  console.log(`wrote ${decodePath} (apply BEFORE the grade LUT)`);

  const combined = (rgb: Vec3): Vec3 => transform(decodeTransform(rgb));
  const combinedPath = join(outDir, `${stem}__${theme.name}-combined.cube`);
  writeFileSync(combinedPath, writeCube(bakeLut(combined, 33, `${profile.name} decode + ${theme.name}`)));
  console.log(`wrote ${combinedPath} (single-LUT convenience: decode + grade)`);
}

function clamp01(x: number): number {
  return x < 0 ? 0 : x > 1 ? 1 : x;
}

function printStats(label: string, s: FootageStats): void {
  const f = (x: number) => x.toFixed(3);
  const p = s.lumaPercentiles;
  console.log(`\n${label}`);
  console.log(`  luma pct   p1 ${f(p.p1)}  p5 ${f(p.p5)}  p25 ${f(p.p25)}  p50 ${f(p.p50)}  p75 ${f(p.p75)}  p95 ${f(p.p95)}  p99 ${f(p.p99)}`);
  console.log(`  LAB mean   L ${f(s.lab.mean[0])}  a ${f(s.lab.mean[1])}  b ${f(s.lab.mean[2])}`);
  console.log(`  LAB std    L ${f(s.lab.std[0])}  a ${f(s.lab.std[1])}  b ${f(s.lab.std[2])}`);
  console.log(`  bandChroma shadows ${f(s.bandChroma.shadows)}  mids ${f(s.bandChroma.mids)}  highlights ${f(s.bandChroma.highlights)}`);
  console.log(`  saturation mean ${f(s.saturation.mean)}  std ${f(s.saturation.std)}`);
  console.log(`  skin       ${(s.skinPresence * 100).toFixed(2)}%${s.skinPresence > 0.02 ? '  (protection ACTIVE)' : ''}`);
  console.log(`  clipping   low ${(s.clipping.low * 100).toFixed(2)}%  high ${(s.clipping.high * 100).toFixed(2)}%`);
}
