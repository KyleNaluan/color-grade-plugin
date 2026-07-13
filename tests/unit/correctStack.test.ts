import { describe, expect, it, vi } from 'vitest';
import { setCorrectProfile } from '../../src/panel/correctStack';
import { bakeDecodeLut } from '../../src/core/lut/decodeLut.js';
import { writeCube } from '../../src/core/lut/cube.js';
import { VLOG } from '../../src/core/color/vlog.js';
import type { Bridge, CorrectStackResult } from '../../src/host/bridge';

type CorrectBridge = Pick<Bridge, 'setCorrectProfile'>;

function fakeBridge(result: CorrectStackResult): CorrectBridge & {
  calls: Array<[boolean, string | null, number]>;
} {
  const calls: Array<[boolean, string | null, number]> = [];
  return {
    calls,
    async setCorrectProfile(isLog, decodeLutCube, targetLayerId) {
      calls.push([isLog, decodeLutCube, targetLayerId]);
      return result;
    },
  };
}

/**
 * A bridge that mirrors the ExtendScript guard: it mutates only the currently
 * selected layer and refuses when the caller's target id no longer matches.
 */
function selectionGuardedBridge(selectedLayerId: number): CorrectBridge {
  return {
    async setCorrectProfile(_isLog, _decodeLutCube, targetLayerId) {
      if (targetLayerId !== selectedLayerId) {
        throw new Error('selection changed before the Correct stack could be applied - try again');
      }
      return { decodeLutPath: null };
    },
  };
}

describe('setCorrectProfile (panel logic against a fake bridge)', () => {
  it('flagging V-Log bakes the Decode LUT from the profile and sends it to the bridge', async () => {
    const bridge = fakeBridge({ decodeLutPath: '/proj/.colorgrade/decode_1.cube' });
    const result = await setCorrectProfile(bridge, true, VLOG, 42);

    expect(bridge.calls).toHaveLength(1);
    const [isLog, cubeText, targetLayerId] = bridge.calls[0]!;
    expect(isLog).toBe(true);
    expect(cubeText).toBe(writeCube(bakeDecodeLut(VLOG)));
    expect(targetLayerId).toBe(42);
    expect(result).toEqual({ decodeLutPath: '/proj/.colorgrade/decode_1.cube' });
  });

  it('flagging standard sends null - no LUT baked or written', async () => {
    const bridge = fakeBridge({ decodeLutPath: null });
    const result = await setCorrectProfile(bridge, false, VLOG, 42);

    expect(bridge.calls).toEqual([[false, null, 42]]);
    expect(result).toEqual({ decodeLutPath: null });
  });

  it('applies to the layer whose id still matches the captured target', async () => {
    const bridge = selectionGuardedBridge(7);
    await expect(setCorrectProfile(bridge, true, VLOG, 7)).resolves.toEqual({ decodeLutPath: null });
  });

  it('refuses (does not silently mutate) when the selection moved off the target', async () => {
    const bridge = selectionGuardedBridge(7);
    await expect(setCorrectProfile(bridge, true, VLOG, 9)).rejects.toThrow(/selection changed/);
  });

  it('propagates bridge failures (e.g. unsaved project) without swallowing them', async () => {
    const bridge: CorrectBridge = {
      setCorrectProfile: vi.fn(async () => {
        throw new Error('save the project before flagging V-Log clips');
      }),
    };
    await expect(setCorrectProfile(bridge, true, VLOG, 42)).rejects.toThrow(/save the project/);
  });
});
