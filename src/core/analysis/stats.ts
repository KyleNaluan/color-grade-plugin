import type { LogProfile, Vec3 } from '../color/types.js';
import { mat3MulVec } from '../color/matrices.js';
import { linearRec709ToLab } from '../color/lab.js';
import { rec709Encode, rec709Decode } from '../color/rec709.js';

/**
 * Footage stats / target stats share this schema so the engine matches stat-to-stat.
 * All values are measured on the decoded (display-referred, gamma-encoded) Rec.709 signal.
 */
export interface FootageStats {
  lumaPercentiles: {
    p1: number;
    p5: number;
    p25: number;
    p50: number;
    p75: number;
    p95: number;
    p99: number;
  };
  lab: {
    mean: Vec3; // [L, a, b]
    std: Vec3;
  };
  /** Mean LAB chroma per tonal band (fixed luma thresholds 0-0.25 / 0.25-0.7 / 0.7-1.0). */
  bandChroma: {
    shadows: number;
    mids: number;
    highlights: number;
  };
  saturation: {
    mean: number;
    std: number;
  };
  /** Fraction of pixels classified as skin-toned (near the vectorscope skin line). */
  skinPresence: number;
  clipping: {
    low: number;
    high: number;
  };
}

export const SHADOW_MID_SPLIT = 0.25;
export const MID_HIGHLIGHT_SPLIT = 0.7;

/** Rec.709 luma from gamma-encoded RGB. */
export function luma709(r: number, g: number, b: number): number {
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/** LAB hue angle in degrees, [0, 360). */
export function labHueDeg(a: number, b: number): number {
  const h = (Math.atan2(b, a) * 180) / Math.PI;
  return h < 0 ? h + 360 : h;
}

/**
 * Soft membership [0,1] in the skin-tone wedge around the vectorscope skin line,
 * evaluated in LAB chroma space. Full weight within +-18 deg of the skin hue,
 * falling to 0 at +-32 deg, gated on plausible chroma and lightness.
 */
export function skinWedgeWeight(l: number, a: number, b: number): number {
  const chroma = Math.hypot(a, b);
  if (chroma < 6 || l < 15 || l > 92) return 0;
  const SKIN_HUE = 42; // degrees, LAB hue of the skin line
  let dh = Math.abs(labHueDeg(a, b) - SKIN_HUE);
  if (dh > 180) dh = 360 - dh;
  const hueW = dh <= 18 ? 1 : dh >= 32 ? 0 : 1 - (dh - 18) / 14;
  // Fade in over low chroma so near-neutral pixels don't count as skin.
  const chromaW = Math.min(1, (chroma - 6) / 6);
  return hueW * chromaW;
}

function percentile(sorted: Float32Array, p: number): number {
  if (sorted.length === 0) return 0;
  const idx = Math.min(sorted.length - 1, Math.max(0, Math.round((p / 100) * (sorted.length - 1))));
  return sorted[idx]!;
}

/**
 * Decode raw encoded pixels (in the given log profile) to display-referred,
 * gamma-encoded Rec.709 in place-safe fashion. Input: interleaved RGB floats [0,1].
 */
export function decodeToRec709(pixels: Float32Array, profile: LogProfile): Float32Array {
  const out = new Float32Array(pixels.length);
  const m = profile.gamutToRec709;
  const isIdentity = profile.name === 'Rec.709';
  for (let i = 0; i < pixels.length; i += 3) {
    if (isIdentity) {
      out[i] = pixels[i]!;
      out[i + 1] = pixels[i + 1]!;
      out[i + 2] = pixels[i + 2]!;
      continue;
    }
    const lin: Vec3 = [
      profile.decode(pixels[i]!),
      profile.decode(pixels[i + 1]!),
      profile.decode(pixels[i + 2]!),
    ];
    const [r, g, b] = mat3MulVec(m, lin);
    out[i] = clamp01(rec709Encode(Math.max(0, r)));
    out[i + 1] = clamp01(rec709Encode(Math.max(0, g)));
    out[i + 2] = clamp01(rec709Encode(Math.max(0, b)));
  }
  return out;
}

function clamp01(x: number): number {
  return x < 0 ? 0 : x > 1 ? 1 : x;
}

/** Encoded Rec.709 RGB pixel -> LAB. */
export function encodedRec709ToLab(r: number, g: number, b: number): Vec3 {
  return linearRec709ToLab([rec709Decode(r), rec709Decode(g), rec709Decode(b)]);
}

/**
 * Compute footage stats from decoded (gamma-encoded Rec.709) interleaved RGB pixels.
 */
export function computeStats(pixels: Float32Array, clipEps = 0.001): FootageStats {
  const n = pixels.length / 3;
  if (n === 0) throw new Error('computeStats: empty pixel buffer');

  const lumas = new Float32Array(n);
  let labSum = [0, 0, 0];
  let labSqSum = [0, 0, 0];
  let satSum = 0;
  let satSqSum = 0;
  let skinSum = 0;
  let clipLow = 0;
  let clipHigh = 0;
  const bandChromaSum = [0, 0, 0];
  const bandCount = [0, 0, 0];

  for (let i = 0, px = 0; i < pixels.length; i += 3, px++) {
    const r = pixels[i]!;
    const g = pixels[i + 1]!;
    const b = pixels[i + 2]!;
    const y = luma709(r, g, b);
    lumas[px] = y;

    const lab = encodedRec709ToLab(r, g, b);
    for (let c = 0; c < 3; c++) {
      labSum[c]! += lab[c]!;
      labSqSum[c]! += lab[c]! * lab[c]!;
    }

    const chroma = Math.hypot(lab[1], lab[2]);
    const band = y < SHADOW_MID_SPLIT ? 0 : y < MID_HIGHLIGHT_SPLIT ? 1 : 2;
    bandChromaSum[band]! += chroma;
    bandCount[band]! += 1;

    const mx = Math.max(r, g, b);
    const mn = Math.min(r, g, b);
    const sat = mx > 1e-6 ? (mx - mn) / mx : 0;
    satSum += sat;
    satSqSum += sat * sat;

    skinSum += skinWedgeWeight(lab[0], lab[1], lab[2]);

    if (mn <= clipEps) clipLow++;
    if (mx >= 1 - clipEps) clipHigh++;
  }

  lumas.sort();
  const mean = (s: number) => s / n;
  const std = (sq: number, m: number) => Math.sqrt(Math.max(0, sq / n - m * m));
  const labMean: Vec3 = [mean(labSum[0]!), mean(labSum[1]!), mean(labSum[2]!)];

  return {
    lumaPercentiles: {
      p1: percentile(lumas, 1),
      p5: percentile(lumas, 5),
      p25: percentile(lumas, 25),
      p50: percentile(lumas, 50),
      p75: percentile(lumas, 75),
      p95: percentile(lumas, 95),
      p99: percentile(lumas, 99),
    },
    lab: {
      mean: labMean,
      std: [std(labSqSum[0]!, labMean[0]), std(labSqSum[1]!, labMean[1]), std(labSqSum[2]!, labMean[2])],
    },
    bandChroma: {
      shadows: bandCount[0]! > 0 ? bandChromaSum[0]! / bandCount[0]! : 0,
      mids: bandCount[1]! > 0 ? bandChromaSum[1]! / bandCount[1]! : 0,
      highlights: bandCount[2]! > 0 ? bandChromaSum[2]! / bandCount[2]! : 0,
    },
    saturation: {
      mean: satSum / n,
      std: std(satSqSum, satSum / n),
    },
    skinPresence: skinSum / n,
    clipping: {
      low: clipLow / n,
      high: clipHigh / n,
    },
  };
}
