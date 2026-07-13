import { describe, expect, it, vi } from 'vitest';
import { setCorrectProfile } from '../../src/panel/correctStack';
import { bakeDecodeLut } from '../../src/core/lut/decodeLut.js';
import { writeCube } from '../../src/core/lut/cube.js';
import { VLOG } from '../../src/core/color/vlog.js';
import type { Bridge, CorrectStackResult } from '../../src/host/bridge';

type CorrectBridge = Pick<Bridge, 'setCorrectProfile'>;

function fakeBridge(result: CorrectStackResult): CorrectBridge & {
  calls: Array<[boolean, string | null]>;
} {
  const calls: Array<[boolean, string | null]> = [];
  return {
    calls,
    async setCorrectProfile(isLog, decodeLutCube) {
      calls.push([isLog, decodeLutCube]);
      return result;
    },
  };
}

describe('setCorrectProfile (panel logic against a fake bridge)', () => {
  it('flagging V-Log bakes the Decode LUT from the profile and sends it to the bridge', async () => {
    const bridge = fakeBridge({ decodeLutPath: '/proj/.colorgrade/decode_1.cube' });
    const result = await setCorrectProfile(bridge, true, VLOG);

    expect(bridge.calls).toHaveLength(1);
    const [isLog, cubeText] = bridge.calls[0]!;
    expect(isLog).toBe(true);
    expect(cubeText).toBe(writeCube(bakeDecodeLut(VLOG)));
    expect(result).toEqual({ decodeLutPath: '/proj/.colorgrade/decode_1.cube' });
  });

  it('flagging standard sends null - no LUT baked or written', async () => {
    const bridge = fakeBridge({ decodeLutPath: null });
    const result = await setCorrectProfile(bridge, false, VLOG);

    expect(bridge.calls).toEqual([[false, null]]);
    expect(result).toEqual({ decodeLutPath: null });
  });

  it('propagates bridge failures (e.g. unsaved project) without swallowing them', async () => {
    const bridge: CorrectBridge = {
      setCorrectProfile: vi.fn(async () => {
        throw new Error('save the project before flagging V-Log clips');
      }),
    };
    await expect(setCorrectProfile(bridge, true, VLOG)).rejects.toThrow(/save the project/);
  });
});
