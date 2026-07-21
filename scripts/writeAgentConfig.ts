/**
 * writeAgentConfig.ts - seed the native editor's persisted agent settings so the
 * agent surfaces (Critique / Auto-grade / Reference / Batch) work "as installed"
 * with NO per-session environment variable (issue: cg-agent-fixes-v4).
 *
 * The AE panel process never inherits a shell's `CG_AGENT_BRIDGE`, so exporting it
 * before launching AE (the old requirement) is impossible from Explorer and the
 * feature was dead ("agent bridge not configured"). The plug-in now reads the bridge
 * path + launcher from a per-user settings file:
 *
 *   %APPDATA%\ColorGradeFX\agent.cfg   (key=value text; parsed by AgentBridge.h)
 *
 * Run this ONCE from the Node checkout that will actually run the bridge (the one
 * with `node_modules` installed - `tsx` must resolve there):
 *
 *   npm run native:agent-config            # points bridge= at this repo's scripts/agentBridge.ts
 *   npm run native:agent-config -- --bridge <path> [--node "<launcher>"]
 *
 * It writes bridge=/node= while PRESERVING an existing apiKeyEnc (the DPAPI-encrypted
 * BYOK key the panel stores) so re-seeding never wipes the saved key. `CG_AGENT_BRIDGE`
 * / `CG_AGENT_NODE` remain OVERRIDES if you prefer env for CI / power use.
 *
 * Cross-boundary aware: run under native Windows Node (uses %APPDATA% directly) OR
 * under WSL (derives %APPDATA% via cmd.exe and converts paths with wslpath). When run
 * under WSL the stored bridge path is the Windows form so Windows Node can launch it.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, '..');
const isWsl =
  process.platform === 'linux' &&
  (() => {
    try {
      return /microsoft/i.test(readFileSync('/proc/version', 'utf8'));
    } catch {
      return false;
    }
  })();

/** A path in the form Windows Node/cmd can open (drive-letter path). */
function toWindowsPath(p: string): string {
  const abs = resolve(p);
  if (process.platform === 'win32') return abs;
  try {
    return execFileSync('wslpath', ['-w', abs], { encoding: 'utf8' }).trim();
  } catch {
    return abs;
  }
}

/** A path this (possibly-WSL) process can open, from a Windows drive-letter path. */
function fromWindowsPath(win: string): string {
  if (process.platform === 'win32') return win;
  try {
    return execFileSync('wslpath', ['-u', win], { encoding: 'utf8' }).trim();
  } catch {
    return win;
  }
}

/** %APPDATA% as a Windows path (for reporting) and a local-FS path (for read/write). */
function appDataPaths(): { win: string; local: string } {
  if (process.env.APPDATA) {
    const win = process.env.APPDATA;
    return { win, local: isWsl ? fromWindowsPath(win) : win };
  }
  // WSL without APPDATA in env: ask Windows for it.
  const out = execFileSync('cmd.exe', ['/c', 'echo %APPDATA%'], { encoding: 'utf8' }).trim();
  if (!out || out.includes('%APPDATA%')) {
    throw new Error('could not determine %APPDATA% (set APPDATA or run from Windows Node)');
  }
  return { win: out, local: fromWindowsPath(out) };
}

/** Parse the tiny key=value settings file (same contract as AgentBridge.h). */
function parseCfg(text: string): Map<string, string> {
  const m = new Map<string, string>();
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    m.set(line.slice(0, eq).trim(), line.slice(eq + 1).trim());
  }
  return m;
}

function formatCfg(m: Map<string, string>): string {
  let out =
    '# ColorGradeFX agent settings (auto-managed). Do not commit; contains a\n' +
    '# DPAPI-protected API key (apiKeyEnc). bridge/node point at the Node bridge.\n';
  for (const [k, v] of m) if (v) out += `${k}=${v}\n`;
  return out;
}

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

function main(): void {
  if (process.argv.includes('--help') || process.argv.includes('-h')) {
    console.log(
      'usage: npm run native:agent-config -- [--bridge <path>] [--node "<launcher>"]\n' +
        '  --bridge  runnable bridge (default: this repo scripts/agentBridge.ts)\n' +
        '  --node    launcher override (default: none; a .ts bridge auto-uses "npx tsx")',
    );
    return;
  }

  const bridgeLocal = arg('bridge') ?? join(repoRoot, 'scripts', 'agentBridge.ts');
  if (!existsSync(resolve(bridgeLocal))) {
    throw new Error(`bridge not found: ${bridgeLocal}`);
  }
  const bridgeWin = toWindowsPath(bridgeLocal);
  const node = arg('node');

  const { win: appdataWin, local: appdataLocal } = appDataPaths();
  const dirLocal = join(appdataLocal, 'ColorGradeFX');
  const fileLocal = join(dirLocal, 'agent.cfg');
  mkdirSync(dirLocal, { recursive: true });

  const cfg = existsSync(fileLocal) ? parseCfg(readFileSync(fileLocal, 'utf8')) : new Map<string, string>();
  cfg.set('bridge', bridgeWin);
  if (node) cfg.set('node', node);
  writeFileSync(fileLocal, formatCfg(cfg));

  console.log('wrote agent settings:');
  console.log(`  file    ${appdataWin}\\ColorGradeFX\\agent.cfg`);
  console.log(`  bridge  ${bridgeWin}`);
  console.log(`  node    ${node ?? '(default; .ts -> npx tsx)'}`);
  if (cfg.get('apiKeyEnc')) console.log('  (existing saved API key preserved)');
  console.log('\nRestart After Effects (or reopen the editor) to pick it up.');
}

main();
