/**
 * Cross-clip grade consistency CLI.
 *
 * Usage:
 *   npm run check-consistency -- <theme> <frameA.tif> <frameB.tif> [more frames...] \
 *     [--profile vlog|rec709] [--strength 0.8] [--scene name] [--json]
 *
 * Grades each input frame toward <theme> exactly like `npm run spike`, then
 * runs the pairwise cross-clip comparator
 * (`src/core/analysis/crossClipConsistency.ts`) over the resulting
 * `GradeImpact`s and prints the consistency report - a readable summary by
 * default, or `--json` for a machine-consumable `SceneConsistencyReport`.
 *
 * Each frame's profile defaults from its filename prefix (vlog-* / rec709-*),
 * same as `spike.ts`; `--profile` overrides it for every frame in this run.
 */
import { basename } from 'node:path';
import { PROFILES } from '../src/core/color/index.js';
import { computeStats, decodeToRec709 } from '../src/core/analysis/stats.js';
import { computeGradeImpact } from '../src/core/analysis/gradeImpact.js';
import { buildTransform } from '../src/core/engine/engine.js';
import { THEMES } from '../src/themes/index.js';
import { loadTiff, downsample } from './lib/loadTiff.js';
import {
  checkSceneConsistency,
  formatSceneConsistencyReport,
  type ClipGradeSummary,
} from '../src/core/analysis/crossClipConsistency.js';

function arg(flag: string): string | undefined {
  const i = process.argv.indexOf(flag);
  return i >= 0 ? process.argv[i + 1] : undefined;
}
function flag(name: string): boolean {
  return process.argv.includes(name);
}

// Positional args (theme + frame paths) must precede any --flags.
const argv = process.argv.slice(2);
const positional: string[] = [];
for (const a of argv) {
  if (a.startsWith('--')) break;
  positional.push(a);
}
const [themeName, ...framePaths] = positional;

if (!themeName || !THEMES[themeName] || framePaths.length < 2) {
  console.error(
    `usage: npm run check-consistency -- <theme> <frameA.tif> <frameB.tif> [more...] [--profile vlog|rec709] [--strength 0..1] [--scene name] [--json]`,
  );
  console.error(`themes: ${Object.keys(THEMES).join(', ')}`);
  process.exit(1);
}
const theme = THEMES[themeName]!;
const profileOverride = arg('--profile');
const strength = arg('--strength') ? parseFloat(arg('--strength')!) : undefined;
const sceneId = arg('--scene');
const asJson = flag('--json');

const clips: ClipGradeSummary[] = framePaths.map((framePath) => {
  const stem = basename(framePath).replace(/\.tiff?$/i, '');
  const profileName = profileOverride ?? (stem.startsWith('vlog') ? 'vlog' : 'rec709');
  const profile = PROFILES[profileName];
  if (!profile) {
    console.error(`unknown profile ${profileName} for ${framePath}`);
    process.exit(1);
  }
  const frame = downsample(loadTiff(framePath));
  const decoded = decodeToRec709(frame.pixels, profile);
  const stats = computeStats(decoded);
  const transform = buildTransform(stats, theme, { strength });
  return { clipId: stem, impact: computeGradeImpact(decoded, transform) };
});

const report = checkSceneConsistency(clips, undefined, sceneId);

if (asJson) {
  console.log(JSON.stringify(report, null, 2));
} else {
  console.log(`theme: ${theme.name}${strength !== undefined ? ` (strength ${strength})` : ''}`);
  console.log(formatSceneConsistencyReport(report));
}

process.exitCode = report.diverged.length > 0 ? 1 : 0;
