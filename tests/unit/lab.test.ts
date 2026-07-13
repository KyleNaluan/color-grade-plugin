import { describe, it, expect } from 'vitest';
import { linearRec709ToLab, labToLinearRec709, xyzToLab } from '../../src/core/color/lab.js';

describe('CIELAB conversions (canonical vectors, D65)', () => {
  it('reference white -> L=100, a=b=0', () => {
    const [l, a, b] = linearRec709ToLab([1, 1, 1]);
    expect(l).toBeCloseTo(100, 3);
    expect(a).toBeCloseTo(0, 3);
    expect(b).toBeCloseTo(0, 3);
  });

  it('black -> L=0', () => {
    const [l, a, b] = linearRec709ToLab([0, 0, 0]);
    expect(l).toBeCloseTo(0, 3);
    expect(a).toBeCloseTo(0, 3);
    expect(b).toBeCloseTo(0, 3);
  });

  it('18% grey -> L ~ 49.50', () => {
    const [l] = linearRec709ToLab([0.18, 0.18, 0.18]);
    expect(l).toBeCloseTo(49.496, 2);
  });

  it('pure red primary -> canonical sRGB/Rec.709 red LAB (~53.2, 80.1, 67.2)', () => {
    const [l, a, b] = linearRec709ToLab([1, 0, 0]);
    expect(l).toBeCloseTo(53.24, 1);
    expect(a).toBeCloseTo(80.09, 1);
    expect(b).toBeCloseTo(67.2, 1);
  });

  it('pure blue primary -> canonical blue LAB (~32.3, 79.2, -107.9)', () => {
    const [l, a, b] = linearRec709ToLab([0, 0, 1]);
    expect(l).toBeCloseTo(32.3, 1);
    expect(a).toBeCloseTo(79.2, 1);
    expect(b).toBeCloseTo(-107.86, 1);
  });

  it('XYZ of D65 white -> L=100, a=b=0 (within rounding of the 0.95047/1.08883 constants)', () => {
    const [l, a, b] = xyzToLab([0.95047, 1.0, 1.08883]);
    expect(l).toBeCloseTo(100, 3);
    expect(Math.abs(a)).toBeLessThan(0.05);
    expect(Math.abs(b)).toBeLessThan(0.05);
  });

  it('round-trips arbitrary colors', () => {
    for (const rgb of [
      [0.2, 0.5, 0.8],
      [0.9, 0.1, 0.3],
      [0.05, 0.05, 0.05],
      [0.7, 0.7, 0.2],
    ] as const) {
      const back = labToLinearRec709(linearRec709ToLab([...rgb]));
      for (let i = 0; i < 3; i++) expect(back[i]).toBeCloseTo(rgb[i]!, 6);
    }
  });
});
