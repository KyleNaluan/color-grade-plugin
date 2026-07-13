import { describe, expect, it } from 'vitest';
import { bakeGradeLut } from '../../src/core/lut/gradeLut';
import { THEMES } from '../../src/themes/index.js';
import type { FootageStats } from '../../src/core/analysis/stats.js';

const theme = THEMES['teal-orange']!;
// Any valid stats work for these invariant checks; a theme's target stats share
// the FootageStats schema, so they stand in as measured footage stats here.
const stats: FootageStats = THEMES['cool-noir']!.targetStats;

describe('bakeGradeLut (pure-core Theme -> grade LUT seam)', () => {
  it('is exact identity at strength 0', () => {
    const size = 5;
    const lut = bakeGradeLut(stats, theme, { strength: 0 }, size);
    let i = 0;
    for (let b = 0; b < size; b++) {
      for (let g = 0; g < size; g++) {
        for (let r = 0; r < size; r++) {
          expect(lut.data[i++]).toBeCloseTo(r / (size - 1), 6);
          expect(lut.data[i++]).toBeCloseTo(g / (size - 1), 6);
          expect(lut.data[i++]).toBeCloseTo(b / (size - 1), 6);
        }
      }
    }
  });

  it('keeps every baked entry bounded in [0,1]', () => {
    const lut = bakeGradeLut(stats, theme);
    for (const v of lut.data) {
      expect(v).toBeGreaterThanOrEqual(0);
      expect(v).toBeLessThanOrEqual(1);
    }
  });

  it('defaults to the 33-point theme-grade grid and titles the LUT', () => {
    const lut = bakeGradeLut(stats, theme);
    expect(lut.size).toBe(33);
    expect(lut.data.length).toBe(33 * 33 * 33 * 3);
    expect(lut.title).toContain(theme.name);
  });

  it('actually moves pixels at the theme default strength (non-identity)', () => {
    const size = 9;
    const lut = bakeGradeLut(stats, theme, {}, size);
    let moved = false;
    let i = 0;
    for (let b = 0; b < size && !moved; b++) {
      for (let g = 0; g < size && !moved; g++) {
        for (let r = 0; r < size && !moved; r++) {
          if (
            Math.abs(lut.data[i]! - r / (size - 1)) > 1e-4 ||
            Math.abs(lut.data[i + 1]! - g / (size - 1)) > 1e-4 ||
            Math.abs(lut.data[i + 2]! - b / (size - 1)) > 1e-4
          ) {
            moved = true;
          }
          i += 3;
        }
      }
    }
    expect(moved).toBe(true);
  });
});
