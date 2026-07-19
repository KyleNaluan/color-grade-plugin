import { describe, expect, it } from 'vitest';
import { classifyGradeRound, classifyGradeSequence, type GradeRoundSignal } from '../../src/core/analysis/gradeGuard.js';
import { expARounds, expBRounds } from '../fixtures/gradeGuardGoldenRounds.js';

function signal(overrides: Partial<GradeRoundSignal> = {}): GradeRoundSignal {
  return { castMagnitude: 10, skinHueShiftDeg: 2, skinChromaShiftPct: 5, skinWeightTotal: 0.2, ...overrides };
}

describe('classifyGradeRound (synthetic cases)', () => {
  it('flags a regression when castMagnitude worsens with no compensating skin improvement', () => {
    const prev = signal({ castMagnitude: 8, skinHueShiftDeg: 2, skinChromaShiftPct: -1.5 });
    const cur = signal({ castMagnitude: 11.5, skinHueShiftDeg: 2.7, skinChromaShiftPct: -0.3 });
    const result = classifyGradeRound(prev, cur);
    expect(result.verdict).toBe('regression');
    expect(result.skinSignalActive).toBe(true);
  });

  it('calls it improvement when castMagnitude worsens but skin hue and chroma both clearly improve', () => {
    const prev = signal({ castMagnitude: 8, skinHueShiftDeg: 8, skinChromaShiftPct: 20 });
    const cur = signal({ castMagnitude: 12, skinHueShiftDeg: 1, skinChromaShiftPct: 1 });
    const result = classifyGradeRound(prev, cur);
    expect(result.verdict).toBe('improvement');
  });

  it('calls it improvement when castMagnitude improves and skin protection does not degrade', () => {
    const prev = signal({ castMagnitude: 16, skinHueShiftDeg: 5, skinChromaShiftPct: 40 });
    const cur = signal({ castMagnitude: 13, skinHueShiftDeg: 2, skinChromaShiftPct: 13 });
    const result = classifyGradeRound(prev, cur);
    expect(result.verdict).toBe('improvement');
  });

  it('flags a regression when castMagnitude improves but skin protection degrades', () => {
    const prev = signal({ castMagnitude: 16, skinHueShiftDeg: 2, skinChromaShiftPct: 2 });
    const cur = signal({ castMagnitude: 13, skinHueShiftDeg: 8, skinChromaShiftPct: 2 });
    const result = classifyGradeRound(prev, cur);
    expect(result.verdict).toBe('regression');
  });

  it('flags a regression when castMagnitude holds flat but skin protection degrades', () => {
    const prev = signal({ castMagnitude: 10, skinHueShiftDeg: 1, skinChromaShiftPct: 1 });
    const cur = signal({ castMagnitude: 10.2, skinHueShiftDeg: 6, skinChromaShiftPct: 1 });
    const result = classifyGradeRound(prev, cur);
    expect(result.verdict).toBe('regression');
  });

  it('is inconclusive when castMagnitude worsens but skin presence is too low to corroborate', () => {
    const prev = signal({ castMagnitude: 5.86, skinWeightTotal: 0, skinHueShiftDeg: 0, skinChromaShiftPct: 0 });
    const cur = signal({ castMagnitude: 11.3, skinWeightTotal: 0, skinHueShiftDeg: 0, skinChromaShiftPct: 0 });
    const result = classifyGradeRound(prev, cur);
    expect(result.verdict).toBe('inconclusive');
    expect(result.skinSignalActive).toBe(false);
  });

  it('is inconclusive when every signal sits within noise thresholds', () => {
    const prev = signal({ castMagnitude: 10, skinHueShiftDeg: 2, skinChromaShiftPct: 5 });
    const cur = signal({ castMagnitude: 10.3, skinHueShiftDeg: 2.2, skinChromaShiftPct: 5.5 });
    const result = classifyGradeRound(prev, cur);
    expect(result.verdict).toBe('inconclusive');
  });

  it('flags a regression immediately when a supplied defect count increases, regardless of other signals', () => {
    const prev = signal({ castMagnitude: 16, skinHueShiftDeg: 5, skinChromaShiftPct: 40, defectCount: 1 });
    const cur = signal({ castMagnitude: 8, skinHueShiftDeg: 1, skinChromaShiftPct: 1, defectCount: 2 });
    const result = classifyGradeRound(prev, cur);
    expect(result.verdict).toBe('regression');
    expect(result.reasons.join(' ')).toMatch(/defect count increased/);
  });

  it('calls it improvement when a supplied defect count decreases and no other signal worsens', () => {
    const prev = signal({ castMagnitude: 16, skinHueShiftDeg: 5, skinChromaShiftPct: 40, defectCount: 2 });
    const cur = signal({ castMagnitude: 13, skinHueShiftDeg: 2, skinChromaShiftPct: 13, defectCount: 1 });
    const result = classifyGradeRound(prev, cur);
    expect(result.verdict).toBe('improvement');
  });

  it('is exact identity (deltas all zero) for two identical rounds', () => {
    const s = signal();
    const result = classifyGradeRound(s, s);
    expect(result.deltas.castMagnitude).toBe(0);
    expect(result.deltas.skinHueShiftDeg).toBe(0);
    expect(result.deltas.skinChromaShiftPct).toBe(0);
    expect(result.verdict).toBe('inconclusive');
  });
});

describe('classifyGradeRound (s7 golden rounds - the regression this guard exists to catch)', () => {
  it('MUST flag Exp B round 3 as a regression relative to round 2', () => {
    const result = classifyGradeRound(expBRounds.round2, expBRounds.round3);
    expect(result.verdict).toBe('regression');
  });

  it('does NOT flag the genuine Exp B improvement rounds as a regression', () => {
    expect(classifyGradeRound(expBRounds.baseline, expBRounds.round1).verdict).not.toBe('regression');
    expect(classifyGradeRound(expBRounds.round1, expBRounds.round2).verdict).not.toBe('regression');
  });

  it('does NOT flag the genuine Exp A improvement rounds as a regression', () => {
    expect(classifyGradeRound(expARounds.baseline, expARounds.round1).verdict).not.toBe('regression');
    expect(classifyGradeRound(expARounds.round1, expARounds.round4).verdict).not.toBe('regression');
    expect(classifyGradeRound(expARounds.round4, expARounds.round5).verdict).not.toBe('regression');
  });

  it('classifies the full Exp B sequence with exactly one regression, at round2->round3', () => {
    const sequence = classifyGradeSequence([expBRounds.baseline, expBRounds.round1, expBRounds.round2, expBRounds.round3]);
    expect(sequence.map((c) => c.verdict)).toEqual(['improvement', 'improvement', 'regression']);
  });
});
