/**
 * The native-editor <-> TS-agent execution bridge protocol (issue: cg-agent-wiring).
 *
 * The native editor cannot run the agent pipeline itself: the vision critic, the
 * rule-based `gradeGuard`, the auto-grade loop, cross-clip consistency, and the
 * reference-stats measurement all live in TypeScript (`src/agent` + `src/core`),
 * which is the locked correctness oracle - porting the Gemini HTTP + structured
 * output + guard classification into C++ would duplicate that oracle and break
 * the native epic's parity discipline. So the editor SPAWNS a short-lived Node
 * subprocess running `scripts/agentBridge.ts`, which reuses this exact TS code
 * and returns a structured result. See `native/docs/adr-agent-execution.md`.
 *
 * The wire format is deliberately a tiny, line-based `key value` text - the same
 * hand-rolled-parser spirit as the `.cube` and reference-stats sidecar formats -
 * so the C++ side (`native/ColorGradeFX/editor/AgentBridge.h`) needs NO JSON
 * library and both ends stay trivially parseable and headless-testable. The API
 * key never rides this file: it is passed to the subprocess via the
 * `GEMINI_API_KEY` environment variable only (BYOK), never written to disk.
 *
 * This module is Node/CEP-free (pure string <-> struct) like the rest of
 * `src/agent`; the actual I/O and model calls live in `scripts/agentBridge.ts`.
 */

/** The four agent surfaces the editor buttons trigger. */
export type AgentCommand = 'critique' | 'autograde' | 'reference' | 'batch';

/** A request the native editor writes and the bridge CLI reads. */
export interface AgentBridgeRequest {
  command: AgentCommand;
  /** correction (absolute) vs shot-match (against a reference). Critique/autograde. */
  mode?: 'correction' | 'shot-match';
  /** Gemini model id; defaults applied by the CLI when omitted. */
  model?: string;
  /** Footage decode profile key (vlog/rec709/slog3/...). */
  profile?: string;
  /** Theme registry key for the auto-grade base look; 'stats-only' or unknown = self-target. */
  theme?: string;
  strength?: number;
  skinProtection?: number;
  chromaGain?: number;
  /** Max tuned auto-grade rounds after the baseline. */
  rounds?: number;
  /**
   * Path to the frame the command operates on. Critique: the graded preview
   * frame the user sees. Autograde: the decoded-source frame the loop re-renders.
   * Carried as a raw RGBA dump (see `readFrameDump`), never a codec file, because
   * native has no image encoder.
   */
  framePath?: string;
  /**
   * Reference IMAGE file (TIFF/PNG the user picked). For `reference` it is the
   * still to measure; for shot-match critique/autograde it is the look to match.
   */
  referencePath?: string;
  /** Output path for the reference-stats sidecar (`reference` command). */
  outPath?: string;
  /** Clip image files for the `batch` cross-clip consistency check. */
  clipPaths?: string[];
  /** Run with a scripted critic (no network, $0) - used by tests and dry runs. */
  mock?: boolean;
}

/** A single param edit the editor should apply to the recipe after auto-grade. */
export interface AgentApplyEdit {
  field:
    | 'strength'
    | 'skinProtection'
    | 'chromaGain'
    | 'shadowTint'
    | 'midtoneTint'
    | 'highlightTint'
    | 'toneCurve'
    | 'channelR'
    | 'channelG'
    | 'channelB';
  /** Scalar edits use one value; tints use two; curves use a flat [x0,y0,x1,y1,...]. */
  values: number[];
}

/** One diverged clip pair from the batch consistency check. */
export interface AgentDivergedPair {
  clipA: string;
  clipB: string;
  reason: string;
}

/** The result the bridge CLI writes and the native editor reads. */
export interface AgentBridgeResponse {
  status: 'ok' | 'error';
  /** Human-readable summary (ok) or the specific failure (error). Never empty on error. */
  message: string;
  /** Named visible defects (critique / each auto-grade round's baseline). */
  defects: string[];
  /** Critic advisory verdict (critique / autograde). */
  verdict?: 'continue' | 'stop';
  /** Auto-grade: the accepted best round index (0 = baseline kept). */
  bestRound?: number;
  /** Auto-grade: whether any tuned round was accepted over the baseline. */
  accepted?: boolean;
  /** Auto-grade: why the loop stopped. */
  stopReason?: string;
  /** Auto-grade: edits to apply to the recipe (only the editor-applicable subset). */
  apply: AgentApplyEdit[];
  /**
   * Auto-grade: knobs the critic proposed that have no editor home (e.g.
   * chromaShape.softLimit). Surfaced so nothing is silently dropped.
   */
  unmapped: string[];
  /** Estimated USD spend (0 on free tier / mock). */
  cost?: number;
  /** Batch: pairs that diverged. */
  diverged: AgentDivergedPair[];
  /** Batch: count of pairs compared. */
  pairsCompared?: number;
}

// --- number formatting: stable, round-trippable, locale-free -----------------

function num(v: number): string {
  if (!Number.isFinite(v)) return '0';
  // Trim to a compact but faithful decimal; native parses with strtod.
  return Number(v.toFixed(6)).toString();
}

function sanitizeLine(s: string): string {
  return s.replace(/[\r\n]+/g, ' ').trim();
}

// --- request: format (test/native mirror) + parse (bridge CLI) ---------------

export function formatAgentRequest(req: AgentBridgeRequest): string {
  const lines: string[] = [];
  lines.push(`command ${req.command}`);
  if (req.mode) lines.push(`mode ${req.mode}`);
  if (req.model) lines.push(`model ${sanitizeLine(req.model)}`);
  if (req.profile) lines.push(`profile ${sanitizeLine(req.profile)}`);
  if (req.theme) lines.push(`theme ${sanitizeLine(req.theme)}`);
  if (req.strength !== undefined) lines.push(`strength ${num(req.strength)}`);
  if (req.skinProtection !== undefined) lines.push(`skinProtection ${num(req.skinProtection)}`);
  if (req.chromaGain !== undefined) lines.push(`chromaGain ${num(req.chromaGain)}`);
  if (req.rounds !== undefined) lines.push(`rounds ${Math.round(req.rounds)}`);
  if (req.framePath) lines.push(`frame ${req.framePath}`);
  if (req.referencePath) lines.push(`reference ${req.referencePath}`);
  if (req.outPath) lines.push(`out ${req.outPath}`);
  for (const c of req.clipPaths ?? []) lines.push(`clip ${c}`);
  if (req.mock) lines.push(`mock 1`);
  return lines.join('\n') + '\n';
}

export function parseAgentRequest(text: string): AgentBridgeRequest {
  const req: AgentBridgeRequest = { command: 'critique', apply: [] as never } as AgentBridgeRequest;
  const clips: string[] = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line) continue;
    const sp = line.indexOf(' ');
    const key = sp < 0 ? line : line.slice(0, sp);
    const val = sp < 0 ? '' : line.slice(sp + 1).trim();
    switch (key) {
      case 'command':
        req.command = val as AgentCommand;
        break;
      case 'mode':
        req.mode = val === 'shot-match' ? 'shot-match' : 'correction';
        break;
      case 'model':
        req.model = val;
        break;
      case 'profile':
        req.profile = val;
        break;
      case 'theme':
        req.theme = val;
        break;
      case 'strength':
        req.strength = Number(val);
        break;
      case 'skinProtection':
        req.skinProtection = Number(val);
        break;
      case 'chromaGain':
        req.chromaGain = Number(val);
        break;
      case 'rounds':
        req.rounds = parseInt(val, 10);
        break;
      case 'frame':
        req.framePath = val;
        break;
      case 'reference':
        req.referencePath = val;
        break;
      case 'out':
        req.outPath = val;
        break;
      case 'clip':
        clips.push(val);
        break;
      case 'mock':
        req.mock = val !== '0' && val !== '';
        break;
      default:
        break; // forward-compatible: ignore unknown keys
    }
  }
  if (clips.length) req.clipPaths = clips;
  // strip the accidental apply seed used only to satisfy the initializer above
  delete (req as unknown as Record<string, unknown>).apply;
  return req;
}

// --- response: format (bridge CLI) + parse (test/native mirror) --------------

export function formatAgentResponse(resp: AgentBridgeResponse): string {
  const lines: string[] = [];
  lines.push(`status ${resp.status}`);
  lines.push(`message ${sanitizeLine(resp.message)}`);
  for (const d of resp.defects) lines.push(`defect ${sanitizeLine(d)}`);
  if (resp.verdict) lines.push(`verdict ${resp.verdict}`);
  if (resp.bestRound !== undefined) lines.push(`bestRound ${Math.round(resp.bestRound)}`);
  if (resp.accepted !== undefined) lines.push(`accepted ${resp.accepted ? 'true' : 'false'}`);
  if (resp.stopReason) lines.push(`stopReason ${sanitizeLine(resp.stopReason)}`);
  for (const e of resp.apply) lines.push(`apply ${e.field} ${e.values.map(num).join(' ')}`);
  for (const u of resp.unmapped) lines.push(`unmapped ${sanitizeLine(u)}`);
  if (resp.cost !== undefined) lines.push(`cost ${num(resp.cost)}`);
  for (const p of resp.diverged) lines.push(`diverged ${sanitizeLine(p.clipA)} | ${sanitizeLine(p.clipB)} | ${sanitizeLine(p.reason)}`);
  if (resp.pairsCompared !== undefined) lines.push(`pairsCompared ${Math.round(resp.pairsCompared)}`);
  return lines.join('\n') + '\n';
}

const APPLY_FIELDS = new Set<AgentApplyEdit['field']>([
  'strength',
  'skinProtection',
  'chromaGain',
  'shadowTint',
  'midtoneTint',
  'highlightTint',
  'toneCurve',
  'channelR',
  'channelG',
  'channelB',
]);

export function parseAgentResponse(text: string): AgentBridgeResponse {
  const resp: AgentBridgeResponse = {
    status: 'error',
    message: '',
    defects: [],
    apply: [],
    unmapped: [],
    diverged: [],
  };
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line) continue;
    const sp = line.indexOf(' ');
    const key = sp < 0 ? line : line.slice(0, sp);
    const val = sp < 0 ? '' : line.slice(sp + 1).trim();
    switch (key) {
      case 'status':
        resp.status = val === 'ok' ? 'ok' : 'error';
        break;
      case 'message':
        resp.message = val;
        break;
      case 'defect':
        resp.defects.push(val);
        break;
      case 'verdict':
        resp.verdict = val === 'stop' ? 'stop' : 'continue';
        break;
      case 'bestRound':
        resp.bestRound = parseInt(val, 10);
        break;
      case 'accepted':
        resp.accepted = val === 'true';
        break;
      case 'stopReason':
        resp.stopReason = val;
        break;
      case 'apply': {
        const toks = val.split(/\s+/).filter((t) => t.length > 0);
        const field = toks[0] as AgentApplyEdit['field'];
        if (APPLY_FIELDS.has(field)) {
          resp.apply.push({ field, values: toks.slice(1).map(Number) });
        }
        break;
      }
      case 'unmapped':
        resp.unmapped.push(val);
        break;
      case 'cost':
        resp.cost = Number(val);
        break;
      case 'diverged': {
        const parts = val.split('|').map((s) => s.trim());
        resp.diverged.push({ clipA: parts[0] ?? '', clipB: parts[1] ?? '', reason: parts[2] ?? '' });
        break;
      }
      case 'pairsCompared':
        resp.pairsCompared = parseInt(val, 10);
        break;
      default:
        break;
    }
  }
  return resp;
}

// --- error helper ------------------------------------------------------------

export function errorResponse(message: string): AgentBridgeResponse {
  return {
    status: 'error',
    message: sanitizeLine(message) || 'unknown error',
    defects: [],
    apply: [],
    unmapped: [],
    diverged: [],
  };
}
