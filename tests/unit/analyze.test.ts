import { describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { analyzeCurrentFrame, downsampleFrame } from '../../src/panel/analyze';
import { createFileFrameSource } from '../../src/host/fileFrameSource';
import { PROFILES } from '../../src/core/color/index.js';
import type { Bridge } from '../../src/host/bridge';
import type { Frame, FrameFileReader, RenderedFrameRef } from '../../src/host/frameSource';

const FIXTURES = join(__dirname, '..', 'fixtures', 'frame-source');

const nodeReader: FrameFileReader = {
  async read(path: string) {
    return new Uint8Array(readFileSync(path));
  },
};

/** A bridge whose only relevant behavior is the current-time it reports. */
const timeBridge = (time: number | null): Pick<Bridge, 'getCurrentTime'> => ({
  async getCurrentTime() {
    return time;
  },
});

/** File-backed fake FrameSource fed with the committed synthetic TIFF fixture. */
const fixtureSource = (name = 'synthetic.tif', format: RenderedFrameRef['format'] = 'tiff') =>
  createFileFrameSource(() => ({ path: join(FIXTURES, name), format }), nodeReader);

describe('analyzeCurrentFrame', () => {
  it('measures Footage stats of the current frame through the FrameSource', async () => {
    const result = await analyzeCurrentFrame(timeBridge(1.5), fixtureSource(), PROFILES['rec709']!);

    expect(result.time).toBe(1.5);
    expect(result.width).toBe(64);
    expect(result.height).toBe(48);
    expect(result.bitDepthOfSource).toBe(16);
    expect(result.profileName).toBe(PROFILES['rec709']!.name);

    const p = result.stats.lumaPercentiles;
    expect(p.p1).toBeLessThanOrEqual(p.p50);
    expect(p.p50).toBeLessThanOrEqual(p.p99);
    // The picture has clipped black and white corners.
    expect(result.stats.clipping.low).toBeGreaterThan(0);
    expect(result.stats.clipping.high).toBeGreaterThan(0);
    // A skin-toned patch should register some skin presence.
    expect(result.stats.skinPresence).toBeGreaterThan(0);
    expect(result.stats.skinPresence).toBeLessThanOrEqual(1);
  });

  it('works identically against the PNG fallback fixture (same consumer code)', async () => {
    const result = await analyzeCurrentFrame(
      timeBridge(0),
      fixtureSource('synthetic.png', 'png'),
      PROFILES['rec709']!,
    );
    expect(result.bitDepthOfSource).toBe(8);
    expect(result.stats.clipping.high).toBeGreaterThan(0);
  });

  it('throws when no comp is active', async () => {
    await expect(
      analyzeCurrentFrame(timeBridge(null), fixtureSource(), PROFILES['rec709']!),
    ).rejects.toThrow(/no active comp/i);
  });
});

describe('downsampleFrame', () => {
  it('returns frames within budget untouched', () => {
    const frame: Frame = { width: 4, height: 2, data: new Float32Array(4 * 2 * 3), bitDepthOfSource: 16 };
    expect(downsampleFrame(frame, 960)).toBe(frame);
  });

  it('shrinks wide frames to the max width and preserves bit depth', () => {
    const w = 100;
    const h = 50;
    const data = new Float32Array(w * h * 3).fill(0.5);
    const out = downsampleFrame({ width: w, height: h, data, bitDepthOfSource: 8 }, 20);
    expect(out.width).toBe(20);
    expect(out.height).toBe(10);
    expect(out.bitDepthOfSource).toBe(8);
    expect(out.data.length).toBe(20 * 10 * 3);
  });
});
