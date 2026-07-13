import { describe, it, expect } from 'vitest';
import { bakeDecodeLut } from '../../src/core/lut/decodeLut.js';
import { decodePixelToRec709 } from '../../src/core/color/decode.js';
import { parseCube, sampleLut, writeCube } from '../../src/core/lut/cube.js';
import { VLOG } from '../../src/core/color/vlog.js';
import { REC709 } from '../../src/core/color/rec709.js';
import type { Vec3 } from '../../src/core/color/types.js';

describe('bakeDecodeLut', () => {
  it('samples decode() directly at grid points - no re-derived math', () => {
    const size = 17;
    const lut = bakeDecodeLut(VLOG, size);
    let seed = 5;
    const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
    for (let k = 0; k < 200; k++) {
      const rgb: Vec3 = [
        Math.floor(rand() * size) / (size - 1),
        Math.floor(rand() * size) / (size - 1),
        Math.floor(rand() * size) / (size - 1),
      ];
      const expected = decodePixelToRec709(rgb, VLOG);
      const viaLut = sampleLut(lut, rgb);
      for (let c = 0; c < 3; c++) expect(viaLut[c]).toBeCloseTo(expected[c]!, 4);
    }
  });

  it('stays close to the direct decode off-grid (trilinear interpolation, default 65-point grid)', () => {
    const lut = bakeDecodeLut(VLOG);
    let seed = 21;
    const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
    // Real V-Log footage keeps RGB roughly correlated (it's a photographed
    // scene, not independent per-channel noise); deep in super-white territory
    // (well above 100% IRE, ~encoded 0.8) wildly decorrelated channels can hit
    // a genuine near-discontinuous ridge in the gamut-matrix-mixed, clamped
    // decode - that's an inherent property of the transform, not a LUT bug, so
    // this samples the plausible working range a real clip would occupy.
    for (let k = 0; k < 200; k++) {
      const base = 0.1 + rand() * 0.6; // V-Log black 0.125 .. comfortably below clip
      const jitter = () => Math.min(1, Math.max(0, base + (rand() - 0.5) * 0.08));
      const rgb: Vec3 = [jitter(), jitter(), jitter()];
      const expected = decodePixelToRec709(rgb, VLOG);
      const viaLut = sampleLut(lut, rgb);
      for (let c = 0; c < 3; c++) expect(Math.abs(viaLut[c]! - expected[c]!)).toBeLessThan(0.08);
    }
  });

  it('titles the LUT with the profile name', () => {
    expect(bakeDecodeLut(VLOG).title).toBe('V-Log Decode');
  });

  it('stays bounded in [0,1] across the grid, including flat V-Log highlights', () => {
    const lut = bakeDecodeLut(VLOG);
    let min = Infinity;
    let max = -Infinity;
    for (const v of lut.data) {
      if (v < min) min = v;
      if (v > max) max = v;
    }
    expect(min).toBeGreaterThanOrEqual(0);
    expect(max).toBeLessThanOrEqual(1);
  });

  it('round-trips through .cube text', () => {
    const lut = bakeDecodeLut(VLOG, 17);
    const reparsed = parseCube(writeCube(lut));
    expect(reparsed.size).toBe(17);
    for (let i = 0; i < lut.data.length; i++) {
      expect(Math.abs(reparsed.data[i]! - lut.data[i]!)).toBeLessThan(1e-5);
    }
  });

  it('bakes an identity LUT for the Rec.709 profile (no-op decode)', () => {
    const lut = bakeDecodeLut(REC709, 9);
    let seed = 13;
    const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
    for (let k = 0; k < 50; k++) {
      const rgb: Vec3 = [rand(), rand(), rand()];
      const out = sampleLut(lut, rgb);
      for (let c = 0; c < 3; c++) expect(out[c]).toBeCloseTo(rgb[c]!, 5);
    }
  });
});
