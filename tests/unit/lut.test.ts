import { describe, it, expect } from 'vitest';
import { bakeLut, writeCube, parseCube, sampleLut } from '../../src/core/lut/cube.js';
import { buildTransform } from '../../src/core/engine/engine.js';
import { computeStats } from '../../src/core/analysis/stats.js';
import { tealOrange } from '../../src/themes/teal-orange.js';
import { THEMES } from '../../src/themes/index.js';
import type { Vec3 } from '../../src/core/color/types.js';

/** Deterministic pseudo-random pixels so the property test is reproducible. */
function syntheticFootage(n: number): Float32Array {
  let seed = 42;
  const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
  const px = new Float32Array(n * 3);
  for (let i = 0; i < px.length; i++) px[i] = rand();
  return px;
}

describe('LUT bake -> write -> parse round trip', () => {
  const stats = computeStats(syntheticFootage(5000));
  const transform = buildTransform(stats, tealOrange);
  const lut = bakeLut(transform, 33, 'roundtrip');
  const reparsed = parseCube(writeCube(lut));

  it('preserves size and title through .cube serialization', () => {
    expect(reparsed.size).toBe(33);
    expect(reparsed.title).toBe('roundtrip');
  });

  it('grid points survive serialization within text precision', () => {
    for (let i = 0; i < lut.data.length; i++) {
      expect(Math.abs(reparsed.data[i]! - lut.data[i]!)).toBeLessThan(1e-5);
    }
  });

  it('parsed LUT sampled at grid points equals the direct transform', () => {
    const size = 33;
    let seed = 7;
    const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
    for (let k = 0; k < 200; k++) {
      const g: Vec3 = [
        Math.floor(rand() * size) / (size - 1),
        Math.floor(rand() * size) / (size - 1),
        Math.floor(rand() * size) / (size - 1),
      ];
      const direct = transform(g);
      const viaLut = sampleLut(reparsed, g);
      for (let c = 0; c < 3; c++) expect(Math.abs(viaLut[c]! - direct[c]!)).toBeLessThan(1e-4);
    }
  });

  // 33^3 trilinear vs an analytically curved transform: worst-case error lives in
  // steep tone-curve regions; 0.05 (~5% of full scale) is the acceptance bound.
  it('trilinear interpolation stays close to the direct transform off-grid', () => {
    let seed = 99;
    const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
    for (let k = 0; k < 500; k++) {
      const p: Vec3 = [rand(), rand(), rand()];
      const direct = transform(p);
      const viaLut = sampleLut(reparsed, p);
      for (let c = 0; c < 3; c++) expect(Math.abs(viaLut[c]! - direct[c]!)).toBeLessThan(0.05);
    }
  });

  // Every theme now carries expanded overrides (authored curves + chroma
  // shaping); round-trip each one so those code paths survive .cube text.
  it.each(Object.keys(THEMES))('round-trips a transform using %s expanded overrides', (name) => {
    const theme = THEMES[name]!;
    const t = buildTransform(stats, theme, { strength: 1 });
    const rt = parseCube(writeCube(bakeLut(t, 17, name)));
    let seed = 11;
    const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
    for (let k = 0; k < 200; k++) {
      const g: Vec3 = [
        Math.floor(rand() * 17) / 16,
        Math.floor(rand() * 17) / 16,
        Math.floor(rand() * 17) / 16,
      ];
      const direct = t(g);
      const viaLut = sampleLut(rt, g);
      for (let c = 0; c < 3; c++) {
        expect(viaLut[c]).toBeGreaterThanOrEqual(0);
        expect(viaLut[c]).toBeLessThanOrEqual(1);
        expect(Math.abs(viaLut[c]! - direct[c]!)).toBeLessThan(1e-4);
      }
    }
  });

  it('identity transform bakes to an identity LUT', () => {
    const id = bakeLut((rgb) => rgb, 17);
    const reparsedId = parseCube(writeCube(id));
    let seed = 3;
    const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
    for (let k = 0; k < 100; k++) {
      const p: Vec3 = [rand(), rand(), rand()];
      const out = sampleLut(reparsedId, p);
      for (let c = 0; c < 3; c++) expect(out[c]).toBeCloseTo(p[c]!, 5);
    }
  });
});
