import { describe, it, expect } from 'vitest';
import { buildTransform, toneStretchChromaGuard } from '../../src/core/engine/engine.js';
import {
  computeStats,
  skinWedgeWeight,
  encodedRec709ToLab,
  luma709,
  SHADOW_MID_SPLIT,
  MID_HIGHLIGHT_SPLIT,
  type FootageStats,
} from '../../src/core/analysis/stats.js';
import type { Theme } from '../../src/core/engine/theme.js';
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
      midtoneTint: [-30, 30],
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

describe('midtone tint', () => {
  // Synthetic reproduction of the scout report's Experiment A round-2 gap
  // (`firstmate/data/cg-agent-grade-s7/report.md`): a uniform olive cast
  // sitting entirely in the midtone luma band. shadowTint/highlightTint are
  // weighted by bandWeights(), which is ~0 for shadows/highlights when every
  // pixel's luma sits deep in the mids - so they cannot reach it. midtoneTint
  // uses the same band-weighting approach against the mids weight instead.
  function midtoneCastFrame(n: number): Float32Array {
    let seed = 99;
    const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
    const px = new Float32Array(n * 3);
    for (let i = 0; i < px.length; i += 3) {
      // Luma ~0.5-0.6, comfortably inside SHADOW_MID_SPLIT..MID_HIGHLIGHT_SPLIT
      // (0.25-0.7) with room to spare from the 0.08 feather on either edge.
      const base = 0.5 + rand() * 0.08;
      // Olive cast: boost green, cut blue relative to the neutral base.
      px[i] = Math.min(1, Math.max(0, base - 0.02 + (rand() - 0.5) * 0.02));
      px[i + 1] = Math.min(1, Math.max(0, base + 0.08 + (rand() - 0.5) * 0.02));
      px[i + 2] = Math.min(1, Math.max(0, base - 0.1 + (rand() - 0.5) * 0.02));
    }
    return px;
  }

  function meanAB(px: Float32Array): [number, number] {
    let sumA = 0;
    let sumB = 0;
    let n = 0;
    for (let i = 0; i < px.length; i += 3) {
      const lab = encodedRec709ToLab(px[i]!, px[i + 1]!, px[i + 2]!);
      sumA += lab[1];
      sumB += lab[2];
      n++;
    }
    return [sumA / n, sumB / n];
  }

  function applyToFrame(t: (p: Vec3) => Vec3, px: Float32Array): Float32Array {
    const out = new Float32Array(px.length);
    for (let i = 0; i < px.length; i += 3) {
      const o = t([px[i]!, px[i + 1]!, px[i + 2]!]);
      out[i] = o[0]!;
      out[i + 1] = o[1]!;
      out[i + 2] = o[2]!;
    }
    return out;
  }

  const frame = midtoneCastFrame(4000);
  const src = computeStats(frame);
  const [srcA, srcB] = meanAB(frame);
  // Sanity: the fixture is a real, sizable cast, not a rounding artifact.
  const srcCastMag = Math.hypot(srcA, srcB);

  // Target = source stats verbatim: the automatic stat-matching stage is
  // already fully converged (no tone-curve stretch, no mean-shift, no
  // band-scale change), isolating the tint overrides as the only thing left
  // that can move color - exactly the round-2 situation in the scout report.
  const target: FootageStats = src;
  const baseKnobs = { strength: { default: 1 }, skinProtection: { default: 0 } };

  it('fixture cast is sizable and lands in the midtone band', () => {
    expect(srcCastMag).toBeGreaterThan(8);
  });

  it('shadow+highlight tints alone cannot cancel a midtone-concentrated cast', () => {
    const theme: Theme = {
      name: 'midtone-cast-repro',
      description: 'synthetic repro of the scout report Experiment A round-2 gap',
      targetStats: target,
      knobs: baseKnobs,
      overrides: { shadowTint: [-srcA, -srcB], highlightTint: [-srcA, -srcB] },
    };
    const t = buildTransform(src, theme, { strength: 1, skinProtection: 0 });
    const [outA, outB] = meanAB(applyToFrame(t, frame));
    // Barely moved: shadow/highlight band weight is ~0 for these all-midtone pixels.
    expect(Math.hypot(outA, outB)).toBeGreaterThan(srcCastMag * 0.8);
  });

  it('midtoneTint cancels the same cast, driving mean a/b near neutral', () => {
    const theme: Theme = {
      name: 'midtone-cast-fixed',
      description: 'same repro, with midtoneTint added',
      targetStats: target,
      knobs: baseKnobs,
      overrides: {
        shadowTint: [-srcA, -srcB],
        highlightTint: [-srcA, -srcB],
        midtoneTint: [-srcA, -srcB],
      },
    };
    const t = buildTransform(src, theme, { strength: 1, skinProtection: 0 });
    const [outA, outB] = meanAB(applyToFrame(t, frame));
    expect(Math.hypot(outA, outB)).toBeLessThan(2);
  });

  it('is a no-op when unset (no regression to shadow/highlight-only behavior)', () => {
    const withMidtone: Theme = {
      name: 'a',
      description: 'a',
      targetStats: target,
      knobs: baseKnobs,
      overrides: { shadowTint: [4, -4], highlightTint: [-4, 4], midtoneTint: undefined },
    };
    const without: Theme = { ...withMidtone, overrides: { shadowTint: [4, -4], highlightTint: [-4, 4] } };
    const px: Vec3 = [0.5, 0.55, 0.42];
    const outWith = buildTransform(src, withMidtone, { strength: 1, skinProtection: 0 })(px);
    const outWithout = buildTransform(src, without, { strength: 1, skinProtection: 0 })(px);
    for (let c = 0; c < 3; c++) expect(outWith[c]).toBeCloseTo(outWithout[c]!, 10);
  });
});

describe('chroma-overshoot guard', () => {
  // Synthetic reproduction of the scout report's Experiment A failure shape
  // (no dependency on the gitignored personal footage fixtures): a degraded,
  // low-dynamic-range source whose luma sits in a narrow ~0.30-0.60 band with a
  // green/blue cast, stat-matched toward a full-range target histogram. The
  // large tone-curve stretch relocates mid pixels into the near-empty
  // shadow/highlight bands, where the auto bandScale + kA/kB gains compound and
  // used to explode chroma into a neon blowout.
  function degradedFrame(n: number): Float32Array {
    let seed = 7;
    const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
    const px = new Float32Array(n * 3);
    for (let i = 0; i < px.length; i += 3) {
      const base = 0.3 + rand() * 0.3; // luma ~0.30..0.60 (low dynamic range)
      px[i] = Math.min(1, Math.max(0, base + (rand() - 0.5) * 0.12 - 0.03));
      px[i + 1] = Math.min(1, Math.max(0, base + (rand() - 0.5) * 0.12 + 0.04));
      px[i + 2] = Math.min(1, Math.max(0, base + (rand() - 0.5) * 0.12 + 0.03));
    }
    return px;
  }
  function maxBandChroma(px: Float32Array): number {
    const sum = [0, 0, 0];
    const cnt = [0, 0, 0];
    for (let i = 0; i < px.length; i += 3) {
      const y = luma709(px[i]!, px[i + 1]!, px[i + 2]!);
      const lab = encodedRec709ToLab(px[i]!, px[i + 1]!, px[i + 2]!);
      const chroma = Math.hypot(lab[1], lab[2]);
      const band = y < SHADOW_MID_SPLIT ? 0 : y < MID_HIGHLIGHT_SPLIT ? 1 : 2;
      sum[band]! += chroma;
      cnt[band]! += 1;
    }
    return Math.max(...[0, 1, 2].map((b) => (cnt[b]! > 0 ? sum[b]! / cnt[b]! : 0)));
  }
  const applyToFrame = (t: (p: Vec3) => Vec3, frame: Float32Array): Float32Array => {
    const out = new Float32Array(frame.length);
    for (let i = 0; i < frame.length; i += 3) {
      const o = t([frame[i]!, frame[i + 1]!, frame[i + 2]!]);
      out[i] = o[0];
      out[i + 1] = o[1];
      out[i + 2] = o[2];
    }
    return out;
  };

  const frame = degradedFrame(20000);
  const src = computeStats(frame);
  // Wide-range target with modest per-band chroma (a genuinely full-range look).
  const target: FootageStats = {
    lumaPercentiles: { p1: 0.1, p5: 0.135, p25: 0.42, p50: 0.662, p75: 0.85, p95: 1.0, p99: 1.0 },
    lab: { mean: [66.9, 0.13, 0.8], std: [24, 9, 10] },
    bandChroma: { shadows: 2.1, mids: 5.4, highlights: 3.9 },
    saturation: { mean: 0.15, std: 0.1 },
    skinPresence: 0.047,
    clipping: { low: 0, high: 0.143 },
  };
  const theme: Theme = {
    name: 'synthetic-wide',
    description: 'wide-range target for the overshoot repro',
    targetStats: target,
    knobs: { strength: { default: 1 }, skinProtection: { default: 0.5 } },
  };
  const targetMaxBand = Math.max(target.bandChroma.shadows, target.bandChroma.mids, target.bandChroma.highlights);

  it('engages on a large tone-curve stretch and stays inert on ordinary sources', () => {
    const g = toneStretchChromaGuard(src, target);
    expect(g.stretch).toBeGreaterThan(2.5); // narrow source -> wide target
    expect(g.gain).toBeLessThan(0.3); // auto amplification strongly damped
    expect(g.autoSoftLimit).toBeGreaterThan(0);

    // Ordinary sources (source range within ~1.5x of target) must stay exactly
    // inert, guaranteeing byte-identical output to the pre-guard engine.
    const normal = computeStats(syntheticFootage(20000));
    for (const th of Object.values(THEMES)) {
      const gi = toneStretchChromaGuard(normal, th.targetStats);
      expect(gi.severity).toBe(0);
      expect(gi.gain).toBe(1);
      expect(gi.autoSoftLimit).toBeUndefined();
    }
  });

  it('caps band chroma near the target where the un-guarded engine exploded past it', () => {
    const guard = toneStretchChromaGuard(src, target);

    // With the guard (default: no authored chromaGain/softLimit).
    const guardedMax = maxBandChroma(applyToFrame(buildTransform(src, theme, { strength: 1 }), frame));

    // Reconstruct the pre-guard behavior through the public API: an explicit
    // chromaGain of 1/guard.gain cancels the auto damping and an explicit huge
    // softLimit disables the auto ceiling (authored overrides win). This is the
    // exact transform the engine produced before the guard existed.
    const unguardedTheme: Theme = {
      ...theme,
      overrides: { chromaGain: 1 / guard.gain, chromaShape: { softLimit: 1e9 } },
    };
    const unguardedMax = maxBandChroma(applyToFrame(buildTransform(src, unguardedTheme, { strength: 1 }), frame));

    // Before: the un-guarded transform explodes band chroma many times past target.
    expect(unguardedMax).toBeGreaterThan(5 * targetMaxBand);
    // After: the guard keeps it within a sane multiple of the target's chroma...
    expect(guardedMax).toBeLessThan(2.5 * targetMaxBand);
    // ...and dramatically below the un-guarded blowout.
    expect(guardedMax).toBeLessThan(unguardedMax / 4);
  });

  it('respects an authored chroma ceiling instead of installing its own', () => {
    const guard = toneStretchChromaGuard(src, target);
    expect(guard.autoSoftLimit).toBeGreaterThan(0);
    // An authored softLimit takes precedence over the auto ceiling.
    const authored: Theme = { ...theme, overrides: { chromaShape: { softLimit: 8 } } };
    const authoredMax = maxBandChroma(applyToFrame(buildTransform(src, authored, { strength: 1 }), frame));
    // 8 is tighter than the auto ceiling (~16), so output chroma is bounded by it.
    expect(authoredMax).toBeLessThanOrEqual(8);
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
