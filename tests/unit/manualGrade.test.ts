import { describe, it, expect } from 'vitest';
import {
  buildTransform,
  NEUTRAL_MANUAL,
  NEUTRAL_LGG,
  type ManualGrade,
  type LiftGammaGain,
} from '../../src/core/engine/engine.js';
import { computeStats, encodedRec709ToLab } from '../../src/core/analysis/stats.js';
import { THEMES } from '../../src/themes/index.js';
import { noneManual } from '../../src/themes/none-manual.js';
import type { Vec3 } from '../../src/core/color/types.js';

function synthetic(n: number, seedStart = 7): Float32Array {
  let seed = seedStart;
  const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
  const px = new Float32Array(n * 3);
  for (let i = 0; i < px.length; i++) px[i] = rand();
  return px;
}

const stats = computeStats(synthetic(5000));
const manual = (over: Partial<ManualGrade>): ManualGrade => ({ ...NEUTRAL_MANUAL, ...over });

const SAMPLES: Vec3[] = [
  [0.08, 0.06, 0.05],
  [0.2, 0.25, 0.3],
  [0.5, 0.5, 0.5],
  [0.7, 0.55, 0.45],
  [0.9, 0.4, 0.2],
  [0.95, 0.95, 0.9],
];

const chroma = (rgb: Vec3): number => {
  const lab = encodedRec709ToLab(rgb[0], rgb[1], rgb[2]);
  return Math.hypot(lab[1], lab[2]);
};

describe('None / Manual theme', () => {
  it('is registered in THEMES', () => {
    expect(THEMES['none-manual']).toBe(noneManual);
    expect(noneManual.matchStats).toBe(false);
  });

  it('with a neutral manual grade is exact identity at full strength', () => {
    const t = buildTransform(stats, noneManual, { strength: 1, manual: NEUTRAL_MANUAL });
    for (const p of SAMPLES) {
      const out = t(p);
      for (let c = 0; c < 3; c++) expect(out[c]).toBe(p[c]); // identity fast path: bit-exact clamp
    }
  });

  it('with no manual option at all is exact identity at full strength', () => {
    const t = buildTransform(stats, noneManual, { strength: 1 });
    for (const p of SAMPLES) {
      const out = t(p);
      for (let c = 0; c < 3; c++) expect(out[c]).toBe(p[c]);
    }
  });
});

describe('manual primary correction', () => {
  it('a neutral manual grade never changes a themed grade', () => {
    for (const theme of Object.values(THEMES)) {
      const plain = buildTransform(stats, theme, { strength: 1 });
      const withNeutral = buildTransform(stats, theme, { strength: 1, manual: NEUTRAL_MANUAL });
      for (const p of SAMPLES) {
        const a = plain(p);
        const b = withNeutral(p);
        for (let c = 0; c < 3; c++) expect(b[c]).toBe(a[c]); // bit-exact: neutral is skipped
      }
    }
  });

  it('exposure > 0 brightens, exposure < 0 darkens', () => {
    const up = buildTransform(stats, noneManual, { strength: 1, manual: manual({ exposure: 1 }) });
    const down = buildTransform(stats, noneManual, { strength: 1, manual: manual({ exposure: -1 }) });
    const p: Vec3 = [0.4, 0.4, 0.4];
    expect(up(p)[0]).toBeGreaterThan(p[0]);
    expect(down(p)[0]).toBeLessThan(p[0]);
  });

  it('contrast increases the spread about the pivot', () => {
    const t = buildTransform(stats, noneManual, {
      strength: 1,
      manual: manual({ contrast: 80, pivot: 0.5 }),
    });
    const dark = t([0.25, 0.25, 0.25])[0];
    const light = t([0.75, 0.75, 0.75])[0];
    expect(dark).toBeLessThan(0.25);
    expect(light).toBeGreaterThan(0.75);
    // The pivot maps roughly to itself.
    expect(t([0.5, 0.5, 0.5])[0]).toBeCloseTo(0.5, 6);
  });

  it('shadows lift raises dark pixels more than bright ones', () => {
    const t = buildTransform(stats, noneManual, { strength: 1, manual: manual({ shadows: 100 }) });
    const dLow = t([0.1, 0.1, 0.1])[0] - 0.1;
    const dHigh = t([0.9, 0.9, 0.9])[0] - 0.9;
    expect(dLow).toBeGreaterThan(0.02);
    expect(dLow).toBeGreaterThan(dHigh);
  });

  it('whites lift raises bright pixels more than dark ones', () => {
    const t = buildTransform(stats, noneManual, { strength: 1, manual: manual({ whites: 100 }) });
    const dLow = t([0.1, 0.1, 0.1])[0] - 0.1;
    const dHigh = t([0.9, 0.9, 0.9])[0] - 0.9;
    expect(dHigh).toBeGreaterThan(dLow);
  });

  it('temperature > 0 warms (red rises above blue), < 0 cools', () => {
    const p: Vec3 = [0.5, 0.5, 0.5];
    const warm = buildTransform(stats, noneManual, {
      strength: 1,
      manual: manual({ temperature: 60 }),
    })(p);
    const cool = buildTransform(stats, noneManual, {
      strength: 1,
      manual: manual({ temperature: -60 }),
    })(p);
    expect(warm[0] - warm[2]).toBeGreaterThan(0.02);
    expect(cool[0] - cool[2]).toBeLessThan(-0.02);
  });

  it('saturation scales LAB chroma; 0 desaturates, >1 boosts', () => {
    const p: Vec3 = [0.8, 0.4, 0.2];
    const base = chroma(p);
    // skinProtection 0: this pixel sits on the skin line, so protection would
    // otherwise pull the desaturation back and mask the saturation math.
    const mute = buildTransform(stats, noneManual, {
      strength: 1,
      skinProtection: 0,
      manual: manual({ saturation: 0 }),
    })(p);
    const boost = buildTransform(stats, noneManual, {
      strength: 1,
      skinProtection: 0,
      manual: manual({ saturation: 1.8 }),
    })(p);
    expect(chroma(mute)).toBeLessThan(base * 0.2);
    expect(chroma(boost)).toBeGreaterThan(base);
  });

  it('vibrance boosts low-chroma pixels more than already-saturated ones', () => {
    const lowChroma: Vec3 = [0.55, 0.5, 0.45];
    const highChroma: Vec3 = [0.9, 0.35, 0.15];
    const t = buildTransform(stats, noneManual, {
      strength: 1,
      skinProtection: 0,
      manual: manual({ vibrance: 100 }),
    });
    const gainLow = chroma(t(lowChroma)) / Math.max(chroma(lowChroma), 1e-6);
    const gainHigh = chroma(t(highChroma)) / Math.max(chroma(highChroma), 1e-6);
    expect(gainLow).toBeGreaterThan(gainHigh);
    expect(gainLow).toBeGreaterThan(1);
  });

  it('strength 0 collapses even an aggressive manual grade to identity (D3)', () => {
    const aggressive = manual({
      exposure: 2,
      contrast: 90,
      shadows: 80,
      highlights: -60,
      temperature: 70,
      saturation: 1.6,
    });
    const t = buildTransform(stats, noneManual, { strength: 0, manual: aggressive });
    for (const p of SAMPLES) {
      const out = t(p);
      for (let c = 0; c < 3; c++) expect(out[c]).toBeCloseTo(p[c]!, 6);
    }
  });

  it('output stays in [0,1] over the cube under an extreme manual grade', () => {
    const t = buildTransform(stats, noneManual, {
      strength: 1,
      manual: manual({
        exposure: 5,
        contrast: 100,
        shadows: 100,
        highlights: 100,
        whites: 100,
        blacks: -100,
        temperature: 100,
        tint: -100,
        saturation: 2,
        vibrance: 100,
      }),
    });
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

describe('Look Mix', () => {
  const theme = THEMES['teal-orange']!;

  it('lookMix 1 equals the plain themed grade (bit-exact)', () => {
    const plain = buildTransform(stats, theme, { strength: 1 });
    const full = buildTransform(stats, theme, { strength: 1, lookMix: 1 });
    for (const p of SAMPLES) {
      const a = plain(p);
      const b = full(p);
      for (let c = 0; c < 3; c++) expect(b[c]).toBe(a[c]);
    }
  });

  it('lookMix 0 removes the theme look, leaving only the manual correction', () => {
    const m = manual({ exposure: 0.5, saturation: 1.3 });
    // Same source stats + skinProtection 0 in both, so only the theme look differs
    // (removed by lookMix 0). They then agree within the LAB round-trip tolerance.
    const manualOnly = buildTransform(stats, noneManual, { strength: 1, skinProtection: 0, manual: m });
    const themedNoLook = buildTransform(stats, theme, {
      strength: 1,
      skinProtection: 0,
      manual: m,
      lookMix: 0,
    });
    for (const p of SAMPLES) {
      const a = manualOnly(p);
      const b = themedNoLook(p);
      for (let c = 0; c < 3; c++) expect(b[c]).toBeCloseTo(a[c]!, 5);
    }
  });

  it('lookMix between 0 and 1 lands between manual-only and the full look', () => {
    const partial = buildTransform(stats, theme, { strength: 1, lookMix: 0.5 });
    const full = buildTransform(stats, theme, { strength: 1, lookMix: 1 });
    const none = buildTransform(stats, theme, { strength: 1, lookMix: 0 });
    const p: Vec3 = [0.6, 0.5, 0.4];
    const pv = partial(p)[0];
    const lo = Math.min(full(p)[0], none(p)[0]);
    const hi = Math.max(full(p)[0], none(p)[0]);
    expect(pv).toBeGreaterThanOrEqual(lo - 1e-9);
    expect(pv).toBeLessThanOrEqual(hi + 1e-9);
  });
});

describe('Lift/Gamma/Gain wheels (Phase 6c)', () => {
  const lgg = (over: Partial<LiftGammaGain>): LiftGammaGain => ({ ...NEUTRAL_LGG, ...over });

  it('a neutral LGG is exact identity at full strength (None/Manual)', () => {
    const t = buildTransform(stats, noneManual, { strength: 1, lgg: NEUTRAL_LGG });
    for (const p of SAMPLES) {
      const out = t(p);
      for (let c = 0; c < 3; c++) expect(out[c]).toBe(p[c]); // identity fast path
    }
  });

  it('lift raises the black point (dark pixels move up, white pinned)', () => {
    const t = buildTransform(stats, noneManual, {
      strength: 1,
      skinProtection: 0,
      lgg: lgg({ lift: [0.15, 0.15, 0.15] }),
    });
    const dark: Vec3 = [0.05, 0.05, 0.05];
    const white: Vec3 = [1, 1, 1];
    expect(t(dark)[0]).toBeGreaterThan(dark[0]!);
    // gain 1 => x=1 maps to 1 exactly (lift pivots at white).
    for (let c = 0; c < 3; c++) expect(t(white)[c]).toBeCloseTo(1, 6);
  });

  it('gain scales the white point (black pinned, highlights move)', () => {
    const t = buildTransform(stats, noneManual, {
      strength: 1,
      skinProtection: 0,
      lgg: lgg({ gain: [0.8, 0.8, 0.8] }),
    });
    const black: Vec3 = [0, 0, 0];
    const bright: Vec3 = [0.9, 0.9, 0.9];
    for (let c = 0; c < 3; c++) expect(t(black)[c]).toBeCloseTo(0, 6); // lift 0 pivots at black
    expect(t(bright)[0]).toBeLessThan(bright[0]!);
  });

  it('gamma bends the midtones with both endpoints pinned', () => {
    const t = buildTransform(stats, noneManual, {
      strength: 1,
      skinProtection: 0,
      lgg: lgg({ gamma: [1.6, 1.6, 1.6] }),
    });
    const mid: Vec3 = [0.5, 0.5, 0.5];
    expect(t(mid)[0]).toBeGreaterThan(mid[0]!); // gamma>1 lifts mids
    for (let c = 0; c < 3; c++) {
      expect(t([0, 0, 0])[c]).toBeCloseTo(0, 6);
      expect(t([1, 1, 1])[c]).toBeCloseTo(1, 6);
    }
  });

  it('a per-channel gain pushes color into the highlights', () => {
    const t = buildTransform(stats, noneManual, {
      strength: 1,
      skinProtection: 0,
      lgg: lgg({ gain: [1.1, 1.0, 0.9] }), // warm highlights
    });
    const gray: Vec3 = [0.8, 0.8, 0.8];
    const out = t(gray);
    expect(out[0]).toBeGreaterThan(out[2]!); // red raised above blue
  });
});
