import { describe, expect, it } from 'vitest';
import { PRICING, costForUsage, estimateLoopCost, isPaidModel, DEFAULT_MODEL, PAID_MODEL } from '../../src/agent/index.js';

describe('pricing', () => {
  it('default model is the free tier, paid model is billed', () => {
    expect(PRICING[DEFAULT_MODEL]!.free).toBe(true);
    expect(isPaidModel(DEFAULT_MODEL)).toBe(false);
    expect(isPaidModel(PAID_MODEL)).toBe(true);
  });

  it('free/unknown models cost $0', () => {
    expect(costForUsage(DEFAULT_MODEL, { promptTokenCount: 1000, candidatesTokenCount: 500 })).toBe(0);
    expect(costForUsage('some-unknown-model', { promptTokenCount: 1e6 })).toBe(0);
  });

  it('paid model priced per verified 2026-07-19 rates ($0.50 in / $3.00 out, thinking billed as output)', () => {
    // 1M input + 1M output = $0.50 + $3.00 = $3.50
    const cost = costForUsage(PAID_MODEL, { promptTokenCount: 1e6, candidatesTokenCount: 1e6 });
    expect(cost).toBeCloseTo(3.5, 6);
    // thinking tokens count as output
    const withThoughts = costForUsage(PAID_MODEL, { promptTokenCount: 0, candidatesTokenCount: 5e5, thoughtsTokenCount: 5e5 });
    expect(withThoughts).toBeCloseTo(3.0, 6);
  });

  it('estimateLoopCost is $0 free and a small positive number paid', () => {
    expect(estimateLoopCost(DEFAULT_MODEL, 5)).toBe(0);
    const est = estimateLoopCost(PAID_MODEL, 5);
    expect(est).toBeGreaterThan(0);
    expect(est).toBeLessThan(0.05); // ~6 calls, well under a cent-scale envelope
  });
});
