/**
 * Grade-impact measurement: what a built transform actually does to a
 * frame's pixels, beyond the raw stats printout. This is the numeric
 * evidence source for theme-tuning decisions (see `npm run spike`'s
 * grade-impact report) and the input signal for the round-over-round
 * regression guard in `gradeGuard.ts`.
 */
import { computeStats, encodedRec709ToLab, labHueDeg, skinWedgeWeight, type FootageStats } from './stats.js';
import type { Transform } from '../engine/engine.js';

export interface GradeImpact {
  /** Stats of the graded output, directly comparable to the input stats printout. */
  outStats: FootageStats;
  /** Circular-mean hue shift (deg) of pixels inside the skin wedge, in->out. Near 0 = protection holding. */
  skinHueShiftDeg: number;
  /** Relative chroma change (%) of skin-weighted pixels, in->out. */
  skinChromaShiftPct: number;
  /** Skin-weighted pixel fraction (sanity check against FootageStats.skinPresence). */
  skinWeightTotal: number;
  /** LAB distance the frame's average color moved (0 = no cast, >15-20 starts reading as a strong cast). */
  castMagnitude: number;
  /** Hue angle (deg) of the cast movement vector - which way the average color pushed. */
  castDirectionDeg: number;
}

/**
 * Runs `transform` over every pixel and reports skin-region and overall-cast
 * deltas so tuning decisions (skin protection holding, cross-temperature
 * push staying modest) are backed by numbers instead of eyeballing.
 */
export function computeGradeImpact(pixels: Float32Array, transform: Transform): GradeImpact {
  const n = pixels.length / 3;
  const graded = new Float32Array(pixels.length);

  let skinWeight = 0;
  let skinChromaInSum = 0;
  let skinChromaOutSum = 0;
  let sinSum = 0;
  let cosSum = 0;
  let aInSum = 0;
  let bInSum = 0;
  let aOutSum = 0;
  let bOutSum = 0;

  for (let i = 0; i < pixels.length; i += 3) {
    const rIn = pixels[i]!;
    const gIn = pixels[i + 1]!;
    const bIn = pixels[i + 2]!;
    const [rOut, gOut, bOut] = transform([rIn, gIn, bIn]);
    graded[i] = rOut;
    graded[i + 1] = gOut;
    graded[i + 2] = bOut;

    const labIn = encodedRec709ToLab(rIn, gIn, bIn);
    const labOut = encodedRec709ToLab(rOut, gOut, bOut);
    aInSum += labIn[1];
    bInSum += labIn[2];
    aOutSum += labOut[1];
    bOutSum += labOut[2];

    const w = skinWedgeWeight(labIn[0], labIn[1], labIn[2]);
    if (w > 0) {
      skinWeight += w;
      skinChromaInSum += w * Math.hypot(labIn[1], labIn[2]);
      skinChromaOutSum += w * Math.hypot(labOut[1], labOut[2]);
      const hueIn = labHueDeg(labIn[1], labIn[2]);
      const hueOut = labHueDeg(labOut[1], labOut[2]);
      const rad = ((hueOut - hueIn) * Math.PI) / 180;
      sinSum += w * Math.sin(rad);
      cosSum += w * Math.cos(rad);
    }
  }

  const outStats = computeStats(graded);
  const skinHueShiftDeg = skinWeight > 0 ? (Math.atan2(sinSum, cosSum) * 180) / Math.PI : 0;
  const skinChromaShiftPct =
    skinWeight > 0 && skinChromaInSum > 1e-6 ? ((skinChromaOutSum - skinChromaInSum) / skinChromaInSum) * 100 : 0;

  const aIn = aInSum / n;
  const bIn = bInSum / n;
  const aOut = aOutSum / n;
  const bOut = bOutSum / n;
  const dA = aOut - aIn;
  const dB = bOut - bIn;

  return {
    outStats,
    skinHueShiftDeg,
    skinChromaShiftPct,
    skinWeightTotal: skinWeight / n,
    castMagnitude: Math.hypot(dA, dB),
    castDirectionDeg: labHueDeg(dA, dB),
  };
}
