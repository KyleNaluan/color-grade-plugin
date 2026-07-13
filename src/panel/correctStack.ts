/**
 * The Correct tab's V-Log/standard toggle: manual only, no metadata
 * auto-detection (issue #5). Flagging V-Log bakes a Decode LUT from the
 * chosen Log profile's decode() (core, pure) and hands its .cube text to the
 * bridge, which writes it into the Project-state folder and builds the
 * Decode LUT -> Lumetri Correct stack, both Managed ([cg]) effects.
 * Flagging standard clears the LUT text so the bridge removes the Decode LUT.
 *
 * This is pure panel logic over the `Bridge` interface (no CEP), so it is
 * tested against a scripted fake bridge.
 */
import type { LogProfile } from '../core/color/types.js';
import { bakeDecodeLut } from '../core/lut/decodeLut.js';
import { writeCube } from '../core/lut/cube.js';
import type { Bridge, CorrectStackResult } from '../host/bridge';

/**
 * Apply the given V-Log/standard flag to the selected layer's Correct stack.
 * `logProfile` is only consulted (and only baked into a LUT) when `isLog`.
 * `targetLayerId` is the stable id of the layer the caller intends to mutate,
 * threaded through so the bridge can refuse if the selection moved mid-flight.
 */
export async function setCorrectProfile(
  bridge: Pick<Bridge, 'setCorrectProfile'>,
  isLog: boolean,
  logProfile: LogProfile,
  targetLayerId: number,
): Promise<CorrectStackResult> {
  if (!isLog) return bridge.setCorrectProfile(false, null, targetLayerId);
  const cubeText = writeCube(bakeDecodeLut(logProfile));
  return bridge.setCorrectProfile(true, cubeText, targetLayerId);
}
