import { describe, expect, it } from 'vitest';
import {
  runAutoGradeLoop,
  signalFromImpact,
  applyProposal,
  parseCriticProposal,
  type CriticProposal,
  type RenderedRound,
  type AutoGradeParams,
} from '../../src/agent/index.js';
import type { GradeImpact } from '../../src/core/analysis/gradeImpact.js';
import type { GradeRoundSignal } from '../../src/core/analysis/gradeGuard.js';
import type { FootageStats } from '../../src/core/analysis/stats.js';
import { expARounds, expBRounds } from '../fixtures/gradeGuardGoldenRounds.js';

const DUMMY_STATS: FootageStats = {
  lumaPercentiles: { p1: 0.02, p5: 0.05, p25: 0.25, p50: 0.5, p75: 0.75, p95: 0.95, p99: 0.98 },
  lab: { mean: [50, 0, 0], std: [20, 10, 10] },
  bandChroma: { shadows: 10, mids: 10, highlights: 10 },
  saturation: { mean: 0.3, std: 0.1 },
  skinPresence: 0,
  clipping: { low: 0, high: 0 },
};

/** Build a GradeImpact whose guard-relevant fields come from a golden signal. */
function impactFrom(sig: GradeRoundSignal): GradeImpact {
  return {
    outStats: DUMMY_STATS,
    skinHueShiftDeg: sig.skinHueShiftDeg,
    skinChromaShiftPct: sig.skinChromaShiftPct,
    skinWeightTotal: sig.skinWeightTotal,
    castMagnitude: sig.castMagnitude,
    castDirectionDeg: 0,
  };
}

const proposal = (defectCount: number, verdict: 'continue' | 'stop' = 'continue'): CriticProposal =>
  parseCriticProposal({
    critique: '',
    defects: Array.from({ length: defectCount }, (_, i) => `defect ${i}`),
    // A non-empty override so the loop always has "a next thing to try".
    proposedOverrides: { chromaGain: 0.5 },
    verdict,
    verdictReasoning: '',
  });

/**
 * Drive the loop from a queue of (impact, defectCount) pairs. render() ignores
 * params and returns the next queued impact; critique() returns the paired
 * proposal. This replays a recorded round sequence through the real guard.
 */
function driveWith(queue: Array<{ sig: GradeRoundSignal; defects: number; verdict?: 'continue' | 'stop' }>, maxRounds: number) {
  let idx = 0;
  const seen: number[] = [];
  const render = (params: AutoGradeParams): RenderedRound<number> => {
    const entry = queue[Math.min(idx, queue.length - 1)]!;
    const r = { params, impact: impactFrom(entry.sig), frame: idx };
    return r;
  };
  const critique = (round: RenderedRound<number>): CriticProposal => {
    const entry = queue[Math.min(idx, queue.length - 1)]!;
    seen.push(idx);
    idx += 1;
    return proposal(entry.defects, entry.verdict);
  };
  return runAutoGradeLoop<number>({ render, critique }, {
    baseParams: { strength: 1, skinProtection: 0.5, overrides: {} },
    maxRounds,
  });
}

describe('signalFromImpact', () => {
  it('maps a GradeImpact + defect count into the guard signal shape', () => {
    const sig = signalFromImpact(impactFrom(expBRounds.baseline), 3);
    expect(sig.castMagnitude).toBeCloseTo(16.76, 1);
    expect(sig.defectCount).toBe(3);
    expect(sig.skinWeightTotal).toBeCloseTo(0.1676, 3);
  });
});

describe('applyProposal', () => {
  it('replaces proposed fields and keeps the rest, deep-merging chromaShape/channelCurves', () => {
    const cur: AutoGradeParams = {
      strength: 1,
      skinProtection: 0.5,
      overrides: { chromaGain: 1, chromaShape: { vibrance: 0.2 }, shadowTint: [1, 1] },
    };
    const p = parseCriticProposal({
      defects: [],
      proposedOpts: { strength: 0.9 },
      proposedOverrides: { chromaGain: 0.3, chromaShape: { softLimit: 22 } },
      verdict: 'continue',
    });
    const merged = applyProposal(cur, p);
    expect(merged.strength).toBe(0.9);
    expect(merged.skinProtection).toBe(0.5); // untouched
    expect(merged.overrides.chromaGain).toBe(0.3); // replaced
    expect(merged.overrides.shadowTint).toEqual([1, 1]); // kept
    expect(merged.overrides.chromaShape).toEqual({ vibrance: 0.2, softLimit: 22 }); // deep-merged
  });
});

describe('runAutoGradeLoop - s7 golden rounds', () => {
  it('Experiment B: accepts through round 2, rejects round 3 as a regression, keeps round 2', async () => {
    const result = await driveWith(
      [
        { sig: expBRounds.baseline, defects: 2 },
        { sig: expBRounds.round1, defects: 1 },
        { sig: expBRounds.round2, defects: 0 },
        { sig: expBRounds.round3, defects: 1 }, // blotches return
      ],
      3,
    );
    // round3 is rejected; best is round2.
    expect(result.best.round).toBe(2);
    const round3 = result.rounds.find((r) => r.round === 3)!;
    expect(round3.accepted).toBe(false);
    expect(round3.comparison?.verdict).toBe('regression');
    expect(result.stopReason).toMatch(/regression/);
  });

  it('Experiment A: never falsely flags the genuine correction progression as a regression', async () => {
    const result = await driveWith(
      [
        { sig: expARounds.baseline, defects: 3 },
        { sig: expARounds.round1, defects: 2 },
        { sig: expARounds.round4, defects: 1 },
        { sig: expARounds.round5, defects: 1 },
      ],
      3,
    );
    // Cast grows on skin-free content -> guard stays inconclusive, never regression.
    for (const r of result.rounds.filter((x) => x.round > 0)) {
      expect(r.comparison?.verdict).not.toBe('regression');
      expect(r.accepted).toBe(true);
    }
    expect(result.best.round).toBe(3);
  });

  it('stops when the critic names no defects and the guard sees no further improvement', async () => {
    // Zero defects throughout and only a sub-threshold cast change -> the first
    // tuned round is 'inconclusive', which with no named defects is the stop signal.
    const result = await driveWith(
      [
        { sig: expBRounds.round2, defects: 0 },
        { sig: { ...expBRounds.round2, castMagnitude: expBRounds.round2.castMagnitude + 0.1 }, defects: 0 },
      ],
      5,
    );
    expect(result.stopReason).toMatch(/converged|no named defects/);
    // Stopped at round 1, well before the 5-round cap (baseline + 1 tuned round).
    expect(result.rounds.length).toBe(2);
  });

  it('honors maxRounds as an upper bound', async () => {
    const result = await driveWith(
      [{ sig: expBRounds.baseline, defects: 2 }],
      4,
    );
    // All rounds keep improving-or-flat with defects present -> runs to the cap.
    expect(result.rounds.length).toBeLessThanOrEqual(5); // baseline + <=4
  });
});
