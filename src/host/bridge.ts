/**
 * The typed command boundary between the panel and the ExtendScript bridge.
 *
 * The bridge carries AE DOM queries only - zero color math. Panel logic
 * depends on the `Bridge` interface, never on CEP directly, so it can be
 * tested against a scripted fake.
 */
import type { RenderedFrameRef } from './frameSource';

/** What is selected in the active comp right now. */
export interface SelectionSnapshot {
  /** Name of the active comp, or null when no comp is active. */
  compName: string | null;
  /** Name of the selected layer, or null when zero layers are selected. */
  layerName: string | null;
  /**
   * Stable AE layer id of the selected layer, or null when zero layers are
   * selected. Unlike the name it survives renames and disambiguates
   * same-named layers, so it is the target identity threaded through
   * setCorrectProfile to guard against a mid-flight selection change.
   */
  layerId: number | null;
  /** How many layers are selected in the active comp (0 when no comp). */
  selectedCount: number;
}

/** Result of applying a Correct-stack profile change to the selected layer. */
export interface CorrectStackResult {
  /** Path the Decode LUT .cube was written to, or null when standard (no decode). */
  decodeLutPath: string | null;
}

/** Result of applying a grade to the active comp's Managed adjustment layer. */
export interface GradeResult {
  /** Path the grade .cube was written to in the Project-state folder. */
  gradeLutPath: string;
  /** Path the recipe-inputs JSON was written to alongside it. */
  recipePath: string;
}

export interface Bridge {
  /** Query the active comp and its selected layer. AE DOM read only. */
  getSelection(): Promise<SelectionSnapshot>;

  /**
   * The active comp's current-time indicator, in seconds, or null when no
   * comp is active. The panel uses this to analyze "the current frame".
   */
  getCurrentTime(): Promise<number | null>;

  /**
   * Render the active comp at `time` (seconds) to a temporary file and return
   * a reference to it. The ExtendScript side renders 16-bit TIFF as the
   * primary path and falls back to 8-bit PNG, reporting which it produced; the
   * caller reads the file back through a `FrameFileStore` and deletes it.
   * AE DOM / render-queue op only - no color math crosses this boundary.
   */
  renderFrame(time: number): Promise<RenderedFrameRef>;

  /**
   * Apply the Correct stack's V-Log/standard flag to the selected layer.
   * `isLog: true` writes `decodeLutCube` (a baked .cube's text, produced by
   * the panel via core's bakeDecodeLut) into the Project-state folder and
   * ensures Apply Color LUT then Lumetri, both `[cg]`-tagged, in that order.
   * `isLog: false` removes/disables the Decode LUT effect, leaving Lumetri.
   * AE DOM op only - the LUT bytes are computed entirely on the panel side.
   *
   * `targetLayerId` is the stable id of the layer the panel intended to mutate,
   * captured when the toggle fired. The ExtendScript side re-resolves the layer
   * by this id and refuses if the selection changed out from under the call, so
   * a slow bake/round-trip can never land the stack on a different clip.
   */
  setCorrectProfile(
    isLog: boolean,
    decodeLutCube: string | null,
    targetLayerId: number,
  ): Promise<CorrectStackResult>;

  /**
   * Apply a baked grade to the active comp. The panel has already analyzed the
   * post-Correct frame, built the transform toward the chosen Theme with its
   * default Knobs, and baked the grade into `gradeLutCube` (.cube text), with
   * `recipeJson` capturing the recipe inputs (theme, knobs, measured stats).
   * This writes both into the Project-state folder (named after the analyzed
   * clip) and ensures a single Managed (`[cg]`) adjustment layer at the top of
   * the comp carrying an Apply Color LUT effect pointed at the grade .cube.
   * AE DOM op only - all color math happened on the panel side, and the .cube
   * and recipe cross out-of-band as temp files, never inlined into evalScript.
   *
   * `analyzedLayerId` is the id of the clip whose frame was analyzed: it names
   * the persisted recipe/.cube and is re-resolved so the call refuses if that
   * clip vanished mid-flight.
   */
  applyGrade(
    gradeLutCube: string,
    recipeJson: string,
    analyzedLayerId: number,
  ): Promise<GradeResult>;

  /**
   * Enable or disable the single Managed (`[cg]`) grade adjustment layer.
   * Returns `true` when such a layer existed and was toggled, `false` when none
   * exists (a no-op, e.g. before the first grade). The panel disables the grade
   * layer around the analysis render so a re-grade measures post-Correct pixels
   * rather than pixels that already have the current grade's LUT applied. AE DOM
   * op only - no color math crosses this boundary.
   */
  setGradeLayerEnabled(enabled: boolean): Promise<boolean>;
}

/** Raised when the ExtendScript side returns an error or garbage. */
export class BridgeError extends Error {}

/**
 * Every ExtendScript entry point returns a JSON envelope:
 * `{"ok":true,"value":...}` or `{"ok":false,"error":"message"}`.
 */
export function parseBridgeResult<T>(raw: string): T {
  let envelope: unknown;
  try {
    envelope = JSON.parse(raw);
  } catch {
    throw new BridgeError(`Bridge returned non-JSON: ${raw}`);
  }
  if (typeof envelope !== 'object' || envelope === null || !('ok' in envelope)) {
    throw new BridgeError(`Bridge returned malformed envelope: ${raw}`);
  }
  const e = envelope as { ok: boolean; value?: T; error?: string };
  if (!e.ok) {
    throw new BridgeError(e.error ?? 'Unknown bridge error');
  }
  return e.value as T;
}
