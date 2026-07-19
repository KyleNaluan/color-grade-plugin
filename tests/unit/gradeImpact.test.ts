import { describe, expect, it } from 'vitest';
import { computeGradeImpact } from '../../src/core/analysis/gradeImpact.js';
import type { Transform } from '../../src/core/engine/engine.js';

// A small synthetic frame: neutral gray, a saturated color, and a skin-toned
// pixel (LAB hue ~42deg, chroma > 6, per skinWedgeWeight in stats.ts) so both
// the overall-cast and skin-region signals have something to measure.
function syntheticFrame(): Float32Array {
  return new Float32Array([
    0.5, 0.5, 0.5, // neutral gray
    0.1, 0.6, 0.2, // saturated green
    0.8, 0.6, 0.5, // skin-toned
    0.85, 0.62, 0.52, // skin-toned, slightly different
  ]);
}

const identity: Transform = (rgb) => rgb;

describe('computeGradeImpact', () => {
  it('reports zero cast and zero skin shift for an identity transform', () => {
    const impact = computeGradeImpact(syntheticFrame(), identity);
    expect(impact.castMagnitude).toBeCloseTo(0, 6);
    expect(impact.skinHueShiftDeg).toBeCloseTo(0, 6);
    expect(impact.skinChromaShiftPct).toBeCloseTo(0, 6);
    expect(impact.skinWeightTotal).toBeGreaterThan(0);
  });

  it('reports a nonzero cast magnitude for a transform that shifts color', () => {
    const warm: Transform = ([r, g, b]) => [Math.min(1, r + 0.05), g, Math.max(0, b - 0.05)];
    const impact = computeGradeImpact(syntheticFrame(), warm);
    expect(impact.castMagnitude).toBeGreaterThan(0);
  });

  it('measures skin hue drift when a transform rotates only skin-toned pixels', () => {
    const skinShift: Transform = ([r, g, b]) => (r > 0.7 ? [r, Math.min(1, g + 0.15), b] : [r, g, b]);
    const impact = computeGradeImpact(syntheticFrame(), skinShift);
    expect(Math.abs(impact.skinHueShiftDeg)).toBeGreaterThan(0);
  });
});
