/**
 * Theme-library before/after gallery generator.
 *
 * Renders every registered theme (`THEMES`) against a few representative fixture
 * frames using the exact core seam (`decodeToRec709` -> `buildTransform` ->
 * per-pixel apply), writes before/after PNGs, and emits an index.html + report.md
 * carrying the grade-impact evidence (cast magnitude/direction, skin hue/chroma
 * shift) - a visual review surface for adding or tuning themes.
 *
 * Usage: npm run gallery -- [--out <dir>]   (default out/theme-gallery, gitignored)
 * Needs the local-only fixture frames (tests/fixtures/frames/, see CLAUDE.md).
 */
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { encode as encodePng } from 'fast-png';
import { PROFILES } from '../src/core/color/index.js';
import { computeStats, decodeToRec709 } from '../src/core/analysis/stats.js';
import { buildTransform, type Transform } from '../src/core/engine/engine.js';
import { computeGradeImpact } from '../src/core/analysis/gradeImpact.js';
import type { Theme } from '../src/core/engine/theme.js';
import { THEMES } from '../src/themes/index.js';
import { loadTiff, downsample, type FrameBuffer } from './lib/loadTiff.js';

function argVal(flag: string): string | undefined {
  const i = process.argv.indexOf(flag);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

const OUT_ROOT = argVal('--out') ?? join('out', 'theme-gallery');
const FRAMES_DIR = join('tests', 'fixtures', 'frames');
const GALLERY_MAX_W = 520; // downsample gallery images for a compact page

// Representative frames: daylight+skin, warm indoor high-skin, and night/mixed.
const SHOW_FRAMES = ['rec709-outdoor.tif', 'vlog-tricky-skin.tif', 'vlog-city-night.tif'];

// The original shipping set (issue #6); everything else is the expanded library.
// none-manual has no automatic look (it's the manual-only base), so a static
// before/after gallery would show it as identity - skip it here.
const ORIGINAL = new Set(['teal-orange', 'warm-film', 'cool-noir', 'none-manual']);
const GALLERY_THEMES = Object.keys(THEMES).filter((k) => k !== 'none-manual');
const EXISTING = GALLERY_THEMES.filter((k) => ORIGINAL.has(k));
const NEW_THEMES = GALLERY_THEMES.filter((k) => !ORIGINAL.has(k));

function profileFor(file: string) {
  return PROFILES[file.startsWith('vlog') ? 'vlog' : 'rec709']!;
}

function downTo(frame: FrameBuffer, maxW: number): FrameBuffer {
  return downsample(frame, maxW);
}

function toPng(pixels: Float32Array, w: number, h: number): Uint8Array {
  const data = new Uint8Array(w * h * 3);
  for (let s = 0; s < data.length; s++) data[s] = Math.round(Math.max(0, Math.min(1, pixels[s]!)) * 255);
  return encodePng({ width: w, height: h, data, channels: 3, depth: 8 });
}

function applyTransform(pixels: Float32Array, t: Transform): Float32Array {
  const out = new Float32Array(pixels.length);
  for (let i = 0; i < pixels.length; i += 3) {
    const o = t([pixels[i]!, pixels[i + 1]!, pixels[i + 2]!]);
    out[i] = o[0]; out[i + 1] = o[1]; out[i + 2] = o[2];
  }
  return out;
}

interface FrameData { file: string; short: string; decoded: FrameBuffer; srcStats: ReturnType<typeof computeStats>; }

function loadFrames(): FrameData[] {
  return SHOW_FRAMES.map((file) => {
    const full = downsample(loadTiff(join(FRAMES_DIR, file))); // 960 for stats parity
    const decodedPixels = decodeToRec709(full.pixels, profileFor(file));
    const decoded: FrameBuffer = { width: full.width, height: full.height, pixels: decodedPixels };
    return {
      file,
      short: file.replace(/\.tiff?$/i, ''),
      decoded,
      srcStats: computeStats(decodedPixels),
    };
  });
}

function fmt(x: number, d = 1): string { return x.toFixed(d); }

async function main(): Promise<void> {
  mkdirSync(join(OUT_ROOT, 'img'), { recursive: true });
  const frames = loadFrames();

  // Write "before" images once (shared across all themes).
  for (const f of frames) {
    const small = downTo(f.decoded, GALLERY_MAX_W);
    writeFileSync(join(OUT_ROOT, 'img', `before__${f.short}.png`), toPng(small.pixels, small.width, small.height));
  }

  const reportRows: string[] = [];
  const cards: { group: string; name: string; theme: Theme; perFrame: { short: string; cast: number; dir: number; skinHue: number; skinChroma: number; skinPresent: boolean }[] }[] = [];

  for (const [group, names] of [['Existing (shipping)', EXISTING], ['New (proposed)', NEW_THEMES]] as const) {
    for (const name of names) {
      const theme = THEMES[name]!;
      const perFrame = [];
      for (const f of frames) {
        const transform = buildTransform(f.srcStats, theme);
        const outPixels = applyTransform(f.decoded.pixels, transform);
        const outFrame: FrameBuffer = { width: f.decoded.width, height: f.decoded.height, pixels: outPixels };
        const small = downTo(outFrame, GALLERY_MAX_W);
        writeFileSync(join(OUT_ROOT, 'img', `${name}__${f.short}.png`), toPng(small.pixels, small.width, small.height));
        const impact = computeGradeImpact(f.decoded.pixels, transform);
        const skinPresent = f.srcStats.skinPresence > 0.02;
        perFrame.push({
          short: f.short,
          cast: impact.castMagnitude,
          dir: impact.castDirectionDeg,
          skinHue: impact.skinHueShiftDeg,
          skinChroma: impact.skinChromaShiftPct,
          skinPresent,
        });
        reportRows.push(
          `| ${name} | ${f.short} | ${fmt(impact.castMagnitude)}@${fmt(impact.castDirectionDeg)}° | ` +
            `${skinPresent ? fmt(impact.skinHueShiftDeg) + '° / ' + fmt(impact.skinChromaShiftPct) + '%' : 'n/a'} |`,
        );
      }
      cards.push({ group, name, theme, perFrame });
    }
  }

  writeFileSync(join(OUT_ROOT, 'index.html'), renderHtml(cards, frames.map((f) => f.short)));
  writeFileSync(join(OUT_ROOT, 'report.md'), renderReport(reportRows));
  console.log(`wrote gallery: ${OUT_ROOT} (${cards.length} themes x ${frames.length} frames)`);
}

function renderReport(rows: string[]): string {
  return [
    '# Theme library - grade-impact evidence',
    '',
    'Cast = overall colour shift magnitude@direction (LAB). Skin = hue shift / chroma shift on frames with real skin presence.',
    'Generated by `scripts/genThemeGallery.ts` from the exact `npm run author` core seam.',
    '',
    '| theme | frame | cast | skin (hue / chroma) |',
    '| --- | --- | --- | --- |',
    ...rows,
    '',
  ].join('\n');
}

function esc(s: string): string {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function renderHtml(
  cards: { group: string; name: string; theme: Theme; perFrame: { short: string; cast: number; dir: number; skinHue: number; skinChroma: number; skinPresent: boolean }[] }[],
  frameShorts: string[],
): string {
  const cardHtml = (c: (typeof cards)[number]): string => {
    const rows = c.perFrame
      .map((pf) => {
        const skin = pf.skinPresent
          ? `<span class="${Math.abs(pf.skinHue) > 15 ? 'warn' : ''}">skin ${fmt(pf.skinHue)}° / ${fmt(pf.skinChroma)}%</span>`
          : '<span class="muted">skin n/a</span>';
        const castCls = pf.cast > 15 ? 'warn' : '';
        return `
        <div class="pair">
          <div class="imgs">
            <figure><img loading="lazy" src="img/before__${pf.short}.png" alt="before"><figcaption>before</figcaption></figure>
            <figure><img loading="lazy" src="img/${c.name}__${pf.short}.png" alt="after"><figcaption>after</figcaption></figure>
          </div>
          <div class="meta"><b>${esc(pf.short)}</b> &middot; <span class="${castCls}">cast ${fmt(pf.cast)}@${fmt(pf.dir)}°</span> &middot; ${skin}</div>
        </div>`;
      })
      .join('');
    return `
      <section class="card">
        <header>
          <h3>${esc(c.name)}</h3>
          <p>${esc(c.theme.description)}</p>
          <p class="knobs">strength ${c.theme.knobs.strength.default} &middot; skinProtection ${c.theme.knobs.skinProtection.default}${c.theme.matchStats === false ? ' &middot; matchStats off' : ''}</p>
        </header>
        ${rows}
      </section>`;
  };

  const groups = [...new Set(cards.map((c) => c.group))].map((g) => {
    const inGroup = cards.filter((c) => c.group === g);
    return `<h2 class="group">${esc(g)} <span class="count">(${inGroup.length})</span></h2><div class="grid">${inGroup.map(cardHtml).join('')}</div>`;
  }).join('');

  return `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Colour-grade theme library - before/after gallery</title>
<style>
  :root { color-scheme: light dark; --bg:#0f1115; --panel:#181b22; --edge:#2a2f3a; --fg:#e6e8ec; --muted:#9aa2b1; --warn:#f0a35e; --accent:#6ea8fe; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--fg); font:14px/1.5 -apple-system,Segoe UI,Roboto,sans-serif; }
  header.top { padding:24px 28px 8px; }
  header.top h1 { margin:0 0 4px; font-size:22px; }
  header.top p { margin:0; color:var(--muted); max-width:70ch; }
  h2.group { padding:8px 28px 0; margin:24px 0 0; font-size:16px; border-top:1px solid var(--edge); }
  h2.group .count { color:var(--muted); font-weight:400; }
  .grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(340px,1fr)); gap:16px; padding:16px 28px 28px; }
  .card { background:var(--panel); border:1px solid var(--edge); border-radius:10px; overflow:hidden; min-width:0; }
  .card header { padding:12px 14px 8px; border-bottom:1px solid var(--edge); }
  .card h3 { margin:0 0 2px; font-size:15px; letter-spacing:.2px; }
  .card header p { margin:0; color:var(--muted); font-size:12.5px; }
  .card header p.knobs { margin-top:4px; color:var(--accent); font-size:11.5px; }
  .pair { padding:10px 12px; border-bottom:1px solid var(--edge); }
  .pair:last-child { border-bottom:0; }
  .imgs { display:grid; grid-template-columns:1fr 1fr; gap:6px; min-width:0; }
  figure { margin:0; min-width:0; }
  figure img { display:block; width:100%; height:auto; border-radius:5px; background:#000; }
  figcaption { text-align:center; color:var(--muted); font-size:10.5px; margin-top:2px; text-transform:uppercase; letter-spacing:.4px; }
  .meta { margin-top:7px; font-size:12px; color:var(--fg); }
  .muted { color:var(--muted); }
  .warn { color:var(--warn); font-weight:600; }
</style></head>
<body>
<header class="top">
  <h1>Colour-grade theme library &mdash; before/after gallery</h1>
  <p>Each look rendered through the exact <code>npm run author</code> core seam on three representative fixture frames.
  "cast" is the overall LAB colour-shift magnitude (amber flag &gt;15); "skin" is the hue/chroma shift where real skin is present (amber flag &gt;15°).
  Evidence table in <code>report.md</code>. Frames shown: ${frameShorts.map(esc).join(', ')}.</p>
</header>
${groups}
</body></html>`;
}

await main();
