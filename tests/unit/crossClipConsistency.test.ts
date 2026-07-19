import { describe, expect, it } from 'vitest';
import {
  checkSceneConsistency,
  compareClipPair,
  DEFAULT_CROSS_CLIP_THRESHOLDS,
  formatSceneConsistencyReport,
  type ClipGradeSummary,
} from '../../src/core/analysis/crossClipConsistency.js';
import type { GradeImpact } from '../../src/core/analysis/gradeImpact.js';
import type { FootageStats } from '../../src/core/analysis/stats.js';

function stats(overrides: Partial<FootageStats> = {}): FootageStats {
  return {
    lumaPercentiles: { p1: 0.02, p5: 0.05, p25: 0.2, p50: 0.45, p75: 0.7, p95: 0.9, p99: 0.98 },
    lab: { mean: [50, 5, 10], std: [20, 8, 8] },
    bandChroma: { shadows: 5, mids: 12, highlights: 8 },
    saturation: { mean: 0.3, std: 0.15 },
    skinPresence: 0.1,
    clipping: { low: 0.001, high: 0.001 },
    ...overrides,
  };
}

function impact(overrides: Omit<Partial<GradeImpact>, 'outStats'> & { outStats?: Partial<FootageStats> } = {}): GradeImpact {
  const { outStats, ...rest } = overrides;
  return {
    outStats: stats(outStats),
    skinHueShiftDeg: 2,
    skinChromaShiftPct: 3,
    skinWeightTotal: 0.1,
    castMagnitude: 8,
    castDirectionDeg: 40,
    ...rest,
  };
}

function clip(
  clipId: string,
  overrides: Omit<Partial<GradeImpact>, 'outStats'> & { outStats?: Partial<FootageStats> } = {},
): ClipGradeSummary {
  return { clipId, impact: impact(overrides) };
}

describe('compareClipPair', () => {
  it('is consistent for two clips with matching post-grade looks', () => {
    const a = clip('A');
    const b = clip('B', { skinHueShiftDeg: 2.3, skinChromaShiftPct: 2.7, castMagnitude: 8.4, castDirectionDeg: 43 });
    const result = compareClipPair(a, b);
    expect(result.verdict).toBe('consistent');
    expect(result.reasons).toEqual([]);
  });

  it('flags divergence when the two clips output different mean color (LAB distance)', () => {
    const a = clip('A', { outStats: { lab: { mean: [50, 5, 10], std: [20, 8, 8] } } });
    const b = clip('B', { outStats: { lab: { mean: [50, 15, 20], std: [20, 8, 8] } } });
    const result = compareClipPair(a, b);
    expect(result.verdict).toBe('diverged');
    expect(result.reasons.join(' ')).toMatch(/output color diverges/);
  });

  it('flags divergence when midtone exposure diverges', () => {
    const a = clip('A', { outStats: { lumaPercentiles: { p1: 0.02, p5: 0.05, p25: 0.2, p50: 0.4, p75: 0.7, p95: 0.9, p99: 0.98 } } });
    const b = clip('B', { outStats: { lumaPercentiles: { p1: 0.02, p5: 0.05, p25: 0.2, p50: 0.55, p75: 0.7, p95: 0.9, p99: 0.98 } } });
    const result = compareClipPair(a, b);
    expect(result.verdict).toBe('diverged');
    expect(result.reasons.join(' ')).toMatch(/midtone exposure diverges/);
  });

  it('flags divergence when grades push color by very different magnitudes', () => {
    const a = clip('A', { castMagnitude: 4 });
    const b = clip('B', { castMagnitude: 12 });
    const result = compareClipPair(a, b);
    expect(result.verdict).toBe('diverged');
    expect(result.reasons.join(' ')).toMatch(/different amounts/);
  });

  it('flags divergence when grades push color in different directions, above the noise floor', () => {
    const a = clip('A', { castMagnitude: 8, castDirectionDeg: 10 });
    const b = clip('B', { castMagnitude: 8, castDirectionDeg: 60 });
    const result = compareClipPair(a, b);
    expect(result.verdict).toBe('diverged');
    expect(result.reasons.join(' ')).toMatch(/different directions/);
    expect(result.deltas.castDirectionDeltaDeg).toBeCloseTo(50, 5);
  });

  it('does not evaluate direction divergence when either clip cast is too close to neutral', () => {
    const a = clip('A', { castMagnitude: 0.5, castDirectionDeg: 10 });
    const b = clip('B', { castMagnitude: 8, castDirectionDeg: 200 });
    const result = compareClipPair(a, b);
    expect(result.deltas.castDirectionDeltaDeg).toBeUndefined();
    expect(result.reasons.join(' ')).not.toMatch(/different directions/);
  });

  it('handles the circular wraparound correctly (350deg vs 10deg is 20deg apart, not 340deg)', () => {
    const a = clip('A', { castMagnitude: 8, castDirectionDeg: 350 });
    const b = clip('B', { castMagnitude: 8, castDirectionDeg: 10 });
    const result = compareClipPair(a, b);
    expect(result.deltas.castDirectionDeltaDeg).toBeCloseTo(20, 5);
    expect(result.verdict).toBe('consistent');
  });

  it('flags divergence when skin hue/chroma shift diverges and skin presence is trustworthy in both clips', () => {
    const a = clip('A', { skinHueShiftDeg: 1, skinWeightTotal: 0.15 });
    const b = clip('B', { skinHueShiftDeg: 9, skinWeightTotal: 0.15 });
    const result = compareClipPair(a, b);
    expect(result.skinSignalActive).toBe(true);
    expect(result.verdict).toBe('diverged');
    expect(result.reasons.join(' ')).toMatch(/skin hue shift diverges/);
  });

  it('does not raise skin-based reasons when skin presence is too low in either clip', () => {
    const a = clip('A', { skinHueShiftDeg: 1, skinWeightTotal: 0 });
    const b = clip('B', { skinHueShiftDeg: 9, skinWeightTotal: 0 });
    const result = compareClipPair(a, b);
    expect(result.skinSignalActive).toBe(false);
    expect(result.reasons.join(' ')).not.toMatch(/skin/);
  });

  it('is exact identity (all deltas zero, consistent) for two clips with an identical impact', () => {
    const a = clip('A');
    const b = clip('B');
    const result = compareClipPair(a, b);
    expect(result.verdict).toBe('consistent');
    expect(result.deltas.outLabDeltaE).toBe(0);
    expect(result.deltas.exposureDeltaP50).toBe(0);
    expect(result.deltas.castMagnitudeDelta).toBe(0);
    expect(result.deltas.castDirectionDeltaDeg).toBe(0);
  });
});

describe('checkSceneConsistency', () => {
  it('compares every pair in a multi-clip scene and collects the diverged subset', () => {
    const a = clip('A');
    const b = clip('B');
    const c = clip('C', { outStats: { lab: { mean: [50, 30, 30], std: [20, 8, 8] } } });
    const report = checkSceneConsistency([a, b, c], DEFAULT_CROSS_CLIP_THRESHOLDS, 'porch-scene');

    expect(report.sceneId).toBe('porch-scene');
    expect(report.clipIds).toEqual(['A', 'B', 'C']);
    expect(report.pairs).toHaveLength(3);
    expect(report.diverged.map((p) => [p.clipA, p.clipB])).toEqual([
      ['A', 'C'],
      ['B', 'C'],
    ]);
  });

  it('returns no pairs for a single clip', () => {
    const report = checkSceneConsistency([clip('A')]);
    expect(report.pairs).toEqual([]);
    expect(report.diverged).toEqual([]);
  });
});

describe('formatSceneConsistencyReport', () => {
  it('renders a readable summary noting each pair and any divergence reasons', () => {
    const a = clip('A');
    const b = clip('B', { outStats: { lab: { mean: [50, 40, 40], std: [20, 8, 8] } } });
    const report = checkSceneConsistency([a, b], DEFAULT_CROSS_CLIP_THRESHOLDS, 'kitchen');
    const text = formatSceneConsistencyReport(report);
    expect(text).toMatch(/scene "kitchen"/);
    expect(text).toMatch(/1\/1 pair\(s\) DIVERGED/);
    expect(text).toMatch(/\[DIVERGED\] A <-> B/);
    expect(text).toMatch(/output color diverges/);
  });

  it('notes when all pairs are consistent', () => {
    const report = checkSceneConsistency([clip('A'), clip('B')]);
    const text = formatSceneConsistencyReport(report);
    expect(text).toMatch(/all pairs consistent/);
  });

  it('notes when fewer than two clips were supplied', () => {
    const text = formatSceneConsistencyReport(checkSceneConsistency([clip('A')]));
    expect(text).toMatch(/need at least 2 clips/);
  });
});
