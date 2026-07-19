/**
 * The reference-match stats sidecar: a minimal, dependency-free interchange
 * format so the native editor's "Reference Match" theme (no image codec in
 * `native/`) can consume stats computed on the TS side, where the real
 * decode/computeStats pipeline (FrameSource, TIFF/PNG decode) already lives
 * and is fully tested.
 *
 * Format: `STATS_FIELDS` (21) newline-separated decimal numbers, in the
 * canonical `flattenStats` order - no JSON, in the same hand-rolled-parser
 * spirit as `lut/cube.ts`. The C++ counterpart is
 * `parseReferenceStatsText`/`formatReferenceStatsText` in
 * `native/ColorGradeFX/core/Recipe.h`; both sides tokenize on whitespace/comma
 * so either format round-trips through either parser.
 */
import { flattenStats, unflattenStats, STATS_FIELDS, type FootageStats } from './stats.js';

/** Serialize stats to the sidecar text format (one value per line). */
export function writeReferenceStatsFile(stats: FootageStats): string {
  return flattenStats(stats)
    .map((v) => String(v))
    .join('\n');
}

/** Parse the sidecar text format. Throws if it doesn't contain exactly `STATS_FIELDS` tokens. */
export function parseReferenceStatsFile(text: string): FootageStats {
  const tokens = text.split(/[\s,]+/).filter((t) => t.length > 0);
  if (tokens.length !== STATS_FIELDS) {
    throw new Error(`parseReferenceStatsFile: expected ${STATS_FIELDS} values, got ${tokens.length}`);
  }
  return unflattenStats(tokens.map(Number));
}
