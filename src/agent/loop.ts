/**
 * The auto-grade loop: render -> critic NAMES defects + proposes params ->
 * re-render -> RULES (the QC guard) decide better/worse/stop. Max 4-6 rounds
 * (s7 convergence evidence). This is the orchestration half; the model's role
 * is strictly naming/proposing, never judging (see `critic.ts` header and
 * `data/cg-agents-study/report.md` sec 1f).
 *
 * The loop is pure and dependency-injected: `render` (build the transform,
 * measure grade impact, produce a frame) and `critique` (the vision call) are
 * supplied by the caller. That keeps the decision logic - which is the load-
 * bearing part - fully unit-testable with canned renders/critiques, no API and
 * no pixels required (see `tests/unit/agentLoop.test.ts`, which replays the s7
 * golden rounds through it).
 */
import {
  classifyGradeRound,
  DEFAULT_GRADE_GUARD_THRESHOLDS,
  type GradeGuardThresholds,
  type GradeRoundComparison,
  type GradeRoundSignal,
} from '../core/analysis/gradeGuard.js';
import type { GradeImpact } from '../core/analysis/gradeImpact.js';
import type { ThemeOverrides } from '../core/engine/theme.js';
import type { CriticProposal } from './critic.js';
import type { AutoGradeParams } from './types.js';

/** Everything one round produces: the params tried, the measured impact, and the frame the critic looked at. */
export interface RenderedRound<Frame> {
  params: AutoGradeParams;
  impact: GradeImpact;
  frame: Frame;
}

export interface LoopDeps<Frame> {
  /** Build+apply the transform for a parameter set, measure its grade impact, and yield a frame for the critic. */
  render(params: AutoGradeParams): Promise<RenderedRound<Frame>> | RenderedRound<Frame>;
  /** The vision critic: names defects in `round.frame` and proposes the next params. */
  critique(round: RenderedRound<Frame>): Promise<CriticProposal> | CriticProposal;
}

export interface LoopConfig {
  /** Starting parameters (the base theme's knob defaults + any seed overrides). */
  baseParams: AutoGradeParams;
  /** Max tuned rounds after the baseline. 4-6 per s7. */
  maxRounds: number;
  thresholds?: GradeGuardThresholds;
}

export interface LoopRoundRecord<Frame> {
  /** 0 = baseline; 1..maxRounds = tuned rounds. */
  round: number;
  params: AutoGradeParams;
  impact: GradeImpact;
  /** The critic's read of THIS round's frame (defects + the proposal used for the NEXT round). */
  proposal: CriticProposal;
  /** Guard comparison of this round vs the previous accepted best (undefined for the baseline). */
  comparison?: GradeRoundComparison;
  /** Whether the guard accepted this round as the new best (false = rejected regression). */
  accepted: boolean;
  frame: Frame;
}

export interface LoopResult<Frame> {
  rounds: LoopRoundRecord<Frame>[];
  /** The accepted round with the best (last non-regressing) result. */
  best: LoopRoundRecord<Frame>;
  stopReason: string;
}

/** Map a measured grade impact + the critic's named-defect count into the guard's signal shape. */
export function signalFromImpact(impact: GradeImpact, defectCount: number): GradeRoundSignal {
  return {
    castMagnitude: impact.castMagnitude,
    skinHueShiftDeg: impact.skinHueShiftDeg,
    skinChromaShiftPct: impact.skinChromaShiftPct,
    skinWeightTotal: impact.skinWeightTotal,
    defectCount,
  };
}

/** Merge a critic proposal onto the current params. Proposal fields replace; unspecified fields stay. */
export function applyProposal(current: AutoGradeParams, proposal: CriticProposal): AutoGradeParams {
  const merged: AutoGradeParams = {
    strength: proposal.proposedOpts.strength ?? current.strength,
    skinProtection: proposal.proposedOpts.skinProtection ?? current.skinProtection,
    overrides: mergeOverrides(current.overrides, proposal.proposedOverrides),
  };
  return merged;
}

function mergeOverrides(cur: ThemeOverrides, next: ThemeOverrides): ThemeOverrides {
  const out: ThemeOverrides = { ...cur, ...next };
  if (cur.chromaShape || next.chromaShape) {
    out.chromaShape = { ...cur.chromaShape, ...next.chromaShape };
  }
  if (cur.channelCurves || next.channelCurves) {
    out.channelCurves = { ...cur.channelCurves, ...next.channelCurves };
  }
  return out;
}

/**
 * Run the loop. Each round: apply the previous round's critic proposal, render,
 * critique the new frame (getting both its named-defect count and the next
 * proposal), then let the guard compare it to the current best. A guard
 * `regression` rejects the round and stops (keeping the previous best - pushing
 * past a regression is the exact failure s7's Exp B round 3 documents). The
 * critic's own continue/stop is only advisory: the loop stops on it only when
 * the guard also sees no remaining improvement.
 */
export async function runAutoGradeLoop<Frame>(
  deps: LoopDeps<Frame>,
  config: LoopConfig,
): Promise<LoopResult<Frame>> {
  const thresholds = config.thresholds ?? DEFAULT_GRADE_GUARD_THRESHOLDS;
  const rounds: LoopRoundRecord<Frame>[] = [];

  // Baseline (round 0): the pure theme grade, critiqued but never guard-compared.
  const r0 = await deps.render(config.baseParams);
  const p0 = await deps.critique(r0);
  const baseline: LoopRoundRecord<Frame> = {
    round: 0,
    params: config.baseParams,
    impact: r0.impact,
    proposal: p0,
    accepted: true,
    frame: r0.frame,
  };
  rounds.push(baseline);
  let best = baseline;
  let stopReason = `reached max rounds (${config.maxRounds})`;

  for (let round = 1; round <= config.maxRounds; round++) {
    const candidateParams = applyProposal(best.params, best.proposal);
    const r = await deps.render(candidateParams);
    const p = await deps.critique(r);

    const comparison = classifyGradeRound(
      signalFromImpact(best.impact, best.proposal.defects.length),
      signalFromImpact(r.impact, p.defects.length),
      thresholds,
    );
    const accepted = comparison.verdict !== 'regression';
    const record: LoopRoundRecord<Frame> = {
      round,
      params: candidateParams,
      impact: r.impact,
      proposal: p,
      comparison,
      accepted,
      frame: r.frame,
    };
    rounds.push(record);

    if (!accepted) {
      stopReason = `guard rejected round ${round} as a regression; kept round ${best.round}: ${comparison.reasons.join('; ')}`;
      break;
    }

    best = record;

    // Rules-owned stop conditions. The critic's stop is a hint, honored only
    // when the guard agrees there's no remaining improvement to capture.
    if (p.defects.length === 0 && comparison.verdict !== 'improvement') {
      stopReason = `converged: no named defects and no further measurable improvement at round ${round}`;
      break;
    }
    if (p.verdict === 'stop' && comparison.verdict !== 'improvement') {
      stopReason = `critic signalled stop and the guard sees no remaining improvement at round ${round}`;
      break;
    }
  }

  return { rounds, best, stopReason };
}
