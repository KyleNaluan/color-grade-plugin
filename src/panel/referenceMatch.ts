/**
 * The panel-side glue for reference-image look matching (`data/cg-agents-study/report.md`
 * sec 1d): image picker -> `computeStats` on the reference still -> wrap as a `Theme`
 * (`themeFromReferenceStats`) -> the caller runs it through the existing
 * `applyThemeGrade` (`gradeStack.ts`), unchanged.
 *
 * A reference still is "just" a `Frame`: this reads it through the same `FrameSource`
 * abstraction every other pixel consumer uses (ADR 0001), so no new acquisition path
 * exists - a file-backed FrameSource pointed at the reference image (see
 * `fileFrameSource.ts`) works out of the box. `time` is unused by a still-image
 * FrameSource; `getFrame(0)` is the convention.
 */
import type { LogProfile } from '../core/color/types.js';
import { computeStats, decodeToRec709 } from '../core/analysis/stats.js';
import { themeFromReferenceStats, type ReferenceThemeOptions } from '../core/engine/referenceTheme.js';
import { downsampleFrame } from './analyze.js';
import type { FrameSource } from '../host/frameSource';
import type { Theme } from '../core/engine/theme.js';

/**
 * Build a look-matched `Theme` from a reference still, decoded with `profile`
 * (whatever log format the reference image itself is in - independent of the clip
 * being graded, which may use a different profile).
 */
export async function themeFromReferenceImage(
  frameSource: FrameSource,
  profile: LogProfile,
  opts?: ReferenceThemeOptions,
): Promise<Theme> {
  const source = await frameSource.getFrame(0);
  const frame = downsampleFrame(source);
  const decoded = decodeToRec709(frame.data, profile);
  return themeFromReferenceStats(computeStats(decoded), opts);
}
