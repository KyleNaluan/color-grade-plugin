import { describe, expect, it } from 'vitest';
import { computeStats } from '../../src/core/analysis/stats.js';
import { computeGradeImpact } from '../../src/core/analysis/gradeImpact.js';
import { classifyGradeRound, type GradeRoundSignal } from '../../src/core/analysis/gradeGuard.js';
import { buildTransform } from '../../src/core/engine/engine.js';
import { themeFromReferenceStats } from '../../src/core/engine/referenceTheme.js';
import {
  writeReferenceStatsFile,
  parseReferenceStatsFile,
} from '../../src/core/analysis/referenceStats.js';

// Deterministic xorshift PRNG (same style as native/scripts/core-parity-test.ts).
function makeRand(seed: number): () => number {
  let s = seed >>> 0;
  return () => {
    s ^= s << 13;
    s ^= s >>> 17;
    s ^= s << 5;
    return ((s >>> 0) % 1_000_000) / 1_000_000;
  };
}

/** Skin-heavy, cool daylight source content - mirrors s7 Exp B's frame A. */
function sourceFrame(n = 4096): Float32Array {
  const px = new Float32Array(n * 3);
  const rSkin = makeRand(0x55aa33cc);
  const rBg = makeRand(0x9911aabb);
  for (let p = 0; p < n; p++) {
    if (p % 3 !== 0) {
      // Skin-toned subject pixels.
      const j = (v: number) => Math.min(1, Math.max(0, v + (rSkin() - 0.5) * 0.1));
      px[p * 3] = j(0.72);
      px[p * 3 + 1] = j(0.55);
      px[p * 3 + 2] = j(0.47);
    } else {
      // Near-neutral bright background (blown bokeh/highlight), tiny per-pixel
      // chroma jitter - the pixel population s7 found the blotch artifact on.
      const j = (v: number) => Math.min(1, Math.max(0, v + (rBg() - 0.5) * 0.03));
      px[p * 3] = j(0.93);
      px[p * 3 + 1] = j(0.94);
      px[p * 3 + 2] = j(0.95);
    }
  }
  return px;
}

/** Warm, dark, skin-free reference content - mirrors s7 Exp B's frame B (the cave). */
function referenceFrame(n = 4096): Float32Array {
  const px = new Float32Array(n * 3);
  const r = makeRand(0x13572468);
  for (let p = 0; p < n; p++) {
    const j = (v: number) => Math.min(1, Math.max(0, v + (r() - 0.5) * 0.08));
    px[p * 3] = j(0.42);
    px[p * 3 + 1] = j(0.26);
    px[p * 3 + 2] = j(0.16);
  }
  return px;
}

describe('themeFromReferenceStats (core mechanism)', () => {
  const refStats = computeStats(referenceFrame());

  it('wraps reference stats as a Theme with matchStats left at default (true)', () => {
    const theme = themeFromReferenceStats(refStats);
    expect(theme.targetStats).toBe(refStats);
    expect(theme.matchStats).not.toBe(false);
    expect(theme.overrides).toBeUndefined();
    expect(theme.knobs.strength.default).toBe(1);
    expect(theme.knobs.skinProtection.default).toBe(0.5);
  });

  it('respects opts for name/description/knobs/overrides', () => {
    const theme = themeFromReferenceStats(refStats, {
      name: 'my-ref',
      strength: 0.7,
      skinProtection: 0.9,
      overrides: { chromaGain: 0.5 },
    });
    expect(theme.name).toBe('my-ref');
    expect(theme.knobs.strength.default).toBe(0.7);
    expect(theme.knobs.skinProtection.default).toBe(0.9);
    expect(theme.overrides).toEqual({ chromaGain: 0.5 });
  });

  it('strength 0 is exact identity, like every other theme (engine invariant)', () => {
    const src = sourceFrame(256);
    const srcStats = computeStats(src);
    const theme = themeFromReferenceStats(refStats);
    const transform = buildTransform(srcStats, theme, { strength: 0 });
    for (let i = 0; i < src.length; i += 3) {
      const out = transform([src[i]!, src[i + 1]!, src[i + 2]!]);
      expect(out[0]).toBeCloseTo(src[i]!, 6);
      expect(out[1]).toBeCloseTo(src[i + 1]!, 6);
      expect(out[2]).toBeCloseTo(src[i + 2]!, 6);
    }
  });

  it('round trip: the graded output moves toward the reference stats (sanity, not exact)', () => {
    const src = sourceFrame();
    const srcStats = computeStats(src);
    const theme = themeFromReferenceStats(refStats);
    const transform = buildTransform(srcStats, theme, {});
    const impact = computeGradeImpact(src, transform);
    // The reference is warm and dark (negative-ish a/b-leaning warm cast, lower luma);
    // the graded output's mean luma should have moved down from the source's.
    expect(impact.outStats.lab.mean[0]).toBeLessThan(srcStats.lab.mean[0]);
  });
});

describe('themeFromReferenceStats (artifact protection + gradeGuard sanity, report sec 1d)', () => {
  const refStats = computeStats(referenceFrame());
  const src = sourceFrame();
  const srcStats = computeStats(src);

  function signalFor(theme: ReturnType<typeof themeFromReferenceStats>): GradeRoundSignal {
    const impact = computeGradeImpact(src, buildTransform(srcStats, theme, {}));
    return {
      castMagnitude: impact.castMagnitude,
      skinHueShiftDeg: impact.skinHueShiftDeg,
      skinChromaShiftPct: impact.skinChromaShiftPct,
      skinWeightTotal: impact.skinWeightTotal,
    };
  }

  it('an authored safety override (lower chromaGain + softLimit + raised skinProtection) reduces chroma amplification on the near-neutral background pixels', () => {
    // "Route the generated theme through the existing safeguards": this is the
    // s7-recommended playbook (report sec 1d / data/cg-agent-grade-s7 Exp B round 2:
    // chromaGain down, softLimit added, skinProtection raised to protect skin from the
    // global chromaGain cut) - not new engine math, all three knobs already exist.
    const baseline = themeFromReferenceStats(refStats);
    const safe = themeFromReferenceStats(refStats, {
      skinProtection: 0.8,
      overrides: { chromaGain: 0.5, chromaShape: { softLimit: 24 } },
    });
    const baselineImpact = computeGradeImpact(src, buildTransform(srcStats, baseline, {}));
    const safeImpact = computeGradeImpact(src, buildTransform(srcStats, safe, {}));
    // The near-neutral background pixels grade down into the output's mids band here
    // (the reference is much darker than the source); that band is where the guard's
    // blotch-risk chroma amplification would show up. The safety override must not
    // amplify it more than the raw baseline.
    expect(safeImpact.outStats.bandChroma.mids).toBeLessThan(baselineImpact.outStats.bandChroma.mids);
  });

  it('gradeGuard classifies the safety-tuned round as an improvement over the raw baseline', () => {
    const baseline = themeFromReferenceStats(refStats);
    const safe = themeFromReferenceStats(refStats, {
      skinProtection: 0.8,
      overrides: { chromaGain: 0.5, chromaShape: { softLimit: 24 } },
    });
    const result = classifyGradeRound(signalFor(baseline), signalFor(safe));
    expect(result.verdict).toBe('improvement');
  });

  it(
    'LIMITATION (documented, not fixed here): pushing the reference mood harder than the ' +
      'safety-tuned round can reintroduce the artifact risk gradeGuard is designed to catch ' +
      '- this is the exact s7 Exp B round 2->3 shape; see gradeGuard.test.ts for the real ' +
      'golden-round regression this guard exists to flag. Nothing here auto-tunes past this ' +
      'point; that is future agent-loop territory (report secs 1a/1d/1f), not this feature.',
    () => {
      const safe = themeFromReferenceStats(refStats, {
        skinProtection: 0.8,
        overrides: { chromaGain: 0.5, chromaShape: { softLimit: 24 } },
      });
      const pushedHarder = themeFromReferenceStats(refStats, {
        skinProtection: 0.8,
        overrides: { chromaGain: 0.9, chromaShape: { softLimit: 40 } },
      });
      const safeImpact = computeGradeImpact(src, buildTransform(srcStats, safe, {}));
      const pushedImpact = computeGradeImpact(src, buildTransform(srcStats, pushedHarder, {}));
      // Sanity: pushing chromaGain/softLimit back up measurably increases chroma in the
      // artifact-prone band again, i.e. the "safe" point really was a deliberate tradeoff,
      // not a free lunch - documenting the limitation rather than asserting a fix exists.
      expect(pushedImpact.outStats.bandChroma.mids).toBeGreaterThan(safeImpact.outStats.bandChroma.mids);
    },
  );
});

describe('reference-stats sidecar file (native editor entry point)', () => {
  const stats = computeStats(referenceFrame());

  it('round-trips through the text format', () => {
    const text = writeReferenceStatsFile(stats);
    const parsed = parseReferenceStatsFile(text);
    expect(parsed).toEqual(stats);
  });

  it('tolerates comma/whitespace-separated input (matches the C++ tokenizer)', () => {
    const parsed = parseReferenceStatsFile(Array(21).fill(0.5).join(', '));
    expect(parsed.lab.mean[0]).toBe(0.5);
  });

  it('throws on malformed input (wrong token count)', () => {
    expect(() => parseReferenceStatsFile('1 2 3')).toThrow(/expected 21/);
  });
});
