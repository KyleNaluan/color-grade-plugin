/**
 * Translate an auto-grade result into the subset of edits the NATIVE editor can
 * actually apply to its recipe, plus the list of proposed knobs that have no
 * editor home (so nothing is ever silently dropped - issue cg-agent-wiring, DoD
 * item 5).
 *
 * The native editor composes agent edits onto the popup theme via dedicated USER
 * recipe fields (`native/ColorGradeFX/core/Recipe.h` applyEditorOverrides): user
 * tints ADD onto the theme's authored band tints, user curves REPLACE the theme's
 * authored curve per slot, and the Chroma Gain slider is a RELATIVE multiplier on
 * the theme's authored chromaGain (100% = authored). So to reconstruct the agent's
 * absolute intent through that composition we emit tint DELTAS (proposed - authored)
 * and a chromaGain RATIO (proposed / authored), and pass curves through absolutely.
 * Strength / Skin Protection are absolute params and pass straight through.
 *
 * softLimit / vibrance (chromaShape) have no editor control today, so a non-neutral
 * proposal for them is returned in `unmapped` for the panel to disclose rather than
 * being applied. See `native/docs/adr-agent-execution.md` (Auto-grade section).
 */
import type { Theme, ThemeOverrides } from '../core/engine/theme.js';
import type { AutoGradeParams } from './types.js';
import type { AgentApplyEdit } from './bridgeProtocol.js';

const EPS = 1e-6;

function flatCurve(pts: [number, number][]): number[] {
  const out: number[] = [];
  for (const [x, y] of pts) {
    out.push(x, y);
  }
  return out;
}

/** True if two curves are point-for-point equal (so an unchanged authored curve is not re-emitted). */
function curvesEqual(a: [number, number][] | undefined, b: [number, number][] | undefined): boolean {
  if (!a || !b) return !a && !b;
  if (a.length !== b.length) return false;
  return a.every((p, i) => Math.abs(p[0] - b[i]![0]) < EPS && Math.abs(p[1] - b[i]![1]) < EPS);
}

/** Non-zero if either component of the tint delta is meaningfully non-zero. */
function tintDelta(
  proposed: [number, number] | undefined,
  authored: [number, number] | undefined,
): [number, number] | null {
  if (!proposed) return null;
  const a = proposed[0] - (authored?.[0] ?? 0);
  const b = proposed[1] - (authored?.[1] ?? 0);
  if (Math.abs(a) < EPS && Math.abs(b) < EPS) return null;
  return [a, b];
}

export interface EditorApplyResult {
  apply: AgentApplyEdit[];
  unmapped: string[];
}

/**
 * Build the editor-applicable edits from the loop's best params.
 *
 * @param base       the theme the auto-grade ran against (its authored overrides
 *                   are the baseline the editor composition already applies).
 * @param bestParams the accepted best round's params.
 * @param baseParams the round-0 baseline params (so untouched strength/skin are
 *                   not needlessly rewritten).
 */
export function editorApplyFromResult(
  base: Theme,
  bestParams: AutoGradeParams,
  baseParams: AutoGradeParams,
): EditorApplyResult {
  const apply: AgentApplyEdit[] = [];
  const unmapped: string[] = [];
  const ov: ThemeOverrides = bestParams.overrides ?? {};
  const authored: ThemeOverrides = base.overrides ?? {};

  // Absolute scalar params: emit only when the loop moved them off the baseline.
  if (bestParams.strength !== undefined && Math.abs(bestParams.strength - (baseParams.strength ?? bestParams.strength)) > EPS) {
    apply.push({ field: 'strength', values: [bestParams.strength] });
  }
  if (
    bestParams.skinProtection !== undefined &&
    Math.abs(bestParams.skinProtection - (baseParams.skinProtection ?? bestParams.skinProtection)) > EPS
  ) {
    apply.push({ field: 'skinProtection', values: [bestParams.skinProtection] });
  }

  // Chroma gain: the slider is a ratio on the theme's authored gain.
  if (ov.chromaGain !== undefined) {
    const authoredGain = authored.chromaGain ?? 1;
    const ratio = authoredGain > EPS ? ov.chromaGain / authoredGain : ov.chromaGain;
    if (Math.abs(ratio - 1) > EPS) apply.push({ field: 'chromaGain', values: [ratio] });
  }

  // Band tints: user fields ADD onto the theme, so emit the delta.
  const st = tintDelta(ov.shadowTint, authored.shadowTint);
  if (st) apply.push({ field: 'shadowTint', values: st });
  const mt = tintDelta(ov.midtoneTint, authored.midtoneTint);
  if (mt) apply.push({ field: 'midtoneTint', values: mt });
  const ht = tintDelta(ov.highlightTint, authored.highlightTint);
  if (ht) apply.push({ field: 'highlightTint', values: ht });

  // Curves: user curve REPLACES the theme's per-slot curve, so pass absolute - but only
  // when the agent genuinely changed it, else we'd needlessly rewrite the theme's own
  // authored curve into the user slot on a baseline-kept round.
  if (ov.toneCurve && ov.toneCurve.length >= 2 && !curvesEqual(ov.toneCurve, authored.toneCurve))
    apply.push({ field: 'toneCurve', values: flatCurve(ov.toneCurve) });
  if (ov.channelCurves?.r && ov.channelCurves.r.length >= 2 && !curvesEqual(ov.channelCurves.r, authored.channelCurves?.r))
    apply.push({ field: 'channelR', values: flatCurve(ov.channelCurves.r) });
  if (ov.channelCurves?.g && ov.channelCurves.g.length >= 2 && !curvesEqual(ov.channelCurves.g, authored.channelCurves?.g))
    apply.push({ field: 'channelG', values: flatCurve(ov.channelCurves.g) });
  if (ov.channelCurves?.b && ov.channelCurves.b.length >= 2 && !curvesEqual(ov.channelCurves.b, authored.channelCurves?.b))
    apply.push({ field: 'channelB', values: flatCurve(ov.channelCurves.b) });

  // No editor home: disclose instead of dropping - but only when the agent changed them
  // from whatever the theme authored (a baseline-kept round discloses nothing).
  if (
    ov.chromaShape?.softLimit !== undefined &&
    ov.chromaShape.softLimit !== authored.chromaShape?.softLimit
  ) {
    unmapped.push(`chromaShape.softLimit=${ov.chromaShape.softLimit}`);
  }
  if (
    ov.chromaShape?.vibrance !== undefined &&
    ov.chromaShape.vibrance !== (authored.chromaShape?.vibrance ?? 0) &&
    Math.abs(ov.chromaShape.vibrance) > EPS
  ) {
    unmapped.push(`chromaShape.vibrance=${ov.chromaShape.vibrance}`);
  }

  return { apply, unmapped };
}
