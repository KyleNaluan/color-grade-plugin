/**
 * The Grade tab's "apply theme" action: analyze the selected clip's current
 * frame AFTER the Correct stack, build the transform toward the chosen Theme
 * with its default Knobs, bake it into a grade .cube, and hand the .cube plus
 * its recipe inputs to the bridge, which persists both in the Project-state
 * folder and drops the grade onto a Managed (`[cg]`) adjustment layer via Apply
 * Color LUT.
 *
 * Pixel access goes through FrameSource, and the rendered comp frame has
 * already run through the clip's Correct stack (Decode LUT + Lumetri), so the
 * analysis measures post-decode Rec.709 - exactly the engine's input domain -
 * and applies no further software decode (it decodes through the identity
 * Rec.709 profile). This is the hard "analysis after the Correct stack"
 * requirement of the pipeline order.
 *
 * Pure panel logic over the `Bridge` + `FrameSource` interfaces (no CEP), so it
 * is tested against a fake bridge and a file-backed FrameSource fed fixtures.
 */
import { REC709 } from '../core/color/rec709.js';
import { writeCube } from '../core/lut/cube.js';
import { bakeGradeLut } from '../core/lut/gradeLut.js';
import type { FootageStats } from '../core/analysis/stats.js';
import type { Theme } from '../core/engine/theme.js';
import type { Bridge, GradeResult } from '../host/bridge';
import type { FrameSource } from '../host/frameSource';
import { analyzeCurrentFrame, type AnalyzeResult } from './analyze';

/**
 * The reproducible inputs to a baked grade, persisted alongside the .cube so a
 * later session can rebuild or re-tune it without re-analyzing (issue #11).
 */
export interface GradeRecipe {
  version: 1;
  /** The Theme applied (its `name`, which is also its THEMES key). */
  theme: string;
  /** The Knob values used - defaults for the tracer; user-set once knobs land. */
  knobs: { strength: number; skinProtection: number };
  /** Comp time (seconds) whose frame was analyzed. */
  analyzedTime: number;
  /**
   * The decode profile the analysis assumed. The tracer measures the post-
   * Correct render directly, so this is always Rec.709 (identity) - recorded to
   * make the post-decode contract explicit in the persisted recipe.
   */
  analyzedProfile: string;
  /** The measured Footage stats the transform was built from. */
  stats: FootageStats;
}

export interface GradeApplication {
  analysis: AnalyzeResult;
  recipe: GradeRecipe;
  result: GradeResult;
}

/** Assemble the persisted recipe inputs for a grade of `theme` from `analysis`. */
export function buildGradeRecipe(theme: Theme, analysis: AnalyzeResult): GradeRecipe {
  return {
    version: 1,
    theme: theme.name,
    knobs: {
      strength: theme.knobs.strength.default,
      skinProtection: theme.knobs.skinProtection.default,
    },
    analyzedTime: analysis.time,
    analyzedProfile: analysis.profileName,
    stats: analysis.stats,
  };
}

/**
 * Auto-grade the active comp's selected clip toward `theme` with default Knobs.
 * `analyzedLayerId` is the clip the caller intends to grade, threaded through so
 * the bridge can refuse if that clip vanished mid-flight. Throws if no comp is
 * active, the frame cannot be acquired, or the bridge rejects.
 */
export async function applyThemeGrade(
  bridge: Pick<Bridge, 'getCurrentTime' | 'applyGrade' | 'setGradeLayerEnabled'>,
  frameSource: FrameSource,
  theme: Theme,
  analyzedLayerId: number,
): Promise<GradeApplication> {
  // Disable any existing Managed [cg] grade layer so the analysis render
  // measures post-Correct pixels, not pixels the current grade already altered.
  // The finally re-enables it even if analysis throws.
  const hadGradeLayer = await bridge.setGradeLayerEnabled(false);
  let analysis: AnalyzeResult;
  try {
    // Identity (Rec.709) decode: the rendered frame is already post-Correct, so
    // measuring it directly gives post-decode stats without a second decode.
    analysis = await analyzeCurrentFrame(bridge, frameSource, REC709);
  } finally {
    if (hadGradeLayer) await bridge.setGradeLayerEnabled(true);
  }
  const cubeText = writeCube(bakeGradeLut(analysis.stats, theme));
  const recipe = buildGradeRecipe(theme, analysis);
  const result = await bridge.applyGrade(cubeText, JSON.stringify(recipe, null, 2), analyzedLayerId);
  return { analysis, recipe, result };
}
