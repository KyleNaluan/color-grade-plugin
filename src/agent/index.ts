/**
 * Agent-in-the-loop auto-grade (issue: cg-agent-loop-v2). Reusable, Node/CEP-free
 * seam consumed by the offline batch CLI (`scripts/autoGrade.ts`) today and the
 * BYOK agent panel later. See each module's header for the design and the
 * evidence behind it (`data/cg-agent-grade-s7`, `data/cg-agents-study`).
 */
export * from './types.js';
export * from './pricing.js';
export * from './apiKey.js';
export * from './critic.js';
export * from './loop.js';
export * from './bridgeProtocol.js';
export * from './editorApply.js';
