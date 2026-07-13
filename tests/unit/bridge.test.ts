import { describe, expect, it } from 'vitest';
import { BridgeError, parseBridgeResult } from '../../src/host/bridge';

describe('parseBridgeResult', () => {
  it('unwraps a success envelope', () => {
    expect(
      parseBridgeResult('{"ok":true,"value":{"compName":"Main","layerName":"A","selectedCount":1}}'),
    ).toEqual({ compName: 'Main', layerName: 'A', selectedCount: 1 });
  });

  it('throws BridgeError with the message from an error envelope', () => {
    expect(() => parseBridgeResult('{"ok":false,"error":"no project open"}')).toThrowError(
      new BridgeError('no project open'),
    );
  });

  it('throws BridgeError on non-JSON (e.g. ExtendScript stack dump)', () => {
    expect(() => parseBridgeResult('undefined')).toThrow(BridgeError);
  });

  it('throws BridgeError on JSON without an envelope', () => {
    expect(() => parseBridgeResult('42')).toThrow(BridgeError);
    expect(() => parseBridgeResult('{"value":1}')).toThrow(BridgeError);
  });
});
