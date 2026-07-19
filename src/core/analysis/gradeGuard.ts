/**
 * Rule-based grade QC/regression guard: classifies a graded round as an
 * improvement, a regression, or inconclusive relative to the previous round,
 * using round-over-round `GradeImpact`-style deltas (castMagnitude,
 * skin-hue-shift, skin-chroma-shift) - never raw stat-distance-to-target.
 *
 * Evidence for this design: `data/cg-agents-study/report.md` (secs 1f/2a/3)
 * and `data/cg-agent-grade-s7/report.md`'s Exp B round 3. Every vision model
 * tested there (0/16-0/26 calls, free and paid tiers, Flash and Pro) called
 * round 3 "same" or "better" than round 2 because round 3's stats sit
 * numerically closer to the reference target - even though round 3 visibly
 * reintroduces chroma-noise blotches on background highlights that round 2
 * had already fixed. Stat-distance-to-target is deliberately not an input
 * here; it is exactly the metric that produced that wrong verdict.
 */

export type GradeRoundVerdict = 'improvement' | 'regression' | 'inconclusive';

/**
 * The subset of `GradeImpact` (plus an optional external defect count) this
 * guard reasons about. Construct directly from a `computeGradeImpact` result.
 */
export interface GradeRoundSignal {
  /** LAB distance the frame's average color moved, in->out. */
  castMagnitude: number;
  /** Circular-mean skin-region hue shift in degrees, in->out. */
  skinHueShiftDeg: number;
  /** Relative skin-region chroma change in %, in->out. */
  skinChromaShiftPct: number;
  /** Skin-weighted pixel fraction; gates whether the two skin signals above are trustworthy. */
  skinWeightTotal: number;
  /** Optional externally-supplied defect count for this round (e.g. a vision critic's named-defect tally). */
  defectCount?: number;
}

export interface GradeGuardThresholds {
  /** LAB units; castMagnitude must move by more than this to count as a real change, not noise. */
  castMagnitude: number;
  /** Degrees; |skinHueShiftDeg| must move by more than this to count as a real change. */
  skinHueShiftDeg: number;
  /** Percentage points; |skinChromaShiftPct| must move by more than this to count as a real change. */
  skinChromaShiftPct: number;
  /** Minimum skinWeightTotal, required in BOTH rounds, for the skin signals to be trusted at all. */
  skinPresence: number;
}

/**
 * Thresholds chosen as a noise floor around each signal, not tuned to any one
 * golden round: `skinPresence` matches the spike CLI's own
 * skin-presence-active cutoff (`scripts/spike.ts`, `skinPresence > 0.02`).
 */
export const DEFAULT_GRADE_GUARD_THRESHOLDS: GradeGuardThresholds = {
  castMagnitude: 1.5,
  skinHueShiftDeg: 1,
  skinChromaShiftPct: 3,
  skinPresence: 0.02,
};

export interface GradeRoundComparison {
  verdict: GradeRoundVerdict;
  /** Human-readable explanation of which signals drove the verdict. */
  reasons: string[];
  deltas: {
    castMagnitude: number;
    /** Delta of |skinHueShiftDeg|, not the raw signed shift (drift-from-neutral is what matters). */
    skinHueShiftDeg: number;
    /** Delta of |skinChromaShiftPct|, not the raw signed shift. */
    skinChromaShiftPct: number;
    defectCount?: number;
  };
  /** Whether skinWeightTotal cleared `skinPresence` in both rounds, i.e. whether the skin deltas above were trusted. */
  skinSignalActive: boolean;
}

type Direction = 'better' | 'worse' | 'flat';

function classify(delta: number, threshold: number, active = true): Direction {
  if (!active) return 'flat';
  if (delta > threshold) return 'worse';
  if (delta < -threshold) return 'better';
  return 'flat';
}

/**
 * Classifies `current` relative to `previous` using only round-over-round
 * GradeImpact deltas (plus an optional defect count). Conservative by
 * design: when the skin signals aren't trustworthy (too little skin in
 * frame) and castMagnitude alone is ambiguous, this returns 'inconclusive'
 * rather than guessing - a correction-style grade legitimately grows
 * castMagnitude round over round on skin-free content (see
 * `data/cg-agent-grade-s7/report.md` Experiment A), so cast growth alone is
 * never sufficient evidence of a regression.
 */
export function classifyGradeRound(
  previous: GradeRoundSignal,
  current: GradeRoundSignal,
  thresholds: GradeGuardThresholds = DEFAULT_GRADE_GUARD_THRESHOLDS,
): GradeRoundComparison {
  const skinSignalActive = Math.min(previous.skinWeightTotal, current.skinWeightTotal) >= thresholds.skinPresence;

  const dCast = current.castMagnitude - previous.castMagnitude;
  const dHue = Math.abs(current.skinHueShiftDeg) - Math.abs(previous.skinHueShiftDeg);
  const dChroma = Math.abs(current.skinChromaShiftPct) - Math.abs(previous.skinChromaShiftPct);
  const hasDefectCounts = previous.defectCount !== undefined && current.defectCount !== undefined;
  const dDefect = hasDefectCounts ? current.defectCount! - previous.defectCount! : undefined;

  const cast = classify(dCast, thresholds.castMagnitude);
  const hue = classify(dHue, thresholds.skinHueShiftDeg, skinSignalActive);
  const chroma = classify(dChroma, thresholds.skinChromaShiftPct, skinSignalActive);

  const deltas: GradeRoundComparison['deltas'] = {
    castMagnitude: dCast,
    skinHueShiftDeg: dHue,
    skinChromaShiftPct: dChroma,
    ...(hasDefectCounts ? { defectCount: dDefect } : {}),
  };

  const reasons: string[] = [];
  let verdict: GradeRoundVerdict;

  if (hasDefectCounts && dDefect! > 0) {
    verdict = 'regression';
    reasons.push(`named-defect count increased (${previous.defectCount} -> ${current.defectCount})`);
  } else if (hasDefectCounts && dDefect! < 0 && cast !== 'worse' && hue !== 'worse' && chroma !== 'worse') {
    verdict = 'improvement';
    reasons.push(`named-defect count decreased (${previous.defectCount} -> ${current.defectCount})`);
  } else if (cast === 'worse') {
    if (!skinSignalActive) {
      verdict = 'inconclusive';
      reasons.push(
        `castMagnitude worsened (${dCast.toFixed(2)}) but skin presence is too low in one or both rounds to corroborate`,
      );
    } else if (hue === 'better' && chroma === 'better') {
      verdict = 'improvement';
      reasons.push('castMagnitude grew, but skin hue and chroma shift both improved enough to offset it');
    } else {
      verdict = 'regression';
      reasons.push(`castMagnitude worsened (${dCast.toFixed(2)}) with no clear compensating skin-protection improvement`);
      if (hue === 'worse') reasons.push(`skin hue shift also worsened (${dHue.toFixed(2)}deg)`);
      if (chroma === 'worse') reasons.push(`skin chroma shift also worsened (${dChroma.toFixed(2)}pp)`);
    }
  } else if (cast === 'better') {
    if (skinSignalActive && (hue === 'worse' || chroma === 'worse')) {
      verdict = 'regression';
      reasons.push('castMagnitude improved, but skin protection degraded');
      if (hue === 'worse') reasons.push(`skin hue shift worsened (${dHue.toFixed(2)}deg)`);
      if (chroma === 'worse') reasons.push(`skin chroma shift worsened (${dChroma.toFixed(2)}pp)`);
    } else {
      verdict = 'improvement';
      reasons.push(`castMagnitude improved (${dCast.toFixed(2)})`);
    }
  } else if (skinSignalActive && (hue === 'worse' || chroma === 'worse')) {
    verdict = 'regression';
    reasons.push('castMagnitude unchanged, but skin protection degraded');
    if (hue === 'worse') reasons.push(`skin hue shift worsened (${dHue.toFixed(2)}deg)`);
    if (chroma === 'worse') reasons.push(`skin chroma shift worsened (${dChroma.toFixed(2)}pp)`);
  } else if (skinSignalActive && hue === 'better' && chroma === 'better') {
    verdict = 'improvement';
    reasons.push('castMagnitude unchanged; skin protection improved');
  } else {
    verdict = 'inconclusive';
    reasons.push('no signal moved beyond noise thresholds');
  }

  return { verdict, reasons, deltas, skinSignalActive };
}

/**
 * Convenience over `classifyGradeRound` for an ordered sequence of rounds
 * (e.g. a full auto-grade loop's history): returns one comparison per
 * consecutive pair, `rounds[i-1]` -> `rounds[i]`.
 */
export function classifyGradeSequence(
  rounds: readonly GradeRoundSignal[],
  thresholds: GradeGuardThresholds = DEFAULT_GRADE_GUARD_THRESHOLDS,
): GradeRoundComparison[] {
  const out: GradeRoundComparison[] = [];
  for (let i = 1; i < rounds.length; i++) {
    out.push(classifyGradeRound(rounds[i - 1]!, rounds[i]!, thresholds));
  }
  return out;
}
