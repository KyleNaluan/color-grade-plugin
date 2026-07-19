/**
 * Non-interactive theme-authoring pipeline CLI.
 *
 * Turns the human `npm run spike` + grade-impact-printout loop
 * (`data/cg-agents-study/report.md` sec 1c) into something a tuner or agent
 * can drive without a human reading terminal output: it renders a candidate
 * Theme against the fixture frame set, emits a machine-readable JSON snapshot
 * of the grade-impact evidence for every frame, and - given a prior round's
 * snapshot as `--baseline` - runs that evidence through the rule-based
 * `gradeGuard` (src/core/analysis/gradeGuard.ts) to accept or reject the
 * round. No model/API calls happen here; this is the deterministic half of
 * the loop. A vision-critique half (naming *why* something looks wrong)
 * arrives with cg-agent-loop-v2 - this tool only ever decides via rules.
 *
 * Usage:
 *   npm run author -- <candidate> [options]
 *
 * <candidate> is one of:
 *   - a name in THEMES (src/themes/index.ts), e.g. teal-orange
 *   - a path to a .ts/.js module with a default export matching Theme
 *   - a path to a .json file matching the Theme shape
 *
 * Options:
 *   --strength <0..1>     override the theme's default strength knob
 *   --profile vlog|rec709 force a footage profile for every frame instead of
 *                         guessing per-frame from the vlog-/rec709- filename
 *                         prefix (the same convention `npm run spike` uses)
 *   --frames-dir <dir>    fixture frame directory (default tests/fixtures/frames)
 *   --frames <a,b,c>      restrict to these filenames instead of the whole dir
 *   --json <path>         write the machine-readable snapshot here
 *                         (default out/theme-author/<candidate-name>.json)
 *   --baseline <path>     a snapshot JSON from a previous round; when given,
 *                         each frame is classified round-over-round via
 *                         gradeGuard and the run exits 1 on any regression
 *   --verbose             print full per-frame stats, not just the summary line
 *
 * Exit codes: 0 = ran clean (no baseline, or baseline given and no
 * regression); 1 = gradeGuard rejected the round (a frame regressed); 2 =
 * usage/load error.
 *
 * Workflow for an agent/tuner iterating on a candidate theme:
 *   1. npm run author -- ./candidate.json --json out/round0.json
 *   2. Edit the candidate (overrides, target stats, knobs) based on the
 *      printed evidence (cast magnitude/direction, skin hue/chroma shift).
 *   3. npm run author -- ./candidate.json --json out/round1.json --baseline out/round0.json
 *   4. Repeat step 2-3, always comparing the new round's --json against the
 *      previous round's, until gradeGuard reports no regressions and the
 *      evidence looks good on the fixtures that matter for this theme's
 *      intended footage family.
 *   5. A numeric accept from gradeGuard is not a taste sign-off (see
 *      report.md sec 1c) - final acceptance still wants a human or a vision
 *      critique step before a candidate ships as a real theme file.
 */
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { basename, join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { PROFILES } from '../src/core/color/index.js';
import { computeStats, decodeToRec709, type FootageStats } from '../src/core/analysis/stats.js';
import { buildTransform } from '../src/core/engine/engine.js';
import { computeGradeImpact, type GradeImpact } from '../src/core/analysis/gradeImpact.js';
import {
  classifyGradeRound,
  type GradeRoundComparison,
  type GradeRoundSignal,
} from '../src/core/analysis/gradeGuard.js';
import type { Theme } from '../src/core/engine/theme.js';
import { THEMES } from '../src/themes/index.js';
import { loadTiff, downsample } from './lib/loadTiff.js';

interface FrameSnapshot {
  frame: string;
  profile: string;
  srcStats: FootageStats;
  impact: GradeImpact;
}

interface Snapshot {
  candidate: string;
  themeName: string;
  generatedAt: string;
  strength?: number;
  framesDir: string;
  frames: FrameSnapshot[];
}

function arg(flag: string): string | undefined {
  const i = process.argv.indexOf(flag);
  return i >= 0 ? process.argv[i + 1] : undefined;
}
const hasFlag = (flag: string): boolean => process.argv.includes(flag);

const USAGE = [
  'usage: npm run author -- <candidate> [--strength 0..1] [--profile vlog|rec709]',
  '                          [--frames-dir dir] [--frames a.tif,b.tif] [--json path]',
  '                          [--baseline path] [--verbose]',
  '',
  '<candidate>: a THEMES key (teal-orange, warm-film, cool-noir, none-manual),',
  '             or a path to a .ts/.js module (default export) or .json Theme file.',
  '',
  'Authoring workflow (see scripts/authorTheme.ts header for the full version):',
  '  1. npm run author -- ./candidate.json --json out/round0.json',
  '  2. Tune the candidate from the printed evidence (cast, skin hue/chroma shift).',
  '  3. npm run author -- ./candidate.json --json out/round1.json --baseline out/round0.json',
  '  4. Repeat until gradeGuard reports no regressions and the evidence looks right',
  '     for the fixtures this theme targets. A numeric accept is not a taste',
  '     sign-off - a human or vision-critique step still gates shipping it.',
].join('\n');

function usageAndExit(message?: string): never {
  if (message) {
    console.error(`error: ${message}\n`);
    console.error(USAGE);
    process.exit(2);
  }
  console.log(USAGE);
  process.exit(0);
}

async function loadCandidate(candidate: string): Promise<Theme> {
  if (THEMES[candidate]) return THEMES[candidate]!;
  const path = resolve(candidate);
  if (!existsSync(path)) usageAndExit(`candidate "${candidate}" is not a known theme name and no file exists at ${path}`);
  if (path.endsWith('.json')) {
    return JSON.parse(readFileSync(path, 'utf8')) as Theme;
  }
  const mod = (await import(pathToFileURL(path).href)) as { default?: Theme } & Record<string, unknown>;
  const theme = mod.default ?? Object.values(mod).find((v) => isThemeShaped(v));
  if (!theme) usageAndExit(`${path} has no default export (or any Theme-shaped export)`);
  return theme as Theme;
}

function isThemeShaped(v: unknown): v is Theme {
  return !!v && typeof v === 'object' && 'targetStats' in v && 'knobs' in v;
}

function resolveFrames(framesDir: string, restrictTo?: string): string[] {
  if (!existsSync(framesDir)) return [];
  const all = readdirSync(framesDir).filter((f) => /\.tiff?$/i.test(f));
  if (!restrictTo) return all;
  const wanted = new Set(restrictTo.split(',').map((s) => s.trim()));
  return all.filter((f) => wanted.has(f));
}

function profileFor(file: string, override?: string): { key: string; name: string } {
  const key = override ?? (file.startsWith('vlog') ? 'vlog' : 'rec709');
  const profile = PROFILES[key];
  if (!profile) usageAndExit(`unknown profile ${key}`);
  return { key, name: profile.name };
}

function toGuardSignal(impact: GradeImpact): GradeRoundSignal {
  return {
    castMagnitude: impact.castMagnitude,
    skinHueShiftDeg: impact.skinHueShiftDeg,
    skinChromaShiftPct: impact.skinChromaShiftPct,
    skinWeightTotal: impact.skinWeightTotal,
  };
}

function printFrameSummary(snap: FrameSnapshot, verbose: boolean): void {
  const f = (x: number) => x.toFixed(2);
  const { impact, srcStats } = snap;
  const castFlag = impact.castMagnitude > 15 ? '  STRONG' : '';
  let line = `  ${snap.frame.padEnd(48)} cast ${f(impact.castMagnitude)}@${f(impact.castDirectionDeg)}deg${castFlag}`;
  if (srcStats.skinPresence > 0.02) {
    const skinFlag = Math.abs(impact.skinHueShiftDeg) > 8 ? '  CHECK-SKIN' : '';
    line += `  | skin hue ${f(impact.skinHueShiftDeg)}deg chroma ${f(impact.skinChromaShiftPct)}%${skinFlag}`;
  } else {
    line += '  | skin n/a';
  }
  console.log(line);
  if (verbose) {
    const p = srcStats.lumaPercentiles;
    console.log(
      `      src luma p5 ${f(p.p5)} p50 ${f(p.p50)} p95 ${f(p.p95)}  lab mean [${f(srcStats.lab.mean[0])},${f(srcStats.lab.mean[1])},${f(srcStats.lab.mean[2])}]  skin ${(srcStats.skinPresence * 100).toFixed(2)}%`,
    );
    const po = impact.outStats.lumaPercentiles;
    console.log(
      `      out luma p5 ${f(po.p5)} p50 ${f(po.p50)} p95 ${f(po.p95)}  lab mean [${f(impact.outStats.lab.mean[0])},${f(impact.outStats.lab.mean[1])},${f(impact.outStats.lab.mean[2])}]  clip lo ${(impact.outStats.clipping.low * 100).toFixed(2)}% hi ${(impact.outStats.clipping.high * 100).toFixed(2)}%`,
    );
  }
}

function printComparison(frame: string, cmp: GradeRoundComparison): void {
  const badge = cmp.verdict === 'regression' ? 'REGRESSION' : cmp.verdict === 'improvement' ? 'improvement' : 'inconclusive';
  console.log(`  ${frame.padEnd(48)} ${badge}  (${cmp.reasons.join('; ')})`);
}

async function main(): Promise<void> {
  if (hasFlag('--help') || hasFlag('-h')) usageAndExit();
  const [candidateArg] = process.argv.slice(2);
  if (!candidateArg) usageAndExit('missing <candidate>');

  const theme = await loadCandidate(candidateArg!);
  const strengthOverride = arg('--strength') ? parseFloat(arg('--strength')!) : undefined;
  const profileOverride = arg('--profile');
  const framesDir = arg('--frames-dir') ?? join('tests', 'fixtures', 'frames');
  const frames = resolveFrames(framesDir, arg('--frames'));
  const verbose = hasFlag('--verbose');

  if (frames.length === 0) {
    console.error(
      `no frames found in ${framesDir} - fixture frames are local-only (gitignored personal footage), ` +
        'see CLAUDE.md. This pipeline cannot run headless without them.',
    );
    process.exit(2);
  }

  console.log(`candidate: ${candidateArg}`);
  console.log(`theme:     ${theme.name}${theme.description ? ` - ${theme.description}` : ''}`);
  console.log(`strength:  ${strengthOverride ?? theme.knobs.strength.default} (default ${theme.knobs.strength.default})`);
  console.log(`frames:    ${frames.length} from ${framesDir}`);
  console.log();

  const frameSnapshots: FrameSnapshot[] = [];
  for (const file of frames) {
    const { key, name } = profileFor(file, profileOverride);
    const profile = PROFILES[key]!;
    const frame = downsample(loadTiff(join(framesDir, file)));
    const decoded = decodeToRec709(frame.pixels, profile);
    const srcStats = computeStats(decoded);
    const transform = buildTransform(srcStats, theme, { strength: strengthOverride });
    const impact = computeGradeImpact(decoded, transform);
    const snap: FrameSnapshot = { frame: file, profile: name, srcStats, impact };
    frameSnapshots.push(snap);
    printFrameSummary(snap, verbose);
  }

  const snapshot: Snapshot = {
    candidate: candidateArg!,
    themeName: theme.name,
    generatedAt: new Date().toISOString(),
    strength: strengthOverride,
    framesDir,
    frames: frameSnapshots,
  };

  const jsonOutPath = arg('--json') ?? join('out', 'theme-author', `${sanitize(theme.name)}.json`);
  mkdirSync(join(jsonOutPath, '..'), { recursive: true });
  writeFileSync(jsonOutPath, JSON.stringify(snapshot, null, 2));
  console.log(`\nwrote snapshot: ${jsonOutPath}`);

  const baselinePath = arg('--baseline');
  if (!baselinePath) {
    process.exit(0);
  }

  if (!existsSync(baselinePath)) usageAndExit(`--baseline file not found: ${baselinePath}`);
  const baseline = JSON.parse(readFileSync(baselinePath, 'utf8')) as Snapshot;

  console.log(`\ngradeGuard: comparing against baseline ${baselinePath}`);
  let anyRegression = false;
  let anyImprovement = false;
  for (const cur of frameSnapshots) {
    const prev = baseline.frames.find((f) => f.frame === cur.frame);
    if (!prev) {
      console.log(`  ${cur.frame.padEnd(48)} (no baseline frame to compare - skipped)`);
      continue;
    }
    const cmp = classifyGradeRound(toGuardSignal(prev.impact), toGuardSignal(cur.impact));
    printComparison(cur.frame, cmp);
    if (cmp.verdict === 'regression') anyRegression = true;
    if (cmp.verdict === 'improvement') anyImprovement = true;
  }

  const overall = anyRegression ? 'regression' : anyImprovement ? 'improvement' : 'inconclusive';
  console.log(`\noverall: ${overall === 'regression' ? 'REJECT' : 'accept'} (${overall})`);
  process.exit(anyRegression ? 1 : 0);
}

function sanitize(name: string): string {
  return name.replace(/[^a-zA-Z0-9._-]/g, '_');
}

main().catch((err) => {
  console.error(err);
  process.exit(2);
});
