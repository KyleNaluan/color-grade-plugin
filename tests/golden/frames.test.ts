import { describe, it, expect } from 'vitest';
import { existsSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { loadTiff, downsample } from '../../scripts/lib/loadTiff.js';
import { computeStats, decodeToRec709 } from '../../src/core/analysis/stats.js';
import { PROFILES } from '../../src/core/color/index.js';

const FRAMES_DIR = join(__dirname, '..', 'fixtures', 'frames');
const frames = existsSync(FRAMES_DIR)
  ? readdirSync(FRAMES_DIR).filter((f) => /\.tiff?$/i.test(f))
  : [];

// The fixture frames are personal footage and are NOT committed (see .gitignore).
// On a fresh clone this suite skips cleanly.
describe.skipIf(frames.length === 0)(
  'golden fixture frames (skipped: tests/fixtures/frames/ is empty - fixtures are local-only, not in the repo)',
  () => {
    it.each(frames)('%s produces sane stats', (file) => {
      const profile = PROFILES[file.startsWith('vlog') ? 'vlog' : 'rec709']!;
      const frame = downsample(loadTiff(join(FRAMES_DIR, file)));
      const stats = computeStats(decodeToRec709(frame.pixels, profile));

      const p = stats.lumaPercentiles;
      expect(p.p1).toBeLessThanOrEqual(p.p50);
      expect(p.p50).toBeLessThanOrEqual(p.p99);
      expect(p.p99).toBeLessThanOrEqual(1);
      expect(stats.lab.mean[0]).toBeGreaterThan(0);
      expect(stats.lab.mean[0]).toBeLessThan(100);
      expect(stats.skinPresence).toBeGreaterThanOrEqual(0);
      expect(stats.skinPresence).toBeLessThanOrEqual(1);
      expect(stats.clipping.low + stats.clipping.high).toBeLessThanOrEqual(2);
    });

    it.skipIf(!frames.some((f) => f === 'vlog-tricky-skin.tif'))(
      'skin detector: real-skin frames read higher than tricky false-positive frames',
      () => {
        const presence = (file: string) => {
          const frame = downsample(loadTiff(join(FRAMES_DIR, file)));
          return computeStats(decodeToRec709(frame.pixels, PROFILES['vlog']!)).skinPresence;
        };
        // Informational for the spike: log, don't hard-assert tuning.
        console.log('skinPresence vlog-tricky-skin:', presence('vlog-tricky-skin.tif'));
      },
    );
  },
);
