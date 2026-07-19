/**
 * Cross-clip grade consistency: a rule-based pairwise comparator that flags
 * same-scene clips whose post-grade looks have diverged, using the same
 * `GradeImpact`-style numbers `gradeGuard.ts` already validated for
 * round-over-round QC - just compared clip-to-clip instead of round-to-round.
 *
 * Evidence/scope: `data/cg-agents-study/report.md` sec 1e. Batch apply itself
 * is unbuilt (`docs/prd.md:94`, Stretch); this is the standalone rule-based
 * primitive a future batch/consistency workflow would call, in the same
 * spirit as `gradeGuard.ts` for regression QC. No model involved - a model's
 * plausible value-add (describing *why* two grades look inconsistent to a
 * human) is a separate, narrower feature (sec 1e), not this comparator.
 */
import type { GradeImpact } from './gradeImpact.js';

/** One clip's post-grade summary: its identity plus the `GradeImpact` already computed for it. */
export interface ClipGradeSummary {
  clipId: string;
  impact: GradeImpact;
}

export interface CrossClipThresholds {
  /** LAB units; distance between the two clips' output mean color (`impact.outStats.lab.mean`). */
  outLabDeltaE: number;
  /** Normalized [0,1] luma; distance between the two clips' output midtone (p50) exposure. */
  exposureDeltaP50: number;
  /** LAB units; how differently the two grades' casts moved in magnitude (`|castMagnitude_a - castMagnitude_b|`). */
  castMagnitudeDelta: number;
  /** Degrees; circular difference in cast push direction, gated by `castDirectionMinMagnitude` on both clips. */
  castDirectionDeg: number;
  /** LAB units; below this, a clip's own castMagnitude is too close to neutral for its direction to mean anything. */
  castDirectionMinMagnitude: number;
  /** Degrees; delta of |skinHueShiftDeg| between the two clips' grades. */
  skinHueShiftDeg: number;
  /** Percentage points; delta of |skinChromaShiftPct| between the two clips' grades. */
  skinChromaShiftPct: number;
  /** Minimum skinWeightTotal, required in BOTH clips, for the skin signals to be trusted at all. */
  skinPresence: number;
}

/**
 * Thresholds chosen as a noise floor for a *cross-clip* comparison, which is
 * inherently noisier than `gradeGuard.ts`'s round-over-round comparison of the
 * SAME clip (different content, different framing, different skin coverage
 * between the two shots) - so each floor is set at roughly 2x the matching
 * `DEFAULT_GRADE_GUARD_THRESHOLDS` value, not reused verbatim.
 *
 * `outLabDeltaE` and `exposureDeltaP50` are the primary "these two shots don't
 * look like they belong in the same scene" signals - the actual post-grade
 * look. `outLabDeltaE`'s ceiling (6) sits well below `gradeImpact.ts`'s own
 * documented "'>15-20 starts reading as a strong cast" note, since two
 * same-scene clips should look near-identical, not merely both avoid a strong
 * cast individually. `castMagnitudeDelta`/`castDirectionDeg`/skin deltas are
 * secondary "these two grades are behaving differently" diagnostics that can
 * catch a mismatch before it fully shows up in the output look (e.g. two
 * clips pushed in opposite hue directions that happen to land near the same
 * LAB distance from neutral). `skinPresence` reuses the spike CLI's own
 * skin-presence-active cutoff (`scripts/spike.ts`, `skinPresence > 0.02`),
 * same as `gradeGuard.ts`.
 */
export const DEFAULT_CROSS_CLIP_THRESHOLDS: CrossClipThresholds = {
  outLabDeltaE: 6,
  exposureDeltaP50: 0.06,
  castMagnitudeDelta: 3,
  castDirectionDeg: 20,
  castDirectionMinMagnitude: 2,
  skinHueShiftDeg: 4,
  skinChromaShiftPct: 6,
  skinPresence: 0.02,
};

export type ClipConsistencyVerdict = 'consistent' | 'diverged';

export interface ClipPairComparison {
  clipA: string;
  clipB: string;
  verdict: ClipConsistencyVerdict;
  /** Human-readable explanation of which signals drove the verdict. */
  reasons: string[];
  deltas: {
    outLabDeltaE: number;
    exposureDeltaP50: number;
    castMagnitudeDelta: number;
    /** undefined when gated out (either clip's cast is too close to neutral to have a meaningful direction). */
    castDirectionDeltaDeg?: number;
    skinHueShiftDeltaDeg: number;
    skinChromaShiftDeltaPct: number;
  };
  /** Whether skinWeightTotal cleared `skinPresence` in both clips, i.e. whether the skin deltas above were trusted. */
  skinSignalActive: boolean;
}

function labDistance(a: readonly [number, number, number], b: readonly [number, number, number]): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

/** Smallest angle (deg, [0,180]) between two directions on a circle. */
function circularDeltaDeg(a: number, b: number): number {
  const d = Math.abs(a - b) % 360;
  return d > 180 ? 360 - d : d;
}

/**
 * Compares two clips' `GradeImpact`s and flags divergence beyond
 * `thresholds`. Symmetric in `a`/`b` except for reporting order.
 */
export function compareClipPair(
  a: ClipGradeSummary,
  b: ClipGradeSummary,
  thresholds: CrossClipThresholds = DEFAULT_CROSS_CLIP_THRESHOLDS,
): ClipPairComparison {
  const skinSignalActive = Math.min(a.impact.skinWeightTotal, b.impact.skinWeightTotal) >= thresholds.skinPresence;

  const outLabDeltaE = labDistance(a.impact.outStats.lab.mean, b.impact.outStats.lab.mean);
  const exposureDeltaP50 = Math.abs(a.impact.outStats.lumaPercentiles.p50 - b.impact.outStats.lumaPercentiles.p50);
  const castMagnitudeDelta = Math.abs(a.impact.castMagnitude - b.impact.castMagnitude);

  const directionMeaningful =
    a.impact.castMagnitude >= thresholds.castDirectionMinMagnitude &&
    b.impact.castMagnitude >= thresholds.castDirectionMinMagnitude;
  const castDirectionDeltaDeg = directionMeaningful
    ? circularDeltaDeg(a.impact.castDirectionDeg, b.impact.castDirectionDeg)
    : undefined;

  const skinHueShiftDeltaDeg = Math.abs(Math.abs(a.impact.skinHueShiftDeg) - Math.abs(b.impact.skinHueShiftDeg));
  const skinChromaShiftDeltaPct = Math.abs(
    Math.abs(a.impact.skinChromaShiftPct) - Math.abs(b.impact.skinChromaShiftPct),
  );

  const deltas: ClipPairComparison['deltas'] = {
    outLabDeltaE,
    exposureDeltaP50,
    castMagnitudeDelta,
    ...(castDirectionDeltaDeg !== undefined ? { castDirectionDeltaDeg } : {}),
    skinHueShiftDeltaDeg,
    skinChromaShiftDeltaPct,
  };

  const reasons: string[] = [];
  if (outLabDeltaE > thresholds.outLabDeltaE) {
    reasons.push(`output color diverges (${outLabDeltaE.toFixed(2)} LAB units apart)`);
  }
  if (exposureDeltaP50 > thresholds.exposureDeltaP50) {
    reasons.push(`output midtone exposure diverges (${exposureDeltaP50.toFixed(3)} apart)`);
  }
  if (castMagnitudeDelta > thresholds.castMagnitudeDelta) {
    reasons.push(`grades pushed color by different amounts (castMagnitude ${castMagnitudeDelta.toFixed(2)} apart)`);
  }
  if (castDirectionDeltaDeg !== undefined && castDirectionDeltaDeg > thresholds.castDirectionDeg) {
    reasons.push(`grades pushed color in different directions (${castDirectionDeltaDeg.toFixed(1)}deg apart)`);
  }
  if (skinSignalActive && skinHueShiftDeltaDeg > thresholds.skinHueShiftDeg) {
    reasons.push(`skin hue shift diverges (${skinHueShiftDeltaDeg.toFixed(2)}deg apart)`);
  }
  if (skinSignalActive && skinChromaShiftDeltaPct > thresholds.skinChromaShiftPct) {
    reasons.push(`skin chroma shift diverges (${skinChromaShiftDeltaPct.toFixed(2)}pp apart)`);
  }

  return {
    clipA: a.clipId,
    clipB: b.clipId,
    verdict: reasons.length > 0 ? 'diverged' : 'consistent',
    reasons,
    deltas,
    skinSignalActive,
  };
}

export interface SceneConsistencyReport {
  sceneId?: string;
  clipIds: string[];
  /** One comparison per unordered pair, in `clips` input order. */
  pairs: ClipPairComparison[];
  /** Subset of `pairs` with verdict 'diverged'. */
  diverged: ClipPairComparison[];
}

/**
 * Runs `compareClipPair` over every unordered pair in a same-scene clip group.
 * Two clips are enough to compare (one pair); more clips compare all C(n,2) pairs.
 */
export function checkSceneConsistency(
  clips: readonly ClipGradeSummary[],
  thresholds: CrossClipThresholds = DEFAULT_CROSS_CLIP_THRESHOLDS,
  sceneId?: string,
): SceneConsistencyReport {
  const pairs: ClipPairComparison[] = [];
  for (let i = 0; i < clips.length; i++) {
    for (let j = i + 1; j < clips.length; j++) {
      pairs.push(compareClipPair(clips[i]!, clips[j]!, thresholds));
    }
  }
  return {
    sceneId,
    clipIds: clips.map((c) => c.clipId),
    pairs,
    diverged: pairs.filter((p) => p.verdict === 'diverged'),
  };
}

/** Readable multi-line summary of a `SceneConsistencyReport`, for CLI/log output. */
export function formatSceneConsistencyReport(report: SceneConsistencyReport): string {
  const lines: string[] = [];
  const label = report.sceneId ? `scene "${report.sceneId}"` : 'scene';
  lines.push(`${label}: ${report.clipIds.length} clip(s), ${report.pairs.length} pair(s) compared`);
  if (report.pairs.length === 0) {
    lines.push('  (need at least 2 clips to compare)');
    return lines.join('\n');
  }
  if (report.diverged.length === 0) {
    lines.push('  all pairs consistent');
  } else {
    lines.push(`  ${report.diverged.length}/${report.pairs.length} pair(s) DIVERGED`);
  }
  for (const pair of report.pairs) {
    const mark = pair.verdict === 'diverged' ? 'DIVERGED' : 'ok';
    lines.push(`  [${mark}] ${pair.clipA} <-> ${pair.clipB}`);
    for (const reason of pair.reasons) lines.push(`      - ${reason}`);
  }
  return lines.join('\n');
}
