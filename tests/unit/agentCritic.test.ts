import { describe, expect, it } from 'vitest';
import {
  KNOB_TYPE_SPACE,
  CRITIC_RESPONSE_SCHEMA,
  buildCriticPrompt,
  parseCriticProposal,
  critiqueFrame,
  type FetchLike,
} from '../../src/agent/index.js';

describe('KNOB_TYPE_SPACE / prompt embeds numeric ranges', () => {
  it('states the ranges that fix the ~100x magnitude confusion', () => {
    // strength/skinProtection 0-1, tints in LAB units (not 0-1), softLimit scale called out.
    expect(KNOB_TYPE_SPACE).toContain('0-1');
    expect(KNOB_TYPE_SPACE).toContain('LAB units');
    expect(KNOB_TYPE_SPACE).toMatch(/softLimit.*NOT 0-1/s);
    expect(KNOB_TYPE_SPACE).toContain('chromaGain');
  });

  it('buildCriticPrompt carries the mode, the type space, and the current params', () => {
    const prompt = buildCriticPrompt({
      mode: 'correction',
      currentParams: { strength: 1, skinProtection: 0.5, overrides: { chromaGain: 0.4 } },
      round: 2,
    });
    expect(prompt).toContain('CORRECTION');
    expect(prompt).toContain('LAB units');
    expect(prompt).toContain('round 2');
    expect(prompt).toContain('"chromaGain": 0.4');
  });

  it('shot-match mode references the reference frame', () => {
    const prompt = buildCriticPrompt({ mode: 'shot-match', currentParams: { overrides: {} }, round: 0 });
    expect(prompt).toContain('SHOT MATCH');
  });
});

describe('CRITIC_RESPONSE_SCHEMA', () => {
  it('is a structured-output object with the required fields and a continue/stop enum', () => {
    expect(CRITIC_RESPONSE_SCHEMA.type).toBe('object');
    expect(CRITIC_RESPONSE_SCHEMA.required).toEqual(
      expect.arrayContaining(['critique', 'defects', 'proposedOpts', 'proposedOverrides', 'verdict', 'verdictReasoning']),
    );
    expect(CRITIC_RESPONSE_SCHEMA.properties.verdict.enum).toEqual(['continue', 'stop']);
    // The override knob space is fully expressed, including the fields s7 leaned on.
    const ov = CRITIC_RESPONSE_SCHEMA.properties.proposedOverrides.properties;
    for (const k of ['shadowTint', 'midtoneTint', 'highlightTint', 'chromaGain', 'chromaShape', 'toneCurve', 'channelCurves']) {
      expect(ov).toHaveProperty(k);
    }
  });
});

describe('parseCriticProposal', () => {
  it('passes in-range values through untouched', () => {
    const p = parseCriticProposal({
      critique: 'looks casty',
      defects: ['green cast', 'chroma overshoot'],
      proposedOpts: { strength: 0.9, skinProtection: 0.8 },
      proposedOverrides: {
        shadowTint: [5, -5],
        chromaGain: 0.3,
        chromaShape: { softLimit: 22, vibrance: 0.15 },
      },
      verdict: 'continue',
      verdictReasoning: 'reduce chroma',
    });
    expect(p.defects).toHaveLength(2);
    expect(p.proposedOpts).toEqual({ strength: 0.9, skinProtection: 0.8 });
    expect(p.proposedOverrides.chromaGain).toBe(0.3);
    expect(p.proposedOverrides.chromaShape).toEqual({ softLimit: 22, vibrance: 0.15 });
    expect(p.clamped).toEqual([]);
  });

  it('clamps out-of-range numbers and records the clamp (rules-side magnitude backstop)', () => {
    const p = parseCriticProposal({
      defects: [],
      proposedOpts: { strength: 5, skinProtection: -1 },
      proposedOverrides: { shadowTint: [900, -900], chromaGain: 99, chromaShape: { vibrance: 8 } },
      verdict: 'stop',
    });
    expect(p.proposedOpts.strength).toBe(1);
    expect(p.proposedOpts.skinProtection).toBe(0);
    expect(p.proposedOverrides.shadowTint).toEqual([40, -40]);
    expect(p.proposedOverrides.chromaGain).toBe(3);
    expect(p.proposedOverrides.chromaShape?.vibrance).toBe(1);
    expect(p.clamped.length).toBeGreaterThan(0);
    expect(p.verdict).toBe('stop');
  });

  it('drops malformed values instead of crashing', () => {
    const p = parseCriticProposal({
      defects: [1, 'ok', null],
      proposedOpts: { strength: 'nope' },
      proposedOverrides: { shadowTint: [1], toneCurve: [[0, 0]], chromaGain: NaN },
      verdict: 'weird',
    });
    expect(p.defects).toEqual(['ok']);
    expect(p.proposedOpts.strength).toBeUndefined();
    expect(p.proposedOverrides.shadowTint).toBeUndefined(); // too short
    expect(p.proposedOverrides.toneCurve).toBeUndefined(); // <2 points
    expect(p.proposedOverrides.chromaGain).toBeUndefined(); // NaN
    expect(p.verdict).toBe('continue'); // unknown -> default
  });

  it('handles a totally empty payload', () => {
    const p = parseCriticProposal(undefined);
    expect(p.defects).toEqual([]);
    expect(p.proposedOverrides).toEqual({});
    expect(p.verdict).toBe('continue');
  });
});

describe('critiqueFrame (fake fetch)', () => {
  const structured = {
    critique: 'neon blowout',
    defects: ['chroma overshoot'],
    proposedOpts: {},
    proposedOverrides: { chromaGain: 0.25, chromaShape: { softLimit: 22 } },
    verdict: 'continue',
    verdictReasoning: 'tame chroma',
  };

  it('parses a Gemini structured-output response', async () => {
    let capturedUrl = '';
    let capturedBody: any;
    const fetchImpl: FetchLike = async (url, init) => {
      capturedUrl = url;
      capturedBody = JSON.parse(init.body ?? '{}');
      return {
        ok: true,
        status: 200,
        json: async () => ({
          candidates: [{ content: { parts: [{ text: JSON.stringify(structured) }] } }],
          usageMetadata: { promptTokenCount: 1000, candidatesTokenCount: 200 },
        }),
      };
    };
    const res = await critiqueFrame({
      key: 'AIza-test',
      model: 'gemini-flash-latest',
      pngBase64: 'AAAA',
      context: { mode: 'correction', currentParams: { overrides: {} }, round: 0 },
      fetchImpl,
    });
    expect(res.proposal.defects).toEqual(['chroma overshoot']);
    expect(res.proposal.proposedOverrides.chromaGain).toBe(0.25);
    expect(res.usage?.promptTokenCount).toBe(1000);
    expect(capturedUrl).toContain('gemini-flash-latest:generateContent');
    // The image is attached inline and structured output is requested.
    expect(capturedBody.generationConfig.responseMimeType).toBe('application/json');
    expect(JSON.stringify(capturedBody)).toContain('inline_data');
  });

  it('throws a helpful error when the API returns an error', async () => {
    const fetchImpl: FetchLike = async () => ({
      ok: false,
      status: 429,
      json: async () => ({ error: { message: 'quota exceeded' } }),
    });
    await expect(
      critiqueFrame({
        key: 'AIza-test',
        model: 'gemini-flash-latest',
        pngBase64: 'AAAA',
        context: { mode: 'correction', currentParams: { overrides: {} }, round: 0 },
        fetchImpl,
      }),
    ).rejects.toThrow(/quota exceeded/);
  });
});
