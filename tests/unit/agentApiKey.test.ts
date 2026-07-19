import { describe, expect, it } from 'vitest';
import { maskKey, looksLikeGeminiKey, validateGeminiKey, type FetchLike } from '../../src/agent/index.js';

describe('maskKey', () => {
  it('shows first 4 and last 4, redacts the middle', () => {
    const masked = maskKey('AIzaSyABCDEFGHIJKLMNOPQRSTUV1234567890');
    expect(masked.startsWith('AIza')).toBe(true);
    expect(masked.endsWith('7890')).toBe(true);
    expect(masked).toContain('*');
    expect(masked).not.toContain('SyABCDEFG');
  });

  it('fully redacts short strings and handles empty', () => {
    expect(maskKey('short')).toBe('*****');
    expect(maskKey('')).toBe('');
    expect(maskKey('  AIza1234567890abcdef  ')).toContain('AIza'); // trims first
  });
});

describe('looksLikeGeminiKey', () => {
  it('accepts AIza- and AQ.-prefixed keys and rejects junk', () => {
    expect(looksLikeGeminiKey('AIzaSyABCDEFGHIJKLMNOPQRSTUVWX1234567')).toBe(true);
    expect(looksLikeGeminiKey('AQ.Ab8ABCDEFGHIJKLMNOPQRSTUVWX1234567')).toBe(true);
    expect(looksLikeGeminiKey('sk-openai-style')).toBe(false);
    expect(looksLikeGeminiKey('')).toBe(false);
  });
});

describe('validateGeminiKey', () => {
  it('returns ok and available models from a zero-cost listModels call', async () => {
    const fetchImpl: FetchLike = async (url) => {
      expect(url).toContain('/models?key=');
      return {
        ok: true,
        status: 200,
        json: async () => ({ models: [{ name: 'models/gemini-flash-latest' }, { name: 'models/gemini-3-flash-preview' }] }),
      };
    };
    const status = await validateGeminiKey('AIza-test', { fetchImpl });
    expect(status.ok).toBe(true);
    expect(status.availableModels).toContain('gemini-flash-latest');
    expect(status.generationConfirmed).toBe(false); // no probe requested
    // paid-grade model is reachable but tier stays unknown without a generation probe.
    expect(status.tier).toBe('unknown');
  });

  it('reports tier "free" when no paid-grade model is reachable', async () => {
    const fetchImpl: FetchLike = async () => ({
      ok: true,
      status: 200,
      json: async () => ({ models: [{ name: 'models/gemini-flash-latest' }] }),
    });
    const status = await validateGeminiKey('AIza-test', { fetchImpl });
    expect(status.tier).toBe('free');
  });

  it('confirms generation and reports tier "paid" when probe succeeds and paid-grade model is reachable', async () => {
    const fetchImpl: FetchLike = async (url) => {
      if (url.includes(':generateContent')) {
        return { ok: true, status: 200, json: async () => ({ candidates: [{}], usageMetadata: { promptTokenCount: 5 } }) };
      }
      return {
        ok: true,
        status: 200,
        json: async () => ({ models: [{ name: 'models/gemini-3-flash-preview' }, { name: 'models/gemini-flash-latest' }] }),
      };
    };
    const status = await validateGeminiKey('AIza-test', { fetchImpl, probe: true });
    expect(status.generationConfirmed).toBe(true);
    expect(status.tier).toBe('paid');
    expect(status.usage?.promptTokenCount).toBe(5);
  });

  it('returns an error (never throws) on an invalid key', async () => {
    const fetchImpl: FetchLike = async () => ({
      ok: false,
      status: 400,
      json: async () => ({ error: { message: 'API key not valid' } }),
    });
    const status = await validateGeminiKey('bad', { fetchImpl });
    expect(status.ok).toBe(false);
    expect(status.error).toMatch(/not valid/);
  });

  it('short-circuits on an empty key', async () => {
    const status = await validateGeminiKey('   ');
    expect(status.ok).toBe(false);
    expect(status.error).toMatch(/empty/);
  });
});
