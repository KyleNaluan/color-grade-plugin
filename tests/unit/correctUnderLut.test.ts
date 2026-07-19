import { describe, it, expect } from 'vitest';
import {
  applyManualPrimaries,
  buildManualPrimaries,
  NEUTRAL_MANUAL,
  NEUTRAL_LGG,
  type ManualGrade,
  type LiftGammaGain,
} from '../../src/core/engine/engine.js';
import { bakeLut, sampleLut, type Lut3D } from '../../src/core/lut/cube.js';
import { decodePixelToRec709 } from '../../src/core/color/decode.js';
import { VLOG } from '../../src/core/color/vlog.js';
import { REC709 } from '../../src/core/color/rec709.js';
import type { Vec3 } from '../../src/core/color/types.js';

// A stand-in "user creative LUT": an arbitrary invertible-ish channel twist. This
// plays the role of the external .cube the user drops in; the feature under test
// inserts the Correct (decode) + Basics (manual primaries) stage *under* it.
const userLut: Lut3D = bakeLut(
  ([r, g, b]) => [r ** 0.8, g * 0.9 + 0.05, b ** 1.1] as Vec3,
  17,
  'user-creative',
);

// Deterministic pseudo-random grid of encoded input pixels.
function samples(seed0: number, n = 300): Vec3[] {
  let seed = seed0;
  const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
  return Array.from({ length: n }, () => [rand(), rand(), rand()] as Vec3);
}

// The exact reference composition the feature must reproduce, computed from the
// primitives independently of any bake helper: decode -> correct/basics -> userLut.
function reference(x: Vec3, manual?: ManualGrade, lgg?: LiftGammaGain): Vec3 {
  const decoded = decodePixelToRec709(x, VLOG);
  const corrected = applyManualPrimaries(decoded, manual, lgg);
  return sampleLut(userLut, corrected);
}

describe('applyManualPrimaries', () => {
  it('is exact identity when manual + LGG are neutral', () => {
    for (const x of samples(1, 100)) {
      const out = applyManualPrimaries(x, NEUTRAL_MANUAL, NEUTRAL_LGG);
      expect(out[0]).toBe(x[0]);
      expect(out[1]).toBe(x[1]);
      expect(out[2]).toBe(x[2]);
    }
  });

  it('is exact identity when both stages are omitted', () => {
    for (const x of samples(2, 100)) {
      const out = applyManualPrimaries(x);
      expect(out).toEqual([x[0], x[1], x[2]]);
    }
  });

  it('buildManualPrimaries.active reflects whether the stage does anything', () => {
    expect(buildManualPrimaries(NEUTRAL_MANUAL, NEUTRAL_LGG).active).toBe(false);
    expect(buildManualPrimaries().active).toBe(false);
    expect(buildManualPrimaries({ ...NEUTRAL_MANUAL, exposure: 0.5 }).active).toBe(true);
    expect(
      buildManualPrimaries(undefined, { ...NEUTRAL_LGG, gain: [1.1, 1, 1] }).active,
    ).toBe(true);
  });
});

describe('Correct/Basics under a user LUT (decode -> correct/basics -> userLut)', () => {
  const manual: ManualGrade = {
    ...NEUTRAL_MANUAL,
    exposure: -0.7,
    contrast: 35,
    shadows: 40,
    temperature: 30,
    saturation: 1.2,
  };
  const lgg: LiftGammaGain = { lift: [0.02, 0, -0.01], gamma: [1.0, 0.95, 1.0], gain: [1.05, 1, 1] };

  it('the baked single-sample composite equals the decode->basics->userLut reference', () => {
    // The shipped LUT is ONE composite: bake `userLut(basics(decode(grid)))` so the
    // render path samples it once per pixel (single-trilinear-sample invariant). At the
    // 33^3 grid nodes the bake is exact, so the composite reproduces the reference chain
    // there; between nodes only trilinear interpolation (as with any 3D LUT) separates them.
    const size = 33;
    const primaries = buildManualPrimaries(manual, lgg);
    const composite = bakeLut((x) => sampleLut(userLut, primaries.apply(decodePixelToRec709(x, VLOG))), size);
    const n = size - 1;
    let seed = 3;
    const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
    for (let k = 0; k < 300; k++) {
      const x: Vec3 = [
        Math.floor(rand() * size) / n,
        Math.floor(rand() * size) / n,
        Math.floor(rand() * size) / n,
      ];
      const viaComposite = sampleLut(composite, x);
      const ref = reference(x, manual, lgg);
      for (let c = 0; c < 3; c++) expect(viaComposite[c]).toBeCloseTo(ref[c]!, 6);
    }
  });

  it('with correction active, differs from the plain decode->userLut chain', () => {
    let maxDelta = 0;
    for (const x of samples(4)) {
      const withCorrection = reference(x, manual, lgg);
      const plain = sampleLut(userLut, decodePixelToRec709(x, VLOG));
      for (let c = 0; c < 3; c++) maxDelta = Math.max(maxDelta, Math.abs(withCorrection[c]! - plain[c]!));
    }
    expect(maxDelta).toBeGreaterThan(0.02);
  });

  it('with neutral correction, is exactly the plain decode->userLut chain (unchanged behavior)', () => {
    for (const x of samples(5)) {
      const neutral = reference(x, NEUTRAL_MANUAL, NEUTRAL_LGG);
      const plain = sampleLut(userLut, decodePixelToRec709(x, VLOG));
      expect(neutral).toEqual(plain);
    }
  });

  it('on Rec.709 footage (decode is a no-op) still applies the basics under the LUT', () => {
    // Standard footage: decode is identity, so the chain is userLut(basics(x)); the
    // correction must still take effect (this is the non-log branch of the mode).
    for (const x of samples(6, 120)) {
      const out = sampleLut(userLut, applyManualPrimaries(decodePixelToRec709(x, REC709), manual, lgg));
      const plain = sampleLut(userLut, x);
      const changed = out.some((v, c) => Math.abs(v - plain[c]!) > 1e-9);
      // Some pixels are unchanged by clamping; assert the mode is wired (at least one moved).
      if (changed) return;
    }
    throw new Error('expected the basics stage to change at least one Rec.709 pixel under the LUT');
  });
});
