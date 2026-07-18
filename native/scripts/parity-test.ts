/*
 * parity-test.ts - cross-engine golden test for the ported LUT-apply path.
 *
 * Bakes a real theme grade LUT with the TS engine, runs the TS sampleLut (the oracle)
 * over a deterministic set of sample points, compiles + runs the C++ port
 * (native/tests/parity/parity_test.cpp -> CubeLut.h) over the same .cube + points,
 * and asserts agreement within TOL (~1e-4). This is the correctness gate for the
 * native LUT apply and the seed of the Phase 2 full cross-engine harness.
 *
 * Runs locally (WSL) / on any host with g++ or clang; it is NOT wired into CI (per
 * the Phase 1 brief: do not gate CI on the C++ build). Invoke: npm run native:parity
 */
import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { bakeGradeLut } from '../../src/core/lut/gradeLut.js';
import { writeCube, sampleLut } from '../../src/core/lut/cube.js';
import { THEMES } from '../../src/themes/index.js';
import type { Vec3 } from '../../src/core/color/types.js';

const TOL = 1e-4;
const here = dirname(fileURLToPath(import.meta.url));
const cppSource = join(here, '..', 'tests', 'parity', 'parity_test.cpp');

function findCompiler(): string {
  for (const cc of ['g++', 'clang++']) {
    try {
      execFileSync(cc, ['--version'], { stdio: 'ignore' });
      return cc;
    } catch {
      /* try next */
    }
  }
  throw new Error('parity-test: no C++ compiler (g++/clang++) found on PATH');
}

// Deterministic sample set: a coarse regular grid (hits node-aligned and mid-cell
// points) plus seeded pseudo-random points (hits arbitrary interpolation weights).
function sampleInputs(): Vec3[] {
  const pts: Vec3[] = [];
  const grid = 7;
  for (let i = 0; i < grid; i++)
    for (let j = 0; j < grid; j++)
      for (let k = 0; k < grid; k++)
        pts.push([i / (grid - 1), j / (grid - 1), k / (grid - 1)]);
  // Include out-of-range values to exercise the clamp in both engines.
  pts.push([-0.3, 0.5, 1.4], [1.2, -0.1, 0.2], [0, 0, 0], [1, 1, 1]);
  let seed = 0x9e3779b9;
  const rand = () => {
    seed ^= seed << 13;
    seed ^= seed >>> 17;
    seed ^= seed << 5;
    return ((seed >>> 0) % 1_000_000) / 1_000_000;
  };
  for (let n = 0; n < 500; n++) pts.push([rand(), rand(), rand()]);
  return pts;
}

function main(): void {
  const cc = findCompiler();
  if (!existsSync(cppSource)) throw new Error(`parity-test: missing ${cppSource}`);

  // Bake the same LUT the embedded header uses (teal-orange grade over warm-film stats).
  const lut = bakeGradeLut(THEMES['warm-film']!.targetStats, THEMES['teal-orange']!, {}, 33);
  const cubeText = writeCube(lut);

  const pts = sampleInputs();

  const tmp = mkdtempSync(join(tmpdir(), 'cg-parity-'));
  const cubePath = join(tmp, 'grade.cube');
  const inputsPath = join(tmp, 'inputs.txt');
  const binPath = join(tmp, process.platform === 'win32' ? 'parity.exe' : 'parity');
  writeFileSync(cubePath, cubeText);
  writeFileSync(inputsPath, pts.map((p) => `${p[0]} ${p[1]} ${p[2]}`).join('\n') + '\n');

  execFileSync(cc, ['-O2', '-std=c++17', cppSource, '-o', binPath], { stdio: 'inherit' });
  const cppOut = execFileSync(binPath, [cubePath, inputsPath], { encoding: 'utf8' })
    .trim()
    .split('\n')
    .map((line) => line.split(/\s+/).map(Number) as Vec3);

  if (cppOut.length !== pts.length) {
    throw new Error(`parity-test: C++ returned ${cppOut.length} rows, expected ${pts.length}`);
  }

  let maxErr = 0;
  let worst = '';
  for (let i = 0; i < pts.length; i++) {
    const ref = sampleLut(lut, pts[i]!); // TS oracle
    const got = cppOut[i]!;
    for (let c = 0; c < 3; c++) {
      const err = Math.abs(ref[c]! - got[c]!);
      if (err > maxErr) {
        maxErr = err;
        worst = `in=[${pts[i]!.map((v) => v.toFixed(4))}] ch${c} ts=${ref[c]!.toFixed(8)} cpp=${got[c]!.toFixed(8)}`;
      }
    }
  }

  console.log(`parity-test: compiler=${cc} points=${pts.length} maxAbsErr=${maxErr.toExponential(3)} (tol=${TOL})`);
  console.log(`  worst: ${worst}`);
  if (maxErr > TOL) {
    console.error(`parity-test: FAIL - max error ${maxErr} exceeds tolerance ${TOL}`);
    process.exit(1);
  }
  console.log('parity-test: PASS');
}

main();
