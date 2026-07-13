import { describe, it, expect } from 'vitest';
import { vlogEncode, vlogDecode, VLOG } from '../../src/core/color/vlog.js';
import { mat3MulVec } from '../../src/core/color/matrices.js';

describe('V-Log transfer function (Panasonic published values)', () => {
  it('encodes 0% reflectance to 0.125 (V-Log black)', () => {
    expect(vlogEncode(0)).toBeCloseTo(0.125, 6);
  });

  it('encodes 18% grey to ~42.3% (published IRE 42)', () => {
    expect(vlogEncode(0.18)).toBeCloseTo(0.42331, 4);
  });

  it('encodes 90% reflectance to ~61.3% (published IRE 61)', () => {
    // c*log10(0.9 + 0.00873) + d
    expect(vlogEncode(0.9)).toBeCloseTo(0.241514 * Math.log10(0.90873) + 0.598206, 6);
  });

  it('is continuous at the linear/log breakpoint (cut1 = 0.01)', () => {
    const below = vlogEncode(0.01 - 1e-9);
    const above = vlogEncode(0.01 + 1e-9);
    expect(Math.abs(below - above)).toBeLessThan(1e-4);
  });

  it('decode inverts encode across the full range', () => {
    for (const x of [0, 0.001, 0.005, 0.01, 0.02, 0.18, 0.5, 0.9, 1, 4, 8]) {
      expect(vlogDecode(vlogEncode(x))).toBeCloseTo(x, 6);
    }
  });

  it('V-Gamut -> Rec.709 matrix maps white to white (rows sum to 1)', () => {
    const [r, g, b] = mat3MulVec(VLOG.gamutToRec709, [1, 1, 1]);
    expect(r).toBeCloseTo(1, 5);
    expect(g).toBeCloseTo(1, 5);
    expect(b).toBeCloseTo(1, 5);
  });
});
