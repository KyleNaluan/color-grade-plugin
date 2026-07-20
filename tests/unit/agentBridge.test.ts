import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { describe, expect, it } from 'vitest';
import {
  formatAgentRequest,
  parseAgentRequest,
  formatAgentResponse,
  parseAgentResponse,
  errorResponse,
  editorApplyFromResult,
  type AgentBridgeRequest,
  type AgentBridgeResponse,
} from '../../src/agent/index.js';
import type { Theme } from '../../src/core/engine/theme.js';
import type { AutoGradeParams } from '../../src/agent/types.js';

const FIX = join(dirname(fileURLToPath(import.meta.url)), '../../native/tests/fixtures/agent');

describe('agent bridge protocol - request round-trip', () => {
  it('formats and re-parses a full request identically', () => {
    const req: AgentBridgeRequest = {
      command: 'autograde',
      mode: 'correction',
      model: 'gemini-flash-latest',
      profile: 'vlog',
      theme: 'teal-orange',
      strength: 0.8,
      skinProtection: 0.75,
      chromaGain: 1,
      rounds: 4,
      framePath: '/tmp/cg-agent-frame.bin',
    };
    expect(parseAgentRequest(formatAgentRequest(req))).toEqual(req);
  });

  it('round-trips batch clip lists and the mock flag', () => {
    const req: AgentBridgeRequest = {
      command: 'batch',
      theme: 'warm-film',
      profile: 'rec709',
      clipPaths: ['/a.tif', '/b.tif', '/c.png'],
      mock: true,
    };
    expect(parseAgentRequest(formatAgentRequest(req))).toEqual(req);
  });

  it('ignores unknown keys (forward compatible)', () => {
    const req = parseAgentRequest('command critique\nsomeFutureKey 42\nframe /x.bin\n');
    expect(req.command).toBe('critique');
    expect(req.framePath).toBe('/x.bin');
  });
});

describe('agent bridge protocol - response round-trip', () => {
  it('formats and re-parses a full auto-grade response', () => {
    const resp: AgentBridgeResponse = {
      status: 'ok',
      message: 'applied round 2',
      defects: ['residual color cast (6.2 LAB units)', 'chroma overshoot in highlights'],
      verdict: 'continue',
      bestRound: 2,
      accepted: true,
      stopReason: 'guard rejected round 3 as a regression; kept round 2',
      apply: [
        { field: 'strength', values: [0.8] },
        { field: 'shadowTint', values: [5, -5] },
        { field: 'toneCurve', values: [0, 0, 0.5, 0.55, 1, 1] },
      ],
      unmapped: ['chromaShape.softLimit=22'],
      cost: 0,
      diverged: [],
    };
    const back = parseAgentResponse(formatAgentResponse(resp));
    expect(back.status).toBe('ok');
    expect(back.message).toBe(resp.message);
    expect(back.defects).toEqual(resp.defects);
    expect(back.verdict).toBe('continue');
    expect(back.bestRound).toBe(2);
    expect(back.accepted).toBe(true);
    expect(back.stopReason).toBe(resp.stopReason);
    expect(back.apply).toEqual(resp.apply);
    expect(back.unmapped).toEqual(resp.unmapped);
  });

  it('round-trips a batch response with diverged pairs', () => {
    const resp: AgentBridgeResponse = {
      status: 'ok',
      message: '1 of 3 pair(s) diverged',
      defects: [],
      apply: [],
      unmapped: [],
      diverged: [{ clipA: 'clipA.tif', clipB: 'clipB.tif', reason: 'exposure drift 0.42; cast direction 31deg' }],
      pairsCompared: 3,
    };
    const back = parseAgentResponse(formatAgentResponse(resp));
    expect(back.diverged).toEqual(resp.diverged);
    expect(back.pairsCompared).toBe(3);
  });

  it('errorResponse never has an empty message and parses as error', () => {
    const e = errorResponse('');
    expect(e.status).toBe('error');
    expect(e.message.length).toBeGreaterThan(0);
    const back = parseAgentResponse(formatAgentResponse(errorResponse('no billing on paid model')));
    expect(back.status).toBe('error');
    expect(back.message).toContain('no billing');
  });
});

// The committed fixtures are the cross-language contract: the native side
// (native/tests/editor/agent_bridge_test.cpp) parses these exact files, so both
// engines must agree on the wire format. If this drifts, update BOTH sides.
describe('agent bridge protocol - committed fixtures (native parity)', () => {
  it('parses the canonical request fixture', () => {
    const req = parseAgentRequest(readFileSync(join(FIX, 'request.txt'), 'utf8'));
    expect(req.command).toBe('autograde');
    expect(req.theme).toBe('teal-orange');
    expect(req.profile).toBe('vlog');
    expect(req.strength).toBeCloseTo(0.8);
    expect(req.rounds).toBe(4);
    expect(req.framePath).toBe('/tmp/cg-agent-frame.bin');
  });

  it('parses the canonical response fixture', () => {
    const resp = parseAgentResponse(readFileSync(join(FIX, 'response.txt'), 'utf8'));
    expect(resp.status).toBe('ok');
    expect(resp.defects).toHaveLength(2);
    expect(resp.bestRound).toBe(2);
    expect(resp.apply.find((a) => a.field === 'shadowTint')?.values).toEqual([5, -5]);
    expect(resp.apply.find((a) => a.field === 'toneCurve')?.values).toEqual([0, 0, 0.5, 0.55, 1, 1]);
    expect(resp.unmapped[0]).toContain('softLimit');
  });

  it('parses the batch response fixture', () => {
    const resp = parseAgentResponse(readFileSync(join(FIX, 'batch-response.txt'), 'utf8'));
    expect(resp.diverged).toHaveLength(1);
    expect(resp.diverged[0]!.clipA).toBe('clipA.tif');
    expect(resp.diverged[0]!.reason).toContain('exposure drift');
    expect(resp.pairsCompared).toBe(3);
  });
});

describe('editorApplyFromResult - native composition mapping', () => {
  const base: Theme = {
    name: 'teal-orange',
    description: 't',
    targetStats: {} as never,
    knobs: { strength: { default: 0.8 }, skinProtection: { default: 0.75 } },
    overrides: { chromaGain: 1.2, shadowTint: [2, -2] },
  };
  const baseParams: AutoGradeParams = { strength: 0.8, skinProtection: 0.75, overrides: { ...base.overrides } };

  it('emits tint DELTAS (user tints add onto the theme) and a chromaGain RATIO', () => {
    const best: AutoGradeParams = {
      strength: 0.8,
      skinProtection: 0.75,
      overrides: { chromaGain: 0.6, shadowTint: [5, -5], midtoneTint: [3, -3] },
    };
    const { apply } = editorApplyFromResult(base, best, baseParams);
    // chromaGain ratio = 0.6 / 1.2 = 0.5
    expect(apply.find((a) => a.field === 'chromaGain')?.values[0]).toBeCloseTo(0.5);
    // shadowTint delta = [5,-5] - [2,-2] = [3,-3]
    expect(apply.find((a) => a.field === 'shadowTint')?.values).toEqual([3, -3]);
    // midtoneTint had no authored value -> full [3,-3]
    expect(apply.find((a) => a.field === 'midtoneTint')?.values).toEqual([3, -3]);
    // strength/skin unchanged from baseline -> not emitted
    expect(apply.find((a) => a.field === 'strength')).toBeUndefined();
  });

  it('passes curves through absolutely and discloses softLimit as unmapped', () => {
    const best: AutoGradeParams = {
      strength: 0.6,
      skinProtection: 0.75,
      overrides: {
        toneCurve: [
          [0, 0],
          [0.5, 0.6],
          [1, 1],
        ],
        chromaShape: { softLimit: 22 },
      },
    };
    const { apply, unmapped } = editorApplyFromResult(base, best, baseParams);
    expect(apply.find((a) => a.field === 'toneCurve')?.values).toEqual([0, 0, 0.5, 0.6, 1, 1]);
    // strength moved 0.8 -> 0.6, emitted
    expect(apply.find((a) => a.field === 'strength')?.values[0]).toBeCloseTo(0.6);
    expect(unmapped.some((u) => u.includes('softLimit=22'))).toBe(true);
  });

  it('emits nothing when the best round equals the baseline', () => {
    const { apply, unmapped } = editorApplyFromResult(base, baseParams, baseParams);
    expect(apply).toHaveLength(0);
    expect(unmapped).toHaveLength(0);
  });

  it('discloses chromaGain (not a bogus ratio) when the theme authors chromaGain:0', () => {
    const monoBase: Theme = {
      name: 'monochrome-bw',
      description: 'm',
      targetStats: {} as never,
      knobs: { strength: { default: 1 }, skinProtection: { default: 0 } },
      overrides: { chromaGain: 0 },
    };
    const monoBaseParams: AutoGradeParams = {
      strength: 1,
      skinProtection: 0,
      overrides: { ...monoBase.overrides },
    };
    const best: AutoGradeParams = {
      strength: 1,
      skinProtection: 0,
      overrides: { chromaGain: 0.4 },
    };
    const { apply, unmapped } = editorApplyFromResult(monoBase, best, monoBaseParams);
    expect(apply.find((a) => a.field === 'chromaGain')).toBeUndefined();
    expect(unmapped.some((u) => u.includes('chromaGain=0.4'))).toBe(true);
  });
});
