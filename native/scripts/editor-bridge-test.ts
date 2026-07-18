/*
 * editor-bridge-test.ts - compile + run the headless editor-bridge unit test
 * (native/tests/editor/bridge_test.cpp) with a local g++/clang, OUTSIDE AE.
 *
 * The editor<->effect bridge's pure logic (EditorBridge.h: the thread-safe edit
 * queue, the percent<->fraction mapping/clamps, and applyEdit) has no TS oracle -
 * it is new native code - so the C++ test self-asserts and this runner only checks
 * its exit code. Same convention as native:parity / native:core-parity: local
 * compiler, deliberately NOT wired into CI (the C++ build stays un-gated).
 *
 *   npm run native:editor-test
 */
import { execFileSync } from 'node:child_process';
import { mkdtempSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const cppSource = join(here, '..', 'tests', 'editor', 'bridge_test.cpp');

function findCompiler(): string {
  for (const cc of ['g++', 'clang++']) {
    try {
      execFileSync(cc, ['--version'], { stdio: 'ignore' });
      return cc;
    } catch {
      /* try next */
    }
  }
  throw new Error('editor-bridge-test: no C++ compiler (g++/clang++) found on PATH');
}

function main(): void {
  if (!existsSync(cppSource)) throw new Error(`editor-bridge-test: missing ${cppSource}`);
  const cc = findCompiler();
  const tmp = mkdtempSync(join(tmpdir(), 'cg-editor-bridge-'));
  const bin = join(tmp, process.platform === 'win32' ? 'bridge_test.exe' : 'bridge_test');
  console.log(`editor-bridge-test: compiler=${cc}`);
  execFileSync(cc, ['-O2', '-std=c++17', '-pthread', cppSource, '-o', bin], { stdio: 'inherit' });
  execFileSync(bin, [], { stdio: 'inherit' }); // throws (non-zero exit) on any failed check
}

main();
