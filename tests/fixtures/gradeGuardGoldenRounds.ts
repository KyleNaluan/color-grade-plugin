/**
 * Committed GradeImpact numbers for the s7 golden-round scout
 * (`data/cg-agent-grade-s7/harness/rounds/*.json`), used to regression-test
 * the QC guard (`src/core/analysis/gradeGuard.ts`) against a real recorded
 * defect, not synthetic numbers.
 *
 * Computed once via `computeGradeImpact` (`src/core/analysis/gradeImpact.ts`)
 * replaying the exact harness (`data/cg-agent-grade-s7/harness/{lib,expA,expB}.ts`)
 * against the local-only fixture footage at `tests/fixtures/frames/` (gitignored
 * personal footage, per `CLAUDE.md` - not available in every checkout, which is
 * why the numbers are committed here instead of recomputed at test time).
 *
 * Exp B (shot match, `vlog-portait-...tif` graded toward `vlog-cave-warm.tif`):
 * round 2 fixed the chroma-noise blotches the baseline/round1 had; round 3
 * pushed the same knobs harder and reintroduced them, even though round 3's
 * raw stats sit closer to the reference target than round 2's (`report.md`,
 * "Experiment B" section) - the exact case every vision model tested in
 * `data/cg-agents-study/report.md` sec 2a failed to catch (0/16 calls).
 *
 * Exp A (correction, `rec709-outdoor-with-skin-and-overexposed-sky.tif`
 * synthetically degraded then corrected): baseline through round5 is a
 * genuine, monotonic improvement (report.md, "Experiment A" section) with no
 * skin detected in-frame throughout (the degraded cast collapses skin chroma
 * below `skinWedgeWeight`'s threshold - a real, documented finding, not a
 * bug in this fixture).
 */
import type { GradeRoundSignal } from '../../src/core/analysis/gradeGuard.js';

export const expBRounds: Record<'baseline' | 'round1' | 'round2' | 'round3', GradeRoundSignal> = {
  baseline: {
    castMagnitude: 16.760847203996978,
    skinHueShiftDeg: 5.3627455999254385,
    skinChromaShiftPct: 40.565387897686044,
    skinWeightTotal: 0.16757484252354565,
  },
  round1: {
    castMagnitude: 13.433827898120885,
    skinHueShiftDeg: 2.2427062578604766,
    skinChromaShiftPct: 12.855070455475406,
    skinWeightTotal: 0.16757484252354565,
  },
  round2: {
    castMagnitude: 8.571822651074243,
    skinHueShiftDeg: 2.276190083853529,
    skinChromaShiftPct: -1.5055707535118708,
    skinWeightTotal: 0.16757484252354565,
  },
  round3: {
    castMagnitude: 11.521605399090594,
    skinHueShiftDeg: 2.7486050799951247,
    skinChromaShiftPct: -0.3165450028278177,
    skinWeightTotal: 0.16757484252354565,
  },
};

export const expARounds: Record<'baseline' | 'round1' | 'round4' | 'round5', GradeRoundSignal> = {
  baseline: {
    castMagnitude: 5.860807263033977,
    skinHueShiftDeg: 0,
    skinChromaShiftPct: 0,
    skinWeightTotal: 0,
  },
  round1: {
    castMagnitude: 11.305361139538608,
    skinHueShiftDeg: 0,
    skinChromaShiftPct: 0,
    skinWeightTotal: 0,
  },
  round4: {
    castMagnitude: 15.87394110328176,
    skinHueShiftDeg: 0,
    skinChromaShiftPct: 0,
    skinWeightTotal: 0,
  },
  round5: {
    castMagnitude: 16.54470258206294,
    skinHueShiftDeg: 0,
    skinChromaShiftPct: 0,
    skinWeightTotal: 0,
  },
};
