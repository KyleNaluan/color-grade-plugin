import type { Vec3 } from '../color/types.js';

export interface Lut3D {
  size: number;
  /** Interleaved RGB, red fastest (standard .cube ordering), length size^3 * 3. */
  data: Float32Array;
  title?: string;
}

/** Bake a transform (encoded Rec.709 -> encoded Rec.709) into a 3D LUT grid. */
export function bakeLut(transform: (rgb: Vec3) => Vec3, size = 33, title?: string): Lut3D {
  const data = new Float32Array(size * size * size * 3);
  let i = 0;
  for (let b = 0; b < size; b++) {
    for (let g = 0; g < size; g++) {
      for (let r = 0; r < size; r++) {
        const out = transform([r / (size - 1), g / (size - 1), b / (size - 1)]);
        data[i++] = out[0];
        data[i++] = out[1];
        data[i++] = out[2];
      }
    }
  }
  return { size, data, title };
}

/** Serialize to Adobe/Resolve .cube text. */
export function writeCube(lut: Lut3D): string {
  const lines: string[] = [];
  if (lut.title) lines.push(`TITLE "${lut.title}"`);
  lines.push(`LUT_3D_SIZE ${lut.size}`);
  lines.push('DOMAIN_MIN 0.0 0.0 0.0');
  lines.push('DOMAIN_MAX 1.0 1.0 1.0');
  for (let i = 0; i < lut.data.length; i += 3) {
    lines.push(
      `${lut.data[i]!.toFixed(6)} ${lut.data[i + 1]!.toFixed(6)} ${lut.data[i + 2]!.toFixed(6)}`,
    );
  }
  return lines.join('\n') + '\n';
}

/** Parse .cube text back into a Lut3D. */
export function parseCube(text: string): Lut3D {
  let size = 0;
  let title: string | undefined;
  const values: number[] = [];
  for (const raw of text.split('\n')) {
    const line = raw.trim();
    if (line === '' || line.startsWith('#')) continue;
    if (line.startsWith('TITLE')) {
      title = line.replace(/^TITLE\s*/, '').replace(/^"|"$/g, '');
      continue;
    }
    if (line.startsWith('LUT_3D_SIZE')) {
      size = parseInt(line.split(/\s+/)[1]!, 10);
      continue;
    }
    if (line.startsWith('DOMAIN_') || line.startsWith('LUT_1D')) continue;
    const parts = line.split(/\s+/);
    if (parts.length === 3) {
      values.push(parseFloat(parts[0]!), parseFloat(parts[1]!), parseFloat(parts[2]!));
    }
  }
  if (size === 0) throw new Error('parseCube: missing LUT_3D_SIZE');
  if (values.length !== size * size * size * 3) {
    throw new Error(`parseCube: expected ${size ** 3} entries, got ${values.length / 3}`);
  }
  return { size, data: Float32Array.from(values), title };
}

/** Trilinear sample of a 3D LUT at an encoded RGB point. */
export function sampleLut(lut: Lut3D, rgb: Vec3): Vec3 {
  const { size, data } = lut;
  const n = size - 1;
  const clamp01 = (x: number) => (x < 0 ? 0 : x > 1 ? 1 : x);
  const coord = (x: number) => {
    const v = clamp01(x) * n;
    const i0 = Math.min(Math.floor(v), n - 1);
    return [i0, v - i0] as const;
  };
  const [ri, rf] = coord(rgb[0]);
  const [gi, gf] = coord(rgb[1]);
  const [bi, bf] = coord(rgb[2]);
  const at = (r: number, g: number, b: number, c: number) =>
    data[((b * size + g) * size + r) * 3 + c]!;
  const out: number[] = [];
  for (let c = 0; c < 3; c++) {
    const c00 = at(ri, gi, bi, c) * (1 - rf) + at(ri + 1, gi, bi, c) * rf;
    const c10 = at(ri, gi + 1, bi, c) * (1 - rf) + at(ri + 1, gi + 1, bi, c) * rf;
    const c01 = at(ri, gi, bi + 1, c) * (1 - rf) + at(ri + 1, gi, bi + 1, c) * rf;
    const c11 = at(ri, gi + 1, bi + 1, c) * (1 - rf) + at(ri + 1, gi + 1, bi + 1, c) * rf;
    const c0 = c00 * (1 - gf) + c10 * gf;
    const c1 = c01 * (1 - gf) + c11 * gf;
    out.push(c0 * (1 - bf) + c1 * bf);
  }
  return out as Vec3;
}
