/*
 * agent-bridge-test.ts - compile + run the headless agent-bridge unit test
 * (native/tests/editor/agent_bridge_test.cpp) with a local g++/clang, OUTSIDE AE
 * and without spawning the Node bridge.
 *
 * The agent-bridge seam's pure logic (AgentBridge.h: the line-based wire
 * format + translateAgentApply) is new native code mirroring
 * src/agent/bridgeProtocol.ts. The committed fixtures in
 * native/tests/fixtures/agent/ are the cross-language contract - this test and
 * tests/unit/agentBridge.test.ts both parse them, so a format drift fails one
 * side. Same convention as native:editor-test: local compiler, deliberately NOT
 * wired into CI (the C++ build stays un-gated).
 *
 *   npm run native:agent-test
 */
import { execFileSync } from 'node:child_process';
import { mkdtempSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const cppSource = join(here, '..', 'tests', 'editor', 'agent_bridge_test.cpp');
const fixDir = resolve(here, '..', 'tests', 'fixtures', 'agent');

function findCompiler(): string {
  for (const cc of ['g++', 'clang++']) {
    try {
      execFileSync(cc, ['--version'], { stdio: 'ignore' });
      return cc;
    } catch {
      /* try next */
    }
  }
  throw new Error('agent-bridge-test: no C++ compiler (g++/clang++) found on PATH');
}

function main(): void {
  if (!existsSync(cppSource)) throw new Error(`agent-bridge-test: missing ${cppSource}`);
  const cc = findCompiler();
  const tmp = mkdtempSync(join(tmpdir(), 'cg-agent-bridge-'));
  const bin = join(tmp, process.platform === 'win32' ? 'agent_bridge_test.exe' : 'agent_bridge_test');
  console.log(`agent-bridge-test: compiler=${cc}`);
  execFileSync(cc, ['-O2', '-std=c++17', '-pthread', cppSource, '-o', bin], { stdio: 'inherit' });
  execFileSync(bin, [fixDir], { stdio: 'inherit' }); // throws (non-zero exit) on any failed check
}

main();
