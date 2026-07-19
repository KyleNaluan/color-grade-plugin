import { describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { themeFromReferenceImage } from '../../src/panel/referenceMatch';
import { applyThemeGrade } from '../../src/panel/gradeStack';
import { createFileFrameSource } from '../../src/host/fileFrameSource';
import { PROFILES } from '../../src/core/color/index.js';
import type { Bridge, GradeResult } from '../../src/host/bridge';
import type { FrameFileReader, RenderedFrameRef } from '../../src/host/frameSource';

const FIXTURES = join(__dirname, '..', 'fixtures', 'frame-source');

const nodeReader: FrameFileReader = {
  async read(path: string) {
    return new Uint8Array(readFileSync(path));
  },
};

const fixtureSource = (name = 'synthetic.tif', format: RenderedFrameRef['format'] = 'tiff') =>
  createFileFrameSource(() => ({ path: join(FIXTURES, name), format }), nodeReader);

type GradeBridge = Pick<Bridge, 'getCurrentTime' | 'applyGrade' | 'setGradeLayerEnabled'>;

function fakeBridge(
  time: number | null,
  result: GradeResult = {
    gradeLutPath: '/proj/.colorgrade/grade_ref.cube',
    recipePath: '/proj/.colorgrade/grade_ref.json',
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
    async setGradeLayerEnabled() {
      return false;
    },
  };
}

describe('themeFromReferenceImage -> applyThemeGrade (end-to-end reference-match round trip)', () => {
  it('builds a Theme from a reference still and runs it through the unmodified grade path', async () => {
    // The reference still and the clip being graded can be entirely unrelated content;
    // here they happen to be the same fixture, which is fine - the mechanism only cares
    // about measured stats, not content identity.
    const theme = await themeFromReferenceImage(fixtureSource(), PROFILES['rec709']!);
    expect(theme.name).toBe('reference-match');
    expect(theme.targetStats).toBeDefined();

    const bridge = fakeBridge(1.5);
    const application = await applyThemeGrade(bridge, fixtureSource(), theme, 42);

    expect(bridge.calls).toHaveLength(1);
    const recipe = JSON.parse(bridge.calls[0]![1]);
    expect(recipe.theme).toBe('reference-match');
    expect(application.recipe.theme).toBe('reference-match');
  });

  it('works identically against the PNG fallback fixture (same FrameSource consumer code)', async () => {
    const theme = await themeFromReferenceImage(
      fixtureSource('synthetic.png', 'png'),
      PROFILES['rec709']!,
    );
    expect(theme.targetStats.skinPresence).toBeGreaterThanOrEqual(0);
  });

  it('threads ReferenceThemeOptions (name, knobs, safety overrides) through to the built Theme', async () => {
    const theme = await themeFromReferenceImage(fixtureSource(), PROFILES['rec709']!, {
      name: 'my-look',
      strength: 0.8,
      overrides: { chromaGain: 0.5, chromaShape: { softLimit: 24 } },
    });
    expect(theme.name).toBe('my-look');
    expect(theme.knobs.strength.default).toBe(0.8);
    expect(theme.overrides).toEqual({ chromaGain: 0.5, chromaShape: { softLimit: 24 } });
  });
});
