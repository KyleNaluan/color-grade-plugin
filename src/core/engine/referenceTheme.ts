/**
 * Reference-image look matching ("match this look") - `data/cg-agents-study/report.md`
 * sec 1d: "a reference image is just a theme whose target stats are computed on the
 * spot from the dropped-in still." This wraps a reference still's measured
 * `FootageStats` as a `Theme` (the `statsOnlyTheme` pattern from the s7 Experiment B
 * prototype, `data/cg-agent-grade-s7/harness/lib.ts`), so it flows through the
 * existing `buildTransform`/`applyThemeGrade` grade path with zero new engine math.
 *
 * ARTIFACT RISK (read before wiring this into an unattended flow): s7's Experiment B
 * found raw stat transfer alone produces hue-noise blotches on near-neutral pixels
 * the reference frame never had (e.g. background highlights), independent of and NOT
 * fixed by the engine's automatic `toneStretchChromaGuard` - that guard only engages
 * when the tone-curve stretch (target/source luma range) is large; the Exp B blotch
 * came from the per-pixel LAB std-ratio gain amplifying small chroma noise on
 * near-neutral pixels, which can happen even at modest stretch. The fix s7 found was
 * authored tuning identical to the auto-grade-loop playbook (1a/1c): lower
 * `chromaGain`, add a `chromaShape.softLimit`, prefer explicit tints over raw stat
 * transfer for mood. `opts.overrides` below is the seam for that tuning - pass it
 * when you have reason to think the source content includes near-neutral highlights
 * or shadows prone to this (a human colorist's judgment call today; a candidate for
 * a future agent loop, report sec 1a/1f - not built here). Absent that, this
 * function's output is a strong STARTING POINT, not a guaranteed-clean grade: treat
 * it as round 0 of a tuning loop and sanity-check it with `gradeGuard.ts` (or a human
 * look) before trusting it blindly, exactly as 1f recommends for any auto-grade.
 */
import type { FootageStats } from '../analysis/stats.js';
import type { Theme, ThemeOverrides } from './theme.js';

export interface ReferenceThemeOptions {
  name?: string;
  description?: string;
  /** Theme.knobs.strength default (0-1). Defaults to 1 (full stat transfer). */
  strength?: number;
  /** Theme.knobs.skinProtection default (0-1). Defaults to 0.5. */
  skinProtection?: number;
  /**
   * Authored safety/mood overrides layered on top of the raw stat transfer - the
   * s7-recommended playbook for the artifact risk above (lower `chromaGain`, add
   * `chromaShape.softLimit`, explicit tints). Omit for the pure stats-only baseline.
   */
  overrides?: ThemeOverrides;
}

/**
 * Wrap a reference still's measured stats as a `Theme`. `matchStats` is left at its
 * default (true), so the engine's full automatic look - tone-curve stat match, LAB
 * mean/std transfer, per-band chroma scaling, AND the chroma-overshoot guard - applies
 * exactly as it would for any shipping theme; no reference-specific engine code exists
 * or is needed (see the module doc comment for what the guard does and does not cover).
 */
export function themeFromReferenceStats(stats: FootageStats, opts: ReferenceThemeOptions = {}): Theme {
  return {
    name: opts.name ?? 'reference-match',
    description:
      opts.description ?? 'Look matched from a reference image (stat transfer, no authored overrides).',
    targetStats: stats,
    ...(opts.overrides ? { overrides: opts.overrides } : {}),
    knobs: {
      strength: { default: opts.strength ?? 1 },
      skinProtection: { default: opts.skinProtection ?? 0.5 },
    },
  };
}
