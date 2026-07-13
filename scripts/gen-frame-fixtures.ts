/**
 * Generates the committed synthetic frame fixtures used by the FrameSource
 * tests (tests/fixtures/frame-source/).
 *
 * Unlike the local-only personal footage in tests/fixtures/frames/, these are
 * small, deterministic, and safe to commit, so the file-backed FrameSource
 * tests run on a fresh clone. Both files encode the *same* picture - a 16-bit
 * TIFF (primary render path) and an 8-bit PNG (fallback path) - chosen to give
 * non-degenerate stats: a luma gradient, a skin-toned patch, and clipped
 * black/white corners.
 *
 * Run: `npx tsx scripts/gen-frame-fixtures.ts`. Re-run and commit the output
 * only when the synthetic picture intentionally changes.
 */
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { encode as encodePng } from 'fast-png';

const WIDTH = 64;
const HEIGHT = 48;

/** Build the reference picture as interleaved RGB floats in [0,1]. */
function buildPicture(): Float32Array {
  const px = new Float32Array(WIDTH * HEIGHT * 3);
  for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) {
      const i = (y * WIDTH + x) * 3;
      // Horizontal luma gradient as the base.
      const t = x / (WIDTH - 1);
      let r = t;
      let g = t;
      let b = t;
      // A skin-toned patch (warm, mid-tone) in the upper-left quadrant.
      if (x < WIDTH / 3 && y < HEIGHT / 3) {
        r = 0.78;
        g = 0.56;
        b = 0.45;
      }
      // Clipped-white block, bottom-right; clipped-black block, bottom-left.
      if (y > (HEIGHT * 3) / 4) {
        if (x > (WIDTH * 3) / 4) r = g = b = 1;
        else if (x < WIDTH / 4) r = g = b = 0;
      }
      px[i] = r;
      px[i + 1] = g;
      px[i + 2] = b;
    }
  }
  return px;
}

/** Encode an interleaved-RGB float picture as a baseline uncompressed 16-bit TIFF. */
function encodeTiff16(picture: Float32Array): Uint8Array {
  const samples = WIDTH * HEIGHT * 3;
  const dataLen = samples * 2; // 16-bit
  const IMAGE_OFFSET = 8;
  const extraOffset = IMAGE_OFFSET + dataLen;
  const bitsPerSampleOffset = extraOffset; // 3 shorts = 6 bytes
  const sampleFormatOffset = extraOffset + 6; // 3 shorts = 6 bytes
  const ifdOffset = extraOffset + 12;

  const entries: Array<[tag: number, type: number, count: number, value: number]> = [
    [256, 3, 1, WIDTH], // ImageWidth
    [257, 3, 1, HEIGHT], // ImageLength
    [258, 3, 3, bitsPerSampleOffset], // BitsPerSample -> [16,16,16]
    [259, 3, 1, 1], // Compression = none
    [262, 3, 1, 2], // PhotometricInterpretation = RGB
    [273, 4, 1, IMAGE_OFFSET], // StripOffsets
    [277, 3, 1, 3], // SamplesPerPixel
    [278, 3, 1, HEIGHT], // RowsPerStrip
    [279, 4, 1, dataLen], // StripByteCounts
    [284, 3, 1, 1], // PlanarConfiguration = chunky
    [339, 3, 3, sampleFormatOffset], // SampleFormat -> [1,1,1] unsigned
  ];

  const ifdLen = 2 + entries.length * 12 + 4;
  const total = ifdOffset + ifdLen;
  const buf = new ArrayBuffer(total);
  const dv = new DataView(buf);
  const LE = true;

  // Header.
  dv.setUint8(0, 0x49); // 'I'
  dv.setUint8(1, 0x49); // 'I' -> little-endian
  dv.setUint16(2, 42, LE);
  dv.setUint32(4, ifdOffset, LE);

  // Image data: quantize floats to 16-bit unsigned.
  for (let s = 0; s < samples; s++) {
    const v = Math.max(0, Math.min(1, picture[s]!));
    dv.setUint16(IMAGE_OFFSET + s * 2, Math.round(v * 65535), LE);
  }

  // Extra arrays.
  for (let k = 0; k < 3; k++) dv.setUint16(bitsPerSampleOffset + k * 2, 16, LE);
  for (let k = 0; k < 3; k++) dv.setUint16(sampleFormatOffset + k * 2, 1, LE);

  // IFD (entries must be sorted by tag; they already are).
  dv.setUint16(ifdOffset, entries.length, LE);
  let p = ifdOffset + 2;
  for (const [tag, type, count, value] of entries) {
    dv.setUint16(p, tag, LE);
    dv.setUint16(p + 2, type, LE);
    dv.setUint32(p + 4, count, LE);
    // SHORT single values sit left-aligned in the 4-byte value field.
    if (type === 3 && count === 1) dv.setUint16(p + 8, value, LE);
    else dv.setUint32(p + 8, value, LE);
    p += 12;
  }
  dv.setUint32(p, 0, LE); // next IFD = none

  return new Uint8Array(buf);
}

/** Encode the picture as an 8-bit RGB PNG. */
function encodePng8(picture: Float32Array): Uint8Array {
  const data = new Uint8Array(WIDTH * HEIGHT * 3);
  for (let s = 0; s < data.length; s++) {
    data[s] = Math.round(Math.max(0, Math.min(1, picture[s]!)) * 255);
  }
  return encodePng({ width: WIDTH, height: HEIGHT, data, channels: 3, depth: 8 });
}

const outDir = join(dirname(fileURLToPath(import.meta.url)), '..', 'tests', 'fixtures', 'frame-source');
mkdirSync(outDir, { recursive: true });
const picture = buildPicture();
writeFileSync(join(outDir, 'synthetic.tif'), encodeTiff16(picture));
writeFileSync(join(outDir, 'synthetic.png'), encodePng8(picture));
console.log(`wrote fixtures to ${outDir} (synthetic.tif, synthetic.png)`);
