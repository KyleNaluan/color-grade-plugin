import { describe, it, expect } from 'vitest';
import { buildTransform } from '../../src/core/engine/engine.js';
import { computeStats, skinWedgeWeight } from '../../src/core/analysis/stats.js';
import { THEMES } from '../../src/themes/index.js';
import type { Vec3 } from '../../src/core/color/types.js';

function syntheticFootage(n: number, seedStart = 42): Float32Array {
  let seed = seedStart;
  const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
  const px = new Float32Array(n * 3);
  for (let i = 0; i < px.length; i++) px[i] = rand();
  return px;
}

describe('engine transform', () => {
  const stats = computeStats(syntheticFootage(5000));

  it('strength 0 is the identity for every theme', () => {
    for (const theme of Object.values(THEMES)) {
      const t = buildTransform(stats, theme, { strength: 0 });
      for (const p of [
        [0.1, 0.2, 0.3],
        [0.5, 0.5, 0.5],
        [0.9, 0.4, 0.1],
      ] as Vec3[]) {
        const out = t(p);
        for (let c = 0; c < 3; c++) expect(out[c]).toBeCloseTo(p[c]!, 6);
      }
    }
  });

  it('output stays in [0,1] over the whole input cube', () => {
    for (const theme of Object.values(THEMES)) {
      const t = buildTransform(stats, theme, { strength: 1 });
      for (let r = 0; r <= 4; r++)
        for (let g = 0; g <= 4; g++)
          for (let b = 0; b <= 4; b++) {
            const out = t([r / 4, g / 4, b / 4]);
            for (let c = 0; c < 3; c++) {
              expect(out[c]).toBeGreaterThanOrEqual(0);
              expect(out[c]).toBeLessThanOrEqual(1);
            }
          }
    }
  });

  it('skin protection pulls skin-toned pixels toward identity when skin is present', () => {
    const skinStats = { ...stats, skinPresence: 0.1 };
    const theme = THEMES['teal-orange']!;
    // A typical skin tone in encoded Rec.709.
    const skin: Vec3 = [0.8, 0.55, 0.42];
    const unprotected = buildTransform(skinStats, theme, { strength: 1, skinProtection: 0 })(skin);
    const protectedOut = buildTransform(skinStats, theme, { strength: 1, skinProtection: 1 })(skin);
    const dist = (p: Vec3) => Math.hypot(p[0] - skin[0], p[1] - skin[1], p[2] - skin[2]);
    expect(dist(protectedOut)).toBeLessThan(dist(unprotected));
  });

  it('skin protection stays inactive below the ~2% presence threshold', () => {
    const noSkinStats = { ...stats, skinPresence: 0.01 };
    const theme = THEMES['teal-orange']!;
    const skin: Vec3 = [0.8, 0.55, 0.42];
    const off = buildTransform(noSkinStats, theme, { strength: 1, skinProtection: 0 })(skin);
    const on = buildTransform(noSkinStats, theme, { strength: 1, skinProtection: 1 })(skin);
    for (let c = 0; c < 3; c++) expect(on[c]).toBeCloseTo(off[c]!, 6);
  });
});

describe('damped color transfer', () => {
  it('caps the mean a/b shift when the target opposes the footage (warm theme on cool footage)', () => {
    const theme = THEMES['warm-film']!;
    // Fake strongly cool footage stats: mean b far below the warm target.
    const coolStats = {
      ...computeStats(syntheticFootage(5000)),
      lab: { mean: [45, -5, -25] as Vec3, std: [20, 8, 8] as Vec3 },
    };
    const t = buildTransform(coolStats, theme, { strength: 1, skinProtection: 0 });
    // Neutral grey must not be dragged the full ~38 LAB units toward the target;
    // the tanh soft clamp bounds the shift to ~10 units.
    const grey: Vec3 = [0.45, 0.45, 0.45];
    const out = t(grey);
    // Rough check in RGB: the warm push on grey should be visible but modest.
    expect(out[0] - out[2]).toBeGreaterThan(0); // warmer, not cooler
    expect(out[0] - out[2]).toBeLessThan(0.15); // but nowhere near a full cast
  });
});

describe('skin wedge', () => {
  it('weights a canonical skin tone highly and a blue sky at zero', () => {
    // LAB of a typical skin patch (~L 65, hue ~45deg).
    expect(skinWedgeWeight(65, 18, 17)).toBeGreaterThan(0.8);
    // Blue sky: hue ~ -80deg.
    expect(skinWedgeWeight(60, 5, -35)).toBe(0);
    // Neutral grey: chroma too low.
    expect(skinWedgeWeight(50, 1, 1)).toBe(0);
  });
});
