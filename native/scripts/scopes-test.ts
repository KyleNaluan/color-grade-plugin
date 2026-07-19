/*
 * scopes-test.ts - compile + run the headless Phase 5 editor-scopes core unit test
 * (native/tests/editor/scopes_test.cpp) with a local g++/clang, OUTSIDE AE.
 *
 * The scopes core (Scopes.h: histogram binning + waveform/vectorscope image synthesis)
 * is new native code with no TS oracle, so the C++ test self-asserts and this runner only
 * checks its exit code. Same convention as native:preview-test / native:analysis-test:
 * local compiler, deliberately NOT wired into CI (the C++ build stays un-gated).
 *
 *   npm run native:scopes-test
 */
import { execFileSync } from 'node:child_process';
import { mkdtempSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const cppSource = join(here, '..', 'tests', 'editor', 'scopes_test.cpp');

function findCompiler(): string {
  for (const cc of ['g++', 'clang++']) {
    try {
      execFileSync(cc, ['--version'], { stdio: 'ignore' });
      return cc;
    } catch {
      /* try next */
    }
  }
  throw new Error('scopes-test: no C++ compiler (g++/clang++) found on PATH');
}

function main(): void {
  if (!existsSync(cppSource)) throw new Error(`scopes-test: missing ${cppSource}`);
  const cc = findCompiler();
  const tmp = mkdtempSync(join(tmpdir(), 'cg-scopes-'));
  const bin = join(tmp, process.platform === 'win32' ? 'scopes_test.exe' : 'scopes_test');
  console.log(`scopes-test: compiler=${cc}`);
  execFileSync(cc, ['-O2', '-std=c++17', '-pthread', cppSource, '-o', bin], { stdio: 'inherit' });
  execFileSync(bin, [], { stdio: 'inherit' }); // throws (non-zero exit) on any failed check
}

main();
