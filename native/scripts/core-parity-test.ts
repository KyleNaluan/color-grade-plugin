/*
 * core-parity-test.ts - Phase 2 cross-engine golden harness.
 *
 * Proves the C++ core port (native/ColorGradeFX/core/*.h) matches the TS oracle
 * (src/core/*) within TOL (~1e-4) end to end:
 *   - computeStats over representative synthetic frames,
 *   - bakeGradeLut(computeStats(frame), theme, opts) across all shipping themes
 *     and the typed knob space (strength / skinProtection overrides), and
 *   - bakeDecodeLut for every profile in PROFILES.
 *
 * The TS side generates the golden vectors; the C++ side (tests/parity/core_parity.cpp)
 * replays the SAME chain natively and writes its results as raw f32/f64 buffers,
 * which we diff here. Themes are baked by NAME in both engines, so a transcription
 * drift in the C++ Themes.h surfaces as a grade-parity failure.
 *
 * Extends the Phase 1 pattern (native/scripts/parity-test.ts): local WSL/host g++
 * or clang, NOT wired into CI (the C++ build stays un-gated). Invoke:
 *   npm run native:core-parity
 */
import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, readFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { computeStats, type FootageStats } from '../../src/core/analysis/stats.js';
import {
  toneStretchChromaGuard,
  buildTransform,
  NEUTRAL_MANUAL,
  type ManualGrade,
} from '../../src/core/engine/engine.js';
import { bakeGradeLut } from '../../src/core/lut/gradeLut.js';
import { bakeDecodeLut } from '../../src/core/lut/decodeLut.js';
import { bakeLut, sampleLut, type Lut3D } from '../../src/core/lut/cube.js';
import { decodePixelToRec709 } from '../../src/core/color/decode.js';
import type { EngineOptions } from '../../src/core/engine/engine.js';
import { THEMES } from '../../src/themes/index.js';
import { PROFILES } from '../../src/core/color/index.js';

const TOL = 1e-4;
const here = dirname(fileURLToPath(import.meta.url));
const cppSource = join(here, '..', 'tests', 'parity', 'core_parity.cpp');

function findCompiler(): string {
  for (const cc of ['g++', 'clang++']) {
    try {
      execFileSync(cc, ['--version'], { stdio: 'ignore' });
      return cc;
    } catch {
      /* try next */
    }
  }
  throw new Error('core-parity-test: no C++ compiler (g++/clang++) found on PATH');
}

// Deterministic xorshift PRNG (matches the style in parity-test.ts).
function makeRand(seed: number): () => number {
  let s = seed >>> 0;
  return () => {
    s ^= s << 13;
    s ^= s >>> 17;
    s ^= s << 5;
    return ((s >>> 0) % 1_000_000) / 1_000_000;
  };
}

// Build a set of representative synthetic frames (interleaved RGB float32, [0,1]).
// Each exercises a different corner: skin presence, clipping, dark/noir, mixed.
function makeFrames(): { name: string; pixels: Float32Array }[] {
  const N = 4096; // pixels per frame
  const frames: { name: string; pixels: Float32Array }[] = [];

  const mixed = new Float32Array(N * 3);
  const r1 = makeRand(0x1234abcd);
  for (let i = 0; i < mixed.length; i++) mixed[i] = r1();
  frames.push({ name: 'mixed-random', pixels: mixed });

  // Skin-heavy: warm mid-tone pixels near the vectorscope skin line, jittered,
  // so src.skinPresence > 0.02 and the engine's skin-protection branch engages.
  const skin = new Float32Array(N * 3);
  const r2 = makeRand(0x55aa33cc);
  for (let p = 0; p < N; p++) {
    const j = (v: number) => Math.min(1, Math.max(0, v + (r2() - 0.5) * 0.12));
    skin[p * 3] = j(0.74);
    skin[p * 3 + 1] = j(0.55);
    skin[p * 3 + 2] = j(0.45);
  }
  frames.push({ name: 'skin-heavy', pixels: skin });

  // Dark / noir: crushed values with occasional clipping-low pixels.
  const dark = new Float32Array(N * 3);
  const r3 = makeRand(0x0f0f7777);
  for (let p = 0; p < N; p++) {
    const base = r3() * 0.3;
    dark[p * 3] = base * r3();
    dark[p * 3 + 1] = base * r3();
    dark[p * 3 + 2] = base * (0.5 + 0.5 * r3());
  }
  frames.push({ name: 'dark-noir', pixels: dark });

  // Low-dynamic-range / degraded: a narrow luma band around the mids, so the
  // stat-match tone curve stretches it hard toward the wide theme targets and
  // the automatic chroma-overshoot guard (toneStretchChromaGuard) fully engages.
  const ldr = new Float32Array(N * 3);
  const r5 = makeRand(0x13572468);
  for (let p = 0; p < N; p++) {
    const j = (v: number) => Math.min(1, Math.max(0, v + (r5() - 0.5) * 0.06));
    ldr[p * 3] = j(0.46);
    ldr[p * 3 + 1] = j(0.44);
    ldr[p * 3 + 2] = j(0.43);
  }
  frames.push({ name: 'lowrange-degraded', pixels: ldr });

  // Bright with highlight clipping and a saturated wedge.
  const bright = new Float32Array(N * 3);
  const r4 = makeRand(0x7abc1200);
  for (let p = 0; p < N; p++) {
    bright[p * 3] = Math.min(1, 0.6 + r4() * 0.6);
    bright[p * 3 + 1] = Math.min(1, 0.5 + r4() * 0.5);
    bright[p * 3 + 2] = Math.min(1, 0.4 + r4() * 0.4);
  }
  frames.push({ name: 'bright-clip', pixels: bright });

  return frames;
}

interface Ctx {
  cc: string;
  bin: string;
  tmp: string;
}

function runCpp(ctx: Ctx, args: string[]): void {
  execFileSync(ctx.bin, args, { stdio: 'inherit' });
}

function readF32(path: string, n: number): Float32Array {
  const buf = readFileSync(path);
  if (buf.byteLength !== n * 4) {
    throw new Error(`readF32: ${path} has ${buf.byteLength} bytes, expected ${n * 4}`);
  }
  return new Float32Array(buf.buffer, buf.byteOffset, n);
}

function readF64(path: string, n: number): Float64Array {
  const buf = readFileSync(path);
  if (buf.byteLength !== n * 8) {
    throw new Error(`readF64: ${path} has ${buf.byteLength} bytes, expected ${n * 8}`);
  }
  return new Float64Array(buf.buffer, buf.byteOffset, n);
}

// Canonical FootageStats flattening - must match flattenStats() in core_parity.cpp.
function flattenStats(s: FootageStats): number[] {
  const p = s.lumaPercentiles;
  return [
    p.p1, p.p5, p.p25, p.p50, p.p75, p.p95, p.p99,
    s.lab.mean[0], s.lab.mean[1], s.lab.mean[2],
    s.lab.std[0], s.lab.std[1], s.lab.std[2],
    s.bandChroma.shadows, s.bandChroma.mids, s.bandChroma.highlights,
    s.saturation.mean, s.saturation.std,
    s.skinPresence,
    s.clipping.low, s.clipping.high,
  ];
}

interface Tracker {
  maxErr: number;
  worst: string;
  count: number;
}

function newTracker(): Tracker {
  return { maxErr: 0, worst: '', count: 0 };
}

function track(t: Tracker, ref: ArrayLike<number>, got: ArrayLike<number>, label: string): void {
  const n = Math.min(ref.length, got.length);
  for (let i = 0; i < n; i++) {
    const err = Math.abs(ref[i]! - got[i]!);
    t.count++;
    if (err > t.maxErr) {
      t.maxErr = err;
      t.worst = `${label}[${i}] ts=${ref[i]!.toFixed(8)} cpp=${got[i]!.toFixed(8)}`;
    }
  }
}

function main(): void {
  if (!existsSync(cppSource)) throw new Error(`core-parity-test: missing ${cppSource}`);
  const cc = findCompiler();
  const tmp = mkdtempSync(join(tmpdir(), 'cg-core-parity-'));
  const bin = join(tmp, process.platform === 'win32' ? 'core_parity.exe' : 'core_parity');
  execFileSync(cc, ['-O2', '-std=c++17', cppSource, '-o', bin], { stdio: 'inherit' });
  const ctx: Ctx = { cc, bin, tmp };

  const frames = makeFrames();
  // Stage each frame's pixel buffer to disk once; reuse across theme/opts bakes.
  const framePaths = new Map<string, string>();
  for (const f of frames) {
    const path = join(tmp, `frame-${f.name}.f32`);
    writeFileSync(path, Buffer.from(f.pixels.buffer, f.pixels.byteOffset, f.pixels.byteLength));
    framePaths.set(f.name, path);
  }

  const statsT = newTracker();
  const gradeT = newTracker();
  const recipeT = newTracker();
  const decodeT = newTracker();

  // Coverage: confirm the chroma-overshoot guard actually engages on at least one
  // frame x theme (else parity would only exercise its inert path). Uses the TS
  // oracle's exported guard; the C++ port is validated by the grade LUTs matching.
  let guardEngaged = 0;
  let maxSeverity = 0;
  for (const f of frames) {
    const stats = computeStats(f.pixels);
    for (const theme of Object.values(THEMES)) {
      const g = toneStretchChromaGuard(stats, theme.targetStats);
      if (g.severity > 0) guardEngaged++;
      maxSeverity = Math.max(maxSeverity, g.severity);
    }
  }
  if (guardEngaged === 0) {
    console.error('core-parity-test: FAIL - chroma-overshoot guard never engaged; add a large-stretch frame');
    process.exit(1);
  }

  // --- computeStats parity ---
  for (const f of frames) {
    const ref = flattenStats(computeStats(f.pixels));
    const outPath = join(tmp, `stats-${f.name}.f64`);
    runCpp(ctx, ['stats', framePaths.get(f.name)!, String(f.pixels.length), outPath]);
    const got = readF64(outPath, ref.length);
    track(statsT, ref, got, `stats:${f.name}`);
  }

  // --- bakeGradeLut parity: every theme x every frame x knob-space samples ---
  const optsMatrix: { label: string; opts: EngineOptions }[] = [
    { label: 'default', opts: {} },
    { label: 'strength0', opts: { strength: 0 } }, // exact-identity invariant
    { label: 'strength0.5', opts: { strength: 0.5 } },
    { label: 'strength1', opts: { strength: 1 } },
    { label: 'skinProt0', opts: { skinProtection: 0 } },
    { label: 'skinProt1', opts: { skinProtection: 1 } },
    { label: 'str0.6-skin0.3', opts: { strength: 0.6, skinProtection: 0.3 } },
  ];
  const gradeSizes = [33, 17];
  for (const [themeName, theme] of Object.entries(THEMES)) {
    for (const f of frames) {
      const stats = computeStats(f.pixels);
      for (const { label, opts } of optsMatrix) {
        for (const size of gradeSizes) {
          const ref: Lut3D = bakeGradeLut(stats, theme, opts, size);
          const outPath = join(tmp, `grade-${themeName}-${f.name}-${label}-${size}.f32`);
          const hasStr = opts.strength !== undefined ? '1' : '0';
          const strVal = String(opts.strength ?? 0);
          const hasSkin = opts.skinProtection !== undefined ? '1' : '0';
          const skinVal = String(opts.skinProtection ?? 0);
          runCpp(ctx, [
            'grade', framePaths.get(f.name)!, String(f.pixels.length),
            themeName, hasStr, strVal, hasSkin, skinVal, String(size), outPath,
          ]);
          const got = readF32(outPath, ref.data.length);
          track(gradeT, ref.data, got, `grade:${themeName}/${f.name}/${label}/${size}`);
        }
      }
    }
  }

  // --- arb-data recipe round-trip: recipeFromTheme -> bakeFromRecipe must match
  //     the plain bakeGradeLut oracle (proves the POD recipe carries the full
  //     theme/target-stats + knob space faithfully). ---
  const recipeOpts: { label: string; opts: EngineOptions }[] = [
    { label: 'default', opts: {} },
    { label: 'str0.6-skin0.3', opts: { strength: 0.6, skinProtection: 0.3 } },
  ];
  for (const [themeName, theme] of Object.entries(THEMES)) {
    for (const f of frames) {
      const stats = computeStats(f.pixels);
      for (const { label, opts } of recipeOpts) {
        const ref: Lut3D = bakeGradeLut(stats, theme, opts, 33);
        const outPath = join(tmp, `recipe-${themeName}-${f.name}-${label}.f32`);
        const hasStr = opts.strength !== undefined ? '1' : '0';
        const strVal = String(opts.strength ?? 0);
        const hasSkin = opts.skinProtection !== undefined ? '1' : '0';
        const skinVal = String(opts.skinProtection ?? 0);
        runCpp(ctx, [
          'recipe', framePaths.get(f.name)!, String(f.pixels.length),
          themeName, hasStr, strVal, hasSkin, skinVal, '33', outPath,
        ]);
        const got = readF32(outPath, ref.data.length);
        track(recipeT, ref.data, got, `recipe:${themeName}/${f.name}/${label}`);
      }
    }
  }

  // --- midtoneTint parity (PR #24): inject a midtone tint into each theme (no
  //     shipping theme sets one) and bake via the recipe path in both engines, so
  //     the engine's midtone-tint block and the recipe's carry of it are covered. ---
  const midTints: [number, number][] = [
    [8, -6],
    [-12, 10],
  ];
  for (const [themeName, theme] of Object.entries(THEMES)) {
    for (const f of frames) {
      const stats = computeStats(f.pixels);
      for (const mid of midTints) {
        const themeWithMid = {
          ...theme,
          overrides: { ...(theme.overrides ?? {}), midtoneTint: mid },
        };
        const ref: Lut3D = bakeGradeLut(stats, themeWithMid, {}, 33);
        const outPath = join(tmp, `grademid-${themeName}-${f.name}-${mid[0]}_${mid[1]}.f32`);
        runCpp(ctx, [
          'grademid', framePaths.get(f.name)!, String(f.pixels.length),
          themeName, '0', '0', '0', '0', '33', String(mid[0]), String(mid[1]), outPath,
        ]);
        const got = readF32(outPath, ref.data.length);
        track(recipeT, ref.data, got, `grademid:${themeName}/${f.name}/${mid[0]}_${mid[1]}`);
      }
    }
  }

  // --- Chroma Gain slider parity (effect BakeAutoLut): the slider is a RELATIVE
  //     multiplier on the theme's authored chromaGain, so slider factor 1.0 must
  //     reproduce the plain by-name grade bit-exact (100% = authored). This is the
  //     one path the by-name bakes never exercise. ---
  const chromaFactors = [1.0, 0.5, 1.5];
  for (const [themeName, theme] of Object.entries(THEMES)) {
    const authored = theme.overrides?.chromaGain ?? 1;
    for (const f of frames) {
      const stats = computeStats(f.pixels);
      for (const factor of chromaFactors) {
        const themeWithChroma = {
          ...theme,
          overrides: { ...(theme.overrides ?? {}), chromaGain: authored * factor },
        };
        const ref: Lut3D = bakeGradeLut(stats, themeWithChroma, {}, 33);
        const outPath = join(tmp, `chroma-${themeName}-${f.name}-${factor}.f32`);
        runCpp(ctx, [
          'chromaslider', framePaths.get(f.name)!, String(f.pixels.length),
          themeName, '0', '0', '0', '0', '33', String(factor), outPath,
        ]);
        const got = readF32(outPath, ref.data.length);
        track(gradeT, ref.data, got, `chroma:${themeName}/${f.name}/x${factor}`);
      }
    }
  }

  // --- Correct+Grade decode composition parity (effect BakeAutoLut, V-Log path):
  //     decode each grid node to Rec.709, then grade, baked into one LUT. Proves the
  //     effect's Correct decode stage matches the oracle, not just the pieces. ---
  const decodeProfileKey = 'vlog';
  const decodeOpts: { label: string; opts: EngineOptions }[] = [
    { label: 'default', opts: {} },
    { label: 'str0.6-skin0.3', opts: { strength: 0.6, skinProtection: 0.3 } },
  ];
  for (const [themeName, theme] of Object.entries(THEMES)) {
    for (const f of frames) {
      const stats = computeStats(f.pixels);
      for (const { label, opts } of decodeOpts) {
        const g = buildTransform(stats, theme, opts);
        const profile = PROFILES[decodeProfileKey]!;
        const ref: Lut3D = bakeLut((rgb) => g(decodePixelToRec709(rgb, profile)), 33);
        const outPath = join(tmp, `gradedecode-${themeName}-${f.name}-${label}.f32`);
        const hasStr = opts.strength !== undefined ? '1' : '0';
        const strVal = String(opts.strength ?? 0);
        const hasSkin = opts.skinProtection !== undefined ? '1' : '0';
        const skinVal = String(opts.skinProtection ?? 0);
        runCpp(ctx, [
          'gradedecode', framePaths.get(f.name)!, String(f.pixels.length),
          themeName, hasStr, strVal, hasSkin, skinVal, '33', decodeProfileKey, outPath,
        ]);
        const got = readF32(outPath, ref.data.length);
        track(gradeT, ref.data, got, `gradedecode:${themeName}/${f.name}/${label}`);
      }
    }
  }

  // --- Embedded/External Correct composition parity (effect ComposeDecodeIntoLut):
  //     a baked raw LUT resampled through decode - newLut(x) = rawLut(decode(x)). ---
  for (const [themeName, theme] of Object.entries(THEMES)) {
    for (const f of frames) {
      const stats = computeStats(f.pixels);
      for (const { label, opts } of decodeOpts) {
        const raw: Lut3D = bakeGradeLut(stats, theme, opts, 33);
        const profile = PROFILES[decodeProfileKey]!;
        const ref: Lut3D = bakeLut((rgb) => sampleLut(raw, decodePixelToRec709(rgb, profile)), 33);
        const outPath = join(tmp, `lutdecode-${themeName}-${f.name}-${label}.f32`);
        const hasStr = opts.strength !== undefined ? '1' : '0';
        const strVal = String(opts.strength ?? 0);
        const hasSkin = opts.skinProtection !== undefined ? '1' : '0';
        const skinVal = String(opts.skinProtection ?? 0);
        runCpp(ctx, [
          'lutdecode', framePaths.get(f.name)!, String(f.pixels.length),
          themeName, hasStr, strVal, hasSkin, skinVal, '33', decodeProfileKey, outPath,
        ]);
        const got = readF32(outPath, ref.data.length);
        track(gradeT, ref.data, got, `lutdecode:${themeName}/${f.name}/${label}`);
      }
    }
  }

  // --- Embedded/External V-Log partial-Strength composition parity (effect
  //     ComposeDecodeIntoLut + strength baked in): the decoded-space blend
  //     newLut(x) = lerp(decode(x), rawLut(decode(x)), s). Proves s<100% agrees
  //     with the oracle (no raw-log term left in the blend). ---
  for (const [themeName, theme] of Object.entries(THEMES)) {
    for (const f of frames) {
      const stats = computeStats(f.pixels);
      const raw: Lut3D = bakeGradeLut(stats, theme, {}, 33);
      const profile = PROFILES[decodeProfileKey]!;
      for (const s of [0, 0.5, 1]) {
        const ref: Lut3D = bakeLut((rgb) => {
          const dec = decodePixelToRec709(rgb, profile);
          const lut = sampleLut(raw, dec);
          return [
            dec[0] * (1 - s) + lut[0] * s,
            dec[1] * (1 - s) + lut[1] * s,
            dec[2] * (1 - s) + lut[2] * s,
          ] as [number, number, number];
        }, 33);
        const outPath = join(tmp, `lutdecodestrength-${themeName}-${f.name}-${s}.f32`);
        runCpp(ctx, [
          'lutdecodestrength', framePaths.get(f.name)!, String(f.pixels.length),
          themeName, '0', '0', '0', '0', '33', decodeProfileKey, String(s), outPath,
        ]);
        const got = readF32(outPath, ref.data.length);
        track(gradeT, ref.data, got, `lutdecodestrength:${themeName}/${f.name}/s${s}`);
      }
    }
  }

  // --- Manual primary correction + Look Mix parity (Phase 6a): the new manual
  //     stage baked in both engines, direct (grademanual) and carried through the
  //     arb-data recipe (recipemanual). Presets cover each op, a combined grade,
  //     and Look Mix < 1 on a real theme. Runs on every theme incl. none-manual. ---
  const manualCsv = (m: ManualGrade, lookMix: number): string =>
    [
      m.exposure, m.contrast, m.pivot, m.highlights, m.shadows, m.whites, m.blacks,
      m.temperature, m.tint, m.saturation, m.vibrance, lookMix,
    ].join(',');
  const mk = (over: Partial<ManualGrade>): ManualGrade => ({ ...NEUTRAL_MANUAL, ...over });
  const manualPresets: { label: string; m: ManualGrade; lookMix: number }[] = [
    { label: 'exposure', m: mk({ exposure: 1.3 }), lookMix: 1 },
    { label: 'contrast', m: mk({ contrast: 70, pivot: 0.45 }), lookMix: 1 },
    { label: 'regions', m: mk({ highlights: -40, shadows: 60, whites: 30, blacks: -50 }), lookMix: 1 },
    { label: 'tempTint', m: mk({ temperature: 55, tint: -35 }), lookMix: 1 },
    { label: 'satVib', m: mk({ saturation: 1.5, vibrance: 40 }), lookMix: 1 },
    {
      label: 'combined',
      m: mk({ exposure: -0.8, contrast: 45, shadows: 30, temperature: 40, saturation: 1.3, vibrance: 25 }),
      lookMix: 1,
    },
    { label: 'lookmix0', m: mk({ exposure: 0.5, saturation: 1.2 }), lookMix: 0 },
    { label: 'lookmix0.5', m: mk({ contrast: 30 }), lookMix: 0.5 },
  ];
  const manualOpts: { label: string; opts: EngineOptions }[] = [
    { label: 'default', opts: {} },
    { label: 'str0.6-skin0.3', opts: { strength: 0.6, skinProtection: 0.3 } },
  ];
  for (const [themeName, theme] of Object.entries(THEMES)) {
    for (const f of [frames[0]!, frames[1]!]) {
      const stats = computeStats(f.pixels);
      for (const preset of manualPresets) {
        for (const { label: optLabel, opts } of manualOpts) {
          const fullOpts: EngineOptions = { ...opts, manual: preset.m, lookMix: preset.lookMix };
          const ref: Lut3D = bakeGradeLut(stats, theme, fullOpts, 33);
          const hasStr = opts.strength !== undefined ? '1' : '0';
          const strVal = String(opts.strength ?? 0);
          const hasSkin = opts.skinProtection !== undefined ? '1' : '0';
          const skinVal = String(opts.skinProtection ?? 0);
          const csv = manualCsv(preset.m, preset.lookMix);
          // Direct manual bake.
          const outDirect = join(tmp, `grademanual-${themeName}-${f.name}-${preset.label}-${optLabel}.f32`);
          runCpp(ctx, [
            'grademanual', framePaths.get(f.name)!, String(f.pixels.length),
            themeName, hasStr, strVal, hasSkin, skinVal, '33', csv, outDirect,
          ]);
          track(gradeT, ref.data, readF32(outDirect, ref.data.length),
            `grademanual:${themeName}/${f.name}/${preset.label}/${optLabel}`);
          // Same manual carried through the arb-data recipe.
          const outRecipe = join(tmp, `recipemanual-${themeName}-${f.name}-${preset.label}-${optLabel}.f32`);
          runCpp(ctx, [
            'recipemanual', framePaths.get(f.name)!, String(f.pixels.length),
            themeName, hasStr, strVal, hasSkin, skinVal, '33', csv, outRecipe,
          ]);
          track(recipeT, ref.data, readF32(outRecipe, ref.data.length),
            `recipemanual:${themeName}/${f.name}/${preset.label}/${optLabel}`);
        }
      }
    }
  }

  // --- Versioned arb-data migration self-test (Phase 6a landmine): old grades must
  //     survive the RECIPE_VERSION bump. Self-checks in C++ (exits nonzero on failure). ---
  runCpp(ctx, ['migrate']);
  console.log('  migrate  self-test passed (arb-data v2->v3 forward migration)');

  // --- bakeDecodeLut parity: every profile, grade + fine grids ---
  for (const profileKey of Object.keys(PROFILES)) {
    for (const size of [33, 65]) {
      const ref: Lut3D = bakeDecodeLut(PROFILES[profileKey]!, size);
      const outPath = join(tmp, `decode-${profileKey}-${size}.f32`);
      runCpp(ctx, ['decode', profileKey, String(size), outPath]);
      const got = readF32(outPath, ref.data.length);
      track(decodeT, ref.data, got, `decode:${profileKey}/${size}`);
    }
  }

  const report = (name: string, t: Tracker) =>
    console.log(
      `  ${name.padEnd(8)} samples=${t.count.toString().padStart(9)} maxAbsErr=${t.maxErr.toExponential(3)}  worst: ${t.worst}`,
    );

  console.log(`core-parity-test: compiler=${cc}  tol=${TOL}`);
  console.log(`  guard    engaged in ${guardEngaged} frame*theme cases (max severity=${maxSeverity.toFixed(3)})`);
  report('stats', statsT);
  report('grade', gradeT);
  report('recipe', recipeT);
  report('decode', decodeT);

  const overall = Math.max(statsT.maxErr, gradeT.maxErr, recipeT.maxErr, decodeT.maxErr);
  console.log(`core-parity-test: overall maxAbsErr=${overall.toExponential(3)} (tol=${TOL})`);
  if (overall > TOL) {
    console.error(`core-parity-test: FAIL - max error ${overall} exceeds tolerance ${TOL}`);
    process.exit(1);
  }
  console.log('core-parity-test: PASS');
}

main();
