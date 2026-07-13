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

describe('expanded overrides', () => {
  const stats = computeStats(syntheticFootage(5000));
  const base = THEMES['teal-orange']!;
  /** Same theme with the given overrides replacing the authored ones. */
  const withOverrides = (overrides: NonNullable<typeof base.overrides>) => ({ ...base, overrides });

  it('authored tone curve shifts mids in the authored direction', () => {
    const lifted = withOverrides({ toneCurve: [[0, 0], [0.5, 0.65], [1, 1]] });
    const flat = withOverrides({});
    const grey: Vec3 = [0.4, 0.4, 0.4];
    const outLifted = buildTransform(stats, lifted, { strength: 1, skinProtection: 0 })(grey);
    const outFlat = buildTransform(stats, flat, { strength: 1, skinProtection: 0 })(grey);
    const lum = (p: Vec3) => (p[0] + p[1] + p[2]) / 3;
    expect(lum(outLifted)).toBeGreaterThan(lum(outFlat));
  });

  it('per-channel red curve warms neutral input, moving red most', () => {
    const redBoost = withOverrides({ channelCurves: { r: [[0, 0], [0.5, 0.6], [1, 1]] } });
    const none = withOverrides({});
    const grey: Vec3 = [0.4, 0.4, 0.4];
    const boosted = buildTransform(stats, redBoost, { strength: 1, skinProtection: 0 })(grey);
    const plain = buildTransform(stats, none, { strength: 1, skinProtection: 0 })(grey);
    const dR = boosted[0] - plain[0];
    expect(dR).toBeGreaterThan(0);
    // The curve feeds the LAB color stage, so the chroma stages spread part of
    // the warm push into G/B - but the authored channel must dominate.
    expect(dR).toBeGreaterThan(2 * Math.abs(boosted[1] - plain[1]));
    expect(dR).toBeGreaterThan(2 * Math.abs(boosted[2] - plain[2]));
  });

  it('vibrance boosts low-chroma pixels proportionally more than saturated ones', () => {
    const vib = withOverrides({ chromaShape: { vibrance: 0.5 } });
    const none = withOverrides({});
    const chromaOf = (p: Vec3) => {
      // Approximate chroma via max-min channel spread in encoded RGB.
      return Math.max(...p) - Math.min(...p);
    };
    const muted: Vec3 = [0.5, 0.47, 0.45];
    const saturated: Vec3 = [0.9, 0.4, 0.1];
    const gain = (px: Vec3) => {
      const withVib = buildTransform(stats, vib, { strength: 1, skinProtection: 0 })(px);
      const plain = buildTransform(stats, none, { strength: 1, skinProtection: 0 })(px);
      return chromaOf(withVib) / Math.max(chromaOf(plain), 1e-6);
    };
    expect(gain(muted)).toBeGreaterThan(gain(saturated));
    expect(gain(muted)).toBeGreaterThan(1);
  });

  it('chroma-by-luma curve mutes highlights when authored to', () => {
    const muteHi = withOverrides({ chromaShape: { byLuma: [[0, 1], [0.5, 1], [1, 0.3]] } });
    const none = withOverrides({});
    const brightWarm: Vec3 = [0.95, 0.85, 0.7];
    const spread = (p: Vec3) => Math.max(...p) - Math.min(...p);
    const mutedOut = buildTransform(stats, muteHi, { strength: 1, skinProtection: 0 })(brightWarm);
    const plainOut = buildTransform(stats, none, { strength: 1, skinProtection: 0 })(brightWarm);
    expect(spread(mutedOut)).toBeLessThan(spread(plainOut));
  });

  it('soft chroma limit reduces the most saturated output', () => {
    const limited = withOverrides({ chromaShape: { softLimit: 20 } });
    const none = withOverrides({});
    const saturated: Vec3 = [0.95, 0.2, 0.05];
    const spread = (p: Vec3) => Math.max(...p) - Math.min(...p);
    const lim = buildTransform(stats, limited, { strength: 1, skinProtection: 0 })(saturated);
    const plain = buildTransform(stats, none, { strength: 1, skinProtection: 0 })(saturated);
    expect(spread(lim)).toBeLessThan(spread(plain));
  });

  it('extreme authored overrides still keep output in [0,1]', () => {
    const extreme = withOverrides({
      toneCurve: [[0, 0.2], [0.5, 0.9], [1, 1.2]],
      channelCurves: { r: [[0, -0.2], [1, 1.3]], b: [[0, 0], [0.3, 0.9], [1, 1]] },
      chromaShape: { byLuma: [[0, 3], [0.5, 0.1], [1, 2]], vibrance: 1, softLimit: 200 },
      chromaGain: 2,
      shadowTint: [30, -30],
      highlightTint: [-30, 30],
    });
    const t = buildTransform(stats, extreme, { strength: 1, skinProtection: 0 });
    for (let r = 0; r <= 4; r++)
      for (let g = 0; g <= 4; g++)
        for (let b = 0; b <= 4; b++) {
          const out = t([r / 4, g / 4, b / 4]);
          for (let c = 0; c < 3; c++) {
            expect(out[c]).toBeGreaterThanOrEqual(0);
            expect(out[c]).toBeLessThanOrEqual(1);
          }
        }
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
