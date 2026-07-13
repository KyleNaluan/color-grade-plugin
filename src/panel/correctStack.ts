/**
 * The Correct tab's footage-profile selector: manual only, no metadata
 * auto-detection (issue #5). The one selected profile is the single source of
 * truth for the clip's log format and drives BOTH the analyze decode and the
 * Correct stack. Selecting a log profile bakes a Decode LUT from that profile's
 * decode() (core, pure) and hands its .cube text to the bridge, which writes it
 * into the Project-state folder and builds the Decode LUT -> Lumetri Correct
 * stack, both Managed ([cg]) effects. Selecting standard/Rec.709 clears the LUT
 * text so the bridge removes the Decode LUT.
 *
 * This is pure panel logic over the `Bridge` interface (no CEP), so it is
 * tested against a scripted fake bridge.
 */
import type { LogProfile } from '../core/color/types.js';
import { PROFILES, isLogProfile } from '../core/color/index.js';
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

/**
 * Apply the Correct stack for a selected footage-profile key (the unified
 * selector's single source of truth). Resolves the key to its `LogProfile` and
 * decides log-vs-standard via `isLogProfile`, so the panel never hardcodes a
 * profile in the apply path: a log key bakes and applies that profile's Decode
 * LUT; the standard key removes the Correct Decode LUT.
 */
export async function applyCorrectForProfile(
  bridge: Pick<Bridge, 'setCorrectProfile'>,
  profileKey: string,
  targetLayerId: number,
): Promise<CorrectStackResult> {
  const profile = PROFILES[profileKey];
  if (!profile) throw new Error(`unknown footage profile: ${profileKey}`);
  return setCorrectProfile(bridge, isLogProfile(profileKey), profile, targetLayerId);
}
