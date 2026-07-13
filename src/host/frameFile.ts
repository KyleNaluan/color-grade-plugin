/**
 * Decodes a rendered frame file (16-bit TIFF primary, 8-bit PNG fallback) into
 * a normalized `Frame`. This is the one place that understands file formats;
 * it lives in the host layer because it pulls in npm decoders, which core may
 * not. Consumers see only the `FrameSource` interface, never these bytes.
 */
import { decode as decodeTiff } from 'tiff';
import { decode as decodePng } from 'fast-png';
import type { Frame, FrameFileFormat } from './frameSource';

/** Decode raw file bytes of the given format into a normalized RGB frame. */
export function decodeFrameFile(bytes: Uint8Array, format: FrameFileFormat): Frame {
  return format === 'tiff' ? decodeTiffFrame(bytes) : decodePngFrame(bytes);
}

function decodeTiffFrame(bytes: Uint8Array): Frame {
  const ifds = decodeTiff(bytes);
  const img = ifds[0];
  if (!img) throw new Error('TIFF frame contained no image');
  const { width, height, components, bitsPerSample, sampleFormat } = img;
  if (components < 3) {
    throw new Error(`expected RGB(A) TIFF, got ${components} component(s)`);
  }
  if (sampleFormat !== 1 || (bitsPerSample !== 8 && bitsPerSample !== 16)) {
    throw new Error(
      `unsupported TIFF sample format: ${bitsPerSample}-bit sampleFormat ${sampleFormat} (expected 8- or 16-bit unsigned integer)`,
    );
  }
  const bitDepth = bitsPerSample === 16 ? 16 : 8;
  return {
    width,
    height,
    data: normalizeRgb(img.data as ArrayLike<number>, width, height, components, bitDepth === 16 ? 65535 : 255),
    bitDepthOfSource: bitDepth,
  };
}

function decodePngFrame(bytes: Uint8Array): Frame {
  const png = decodePng(bytes);
  const { width, height, channels, depth } = png;
  if (channels < 3) {
    throw new Error(`expected RGB(A) PNG, got ${channels} channel(s)`);
  }
  const maxVal = depth === 16 ? 65535 : 255;
  return {
    width,
    height,
    data: normalizeRgb(png.data as ArrayLike<number>, width, height, channels, maxVal),
    bitDepthOfSource: depth === 16 ? 16 : 8,
  };
}

/**
 * Copy the first three (RGB) channels of an interleaved integer buffer into a
 * normalized Float32Array, dropping any alpha/extra channels.
 */
function normalizeRgb(
  src: ArrayLike<number>,
  width: number,
  height: number,
  components: number,
  maxVal: number,
): Float32Array {
  const out = new Float32Array(width * height * 3);
  for (let p = 0; p < width * height; p++) {
    out[p * 3] = src[p * components]! / maxVal;
    out[p * 3 + 1] = src[p * components + 1]! / maxVal;
    out[p * 3 + 2] = src[p * components + 2]! / maxVal;
  }
  return out;
}
