import { describe, expect, it, vi } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { applyThemeGrade, buildGradeRecipe } from '../../src/panel/gradeStack';
import { analyzeCurrentFrame } from '../../src/panel/analyze';
import { createFileFrameSource } from '../../src/host/fileFrameSource';
import { writeCube } from '../../src/core/lut/cube.js';
import { bakeGradeLut } from '../../src/core/lut/gradeLut.js';
import { REC709 } from '../../src/core/color/rec709.js';
import { THEMES } from '../../src/themes/index.js';
import type { Bridge, GradeResult } from '../../src/host/bridge';
import type { FrameFileReader, RenderedFrameRef } from '../../src/host/frameSource';

const FIXTURES = join(__dirname, '..', 'fixtures', 'frame-source');
const theme = THEMES['teal-orange']!;

const nodeReader: FrameFileReader = {
  async read(path: string) {
    return new Uint8Array(readFileSync(path));
  },
};

const fixtureSource = (name = 'synthetic.tif', format: RenderedFrameRef['format'] = 'tiff') =>
  createFileFrameSource(() => ({ path: join(FIXTURES, name), format }), nodeReader);

type GradeBridge = Pick<Bridge, 'getCurrentTime' | 'applyGrade'>;

function fakeBridge(
  time: number | null,
  result: GradeResult = {
    gradeLutPath: '/proj/.colorgrade/grade_42.cube',
    recipePath: '/proj/.colorgrade/grade_42.json',
  },
): GradeBridge & { calls: Array<[string, string, number]> } {
  const calls: Array<[string, string, number]> = [];
  return {
    calls,
    async getCurrentTime() {
      return time;
    },
    async applyGrade(gradeLutCube, recipeJson, analyzedLayerId) {
      calls.push([gradeLutCube, recipeJson, analyzedLayerId]);
      return result;
    },
  };
}

describe('applyThemeGrade (panel logic against a fake bridge + file FrameSource)', () => {
  it('analyzes post-decode (identity Rec.709), bakes the theme grade, and sends it', async () => {
    const bridge = fakeBridge(1.5);
    const application = await applyThemeGrade(bridge, fixtureSource(), theme, 42);

    // Independently reproduce the post-decode analysis + bake at the core seam.
    const expectedAnalysis = await analyzeCurrentFrame(fakeBridge(1.5), fixtureSource(), REC709);
    const expectedCube = writeCube(bakeGradeLut(expectedAnalysis.stats, theme));

    expect(bridge.calls).toHaveLength(1);
    const [cubeText, recipeJson, analyzedLayerId] = bridge.calls[0]!;
    expect(cubeText).toBe(expectedCube);
    expect(analyzedLayerId).toBe(42);

    // The analysis rode the identity Rec.709 profile: the rendered frame is
    // already post-Correct, so no second software decode was applied.
    expect(application.analysis.profileName).toBe(REC709.name);
    expect(application.result).toEqual({
      gradeLutPath: '/proj/.colorgrade/grade_42.cube',
      recipePath: '/proj/.colorgrade/grade_42.json',
    });

    const recipe = JSON.parse(recipeJson);
    expect(recipe).toEqual(application.recipe);
  });

  it('persists reproducible recipe inputs (theme, default knobs, stats, profile)', async () => {
    const bridge = fakeBridge(0);
    const application = await applyThemeGrade(bridge, fixtureSource(), theme, 7);

    const recipe = JSON.parse(bridge.calls[0]![1]);
    expect(recipe.version).toBe(1);
    expect(recipe.theme).toBe(theme.name);
    expect(recipe.knobs).toEqual({
      strength: theme.knobs.strength.default,
      skinProtection: theme.knobs.skinProtection.default,
    });
    expect(recipe.analyzedTime).toBe(0);
    expect(recipe.analyzedProfile).toBe(REC709.name);
    // Stats round-trip through JSON, so match against the recipe the panel built.
    expect(recipe.stats).toEqual(application.recipe.stats);
  });

  it('throws when no comp is active (nothing to analyze)', async () => {
    const bridge = fakeBridge(null);
    await expect(applyThemeGrade(bridge, fixtureSource(), theme, 42)).rejects.toThrow(
      /no active comp/i,
    );
    expect(bridge.calls).toHaveLength(0);
  });

  it('propagates bridge failures (e.g. unsaved project) without swallowing them', async () => {
    const bridge: GradeBridge = {
      async getCurrentTime() {
        return 1;
      },
      applyGrade: vi.fn(async () => {
        throw new Error('save the project before grading');
      }),
    };
    await expect(applyThemeGrade(bridge, fixtureSource(), theme, 42)).rejects.toThrow(
      /save the project/,
    );
  });
});

describe('buildGradeRecipe', () => {
  it('captures the theme, its default knobs, and the analyzed frame', async () => {
    const analysis = await analyzeCurrentFrame(fakeBridge(2.25), fixtureSource(), REC709);
    const recipe = buildGradeRecipe(theme, analysis);
    expect(recipe.theme).toBe(theme.name);
    expect(recipe.knobs.strength).toBe(theme.knobs.strength.default);
    expect(recipe.analyzedTime).toBe(2.25);
    expect(recipe.analyzedProfile).toBe(REC709.name);
    expect(recipe.stats).toBe(analysis.stats);
  });
});
