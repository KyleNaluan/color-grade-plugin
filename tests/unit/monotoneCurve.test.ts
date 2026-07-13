import { describe, it, expect } from 'vitest';
import { makeMonotoneCurve, makeShapeCurve } from '../../src/core/engine/monotoneCurve.js';

describe('makeMonotoneCurve', () => {
  it('interpolates control points exactly and stays monotone between them', () => {
    const c = makeMonotoneCurve([0, 0.25, 0.5, 1], [0, 0.2, 0.6, 1]);
    expect(c(0)).toBeCloseTo(0, 9);
    expect(c(0.25)).toBeCloseTo(0.2, 9);
    expect(c(0.5)).toBeCloseTo(0.6, 9);
    expect(c(1)).toBeCloseTo(1, 9);
    let prev = c(0);
    for (let i = 1; i <= 200; i++) {
      const y = c(i / 200);
      expect(y).toBeGreaterThanOrEqual(prev - 1e-9);
      prev = y;
    }
  });

  it('forces decreasing y control points back to monotone', () => {
    const c = makeMonotoneCurve([0, 0.5, 1], [0, 0.8, 0.6]);
    expect(c(1)).toBeGreaterThanOrEqual(c(0.5));
  });
});

describe('makeShapeCurve', () => {
  it('preserves a non-monotone shape (peak in the mids)', () => {
    const c = makeShapeCurve([0, 0.5, 1], [0.9, 1.2, 0.8]);
    expect(c(0)).toBeCloseTo(0.9, 9);
    expect(c(0.5)).toBeCloseTo(1.2, 9);
    expect(c(1)).toBeCloseTo(0.8, 9);
    expect(c(0.5)).toBeGreaterThan(c(0.1));
    expect(c(0.5)).toBeGreaterThan(c(0.9));
  });

  it('never overshoots its control-point range', () => {
    const c = makeShapeCurve([0, 0.3, 0.6, 1], [1, 0.2, 1.8, 0.5]);
    for (let i = 0; i <= 400; i++) {
      const y = c(i / 400);
      expect(y).toBeGreaterThanOrEqual(0.2 - 1e-9);
      expect(y).toBeLessThanOrEqual(1.8 + 1e-9);
    }
  });

  it('clamps to endpoint values outside the control-point domain', () => {
    const c = makeShapeCurve([0.2, 0.8], [0.5, 1.5]);
    expect(c(0)).toBeCloseTo(0.5, 9);
    expect(c(1)).toBeCloseTo(1.5, 9);
  });
});
