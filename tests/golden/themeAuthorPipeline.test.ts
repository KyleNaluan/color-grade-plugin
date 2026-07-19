import { describe, it, expect } from 'vitest';
import { existsSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { PROFILES } from '../../src/core/color/index.js';
import { computeStats, decodeToRec709 } from '../../src/core/analysis/stats.js';
import { buildTransform } from '../../src/core/engine/engine.js';
import { computeGradeImpact } from '../../src/core/analysis/gradeImpact.js';
import { THEMES } from '../../src/themes/index.js';
import { loadTiff, downsample } from '../../scripts/lib/loadTiff.js';

const FRAMES_DIR = join(__dirname, '..', 'fixtures', 'frames');
const frames = existsSync(FRAMES_DIR) ? readdirSync(FRAMES_DIR).filter((f) => /\.tiff?$/i.test(f)) : [];

function skinShift(theme: string, frame: string): { hueDeg: number; chromaPct: number } {
  const file = join(FRAMES_DIR, frame);
  const profile = PROFILES[frame.startsWith('vlog') ? 'vlog' : 'rec709']!;
  const decoded = decodeToRec709(downsample(loadTiff(file)).pixels, profile);
  const stats = computeStats(decoded);
  const transform = buildTransform(stats, THEMES[theme]!);
  const impact = computeGradeImpact(decoded, transform);
  return { hueDeg: impact.skinHueShiftDeg, chromaPct: impact.skinChromaShiftPct };
}

/**
 * Regression test for the `npm run author` pipeline (scripts/authorTheme.ts):
 * every shipping theme's file-header doc comment records "after" skin
 * hue/chroma-shift numbers from this exact grade-impact evidence on named
 * fixtures (issue #6 tuning pass). Reproducing them here proves the CLI
 * pipeline computes the same evidence a human tuner would get from
 * `npm run spike`, just non-interactively. Skips cleanly when the (gitignored,
 * local-only) fixture frames are absent - see `tests/golden/frames.test.ts`.
 */
describe.skipIf(frames.length === 0)(
  'theme-author pipeline reproduces shipping theme header numbers (skipped: tests/fixtures/frames/ is empty)',
  () => {
    const cases: Array<[theme: string, frame: string, hueDeg: number, chromaPct: number]> = [
      // src/themes/teal-orange.ts header: vlog-tricky-skin 23.8%/-7.5deg (after)
      ['teal-orange', 'vlog-tricky-skin.tif', -7.5, 23.8],
      // src/themes/teal-orange.ts header: vlog-portait-cool-other-people-in-background.tif 20.4%/-2.2deg (after)
      ['teal-orange', 'vlog-portait-cool-other-people-in-background.tif', -2.2, 20.4],
      // src/themes/warm-film.ts header: vlog-tricky-skin 7.3%/-1.0deg (after)
      ['warm-film', 'vlog-tricky-skin.tif', -1.0, 7.3],
      // src/themes/warm-film.ts header: vlog-portait-cool-other-people-in-background.tif 9.3%/1.7deg (after)
      ['warm-film', 'vlog-portait-cool-other-people-in-background.tif', 1.7, 9.3],
      // src/themes/cool-noir.ts header: vlog-tricky-skin -23.0%/-4.1deg (after)
      ['cool-noir', 'vlog-tricky-skin.tif', -4.1, -23.0],
      // src/themes/cool-noir.ts header: vlog-portait-cool-other-people-in-background.tif -11.1%/-3.2deg (after)
      ['cool-noir', 'vlog-portait-cool-other-people-in-background.tif', -3.2, -11.1],
    ];

    it.each(cases)('%s on %s: hue %sdeg chroma %s%%', (theme, frame, hueDeg, chromaPct) => {
      if (!frames.includes(frame)) return;
      const shift = skinShift(theme, frame);
      // The header numbers are rounded to 1 decimal; allow a small tolerance
      // for float/tooling drift rather than requiring bit-exact reproduction.
      expect(shift.hueDeg).toBeCloseTo(hueDeg, 0);
      expect(shift.chromaPct).toBeCloseTo(chromaPct, 0);
    });
  },
);
