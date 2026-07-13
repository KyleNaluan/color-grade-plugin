import { readFileSync } from 'node:fs';
import { decode } from 'tiff';

export interface FrameBuffer {
  width: number;
  height: number;
  /** Interleaved RGB floats in [0,1]. */
  pixels: Float32Array;
}

/** Load a TIFF (8/16-bit, RGB or RGBA) as normalized interleaved RGB floats. */
export function loadTiff(path: string): FrameBuffer {
  const ifds = decode(readFileSync(path));
  const img = ifds[0];
  if (!img) throw new Error(`no image in ${path}`);
  const { width, height, components, bitsPerSample } = img;
  if (components < 3) throw new Error(`expected RGB(A) TIFF, got ${components} components`);
  const maxVal = bitsPerSample === 16 ? 65535 : 255;
  const src = img.data as Uint16Array | Uint8Array;
  const pixels = new Float32Array(width * height * 3);
  for (let p = 0; p < width * height; p++) {
    for (let c = 0; c < 3; c++) {
      pixels[p * 3 + c] = src[p * components + c]! / maxVal;
    }
  }
  return { width, height, pixels };
}

/** Nearest-neighbor downsample to at most maxWidth wide (analysis speed). */
export function downsample(frame: FrameBuffer, maxWidth = 960): FrameBuffer {
  if (frame.width <= maxWidth) return frame;
  const scale = frame.width / maxWidth;
  const w = maxWidth;
  const h = Math.max(1, Math.round(frame.height / scale));
  const pixels = new Float32Array(w * h * 3);
  for (let y = 0; y < h; y++) {
    const sy = Math.min(frame.height - 1, Math.floor(y * scale));
    for (let x = 0; x < w; x++) {
      const sx = Math.min(frame.width - 1, Math.floor(x * scale));
      const si = (sy * frame.width + sx) * 3;
      const di = (y * w + x) * 3;
      pixels[di] = frame.pixels[si]!;
      pixels[di + 1] = frame.pixels[si + 1]!;
      pixels[di + 2] = frame.pixels[si + 2]!;
    }
  }
  return { width: w, height: h, pixels };
}
