import { describe, expect, it, vi } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { decodeFrameFile } from '../../src/host/frameFile';
import { createRenderFrameSource, type FrameRenderer } from '../../src/host/renderFrameSource';
import { createFileFrameSource } from '../../src/host/fileFrameSource';
import type {
  FrameFileFormat,
  FrameFileReader,
  FrameFileStore,
  RenderedFrameRef,
} from '../../src/host/frameSource';

const FIXTURES = join(__dirname, '..', 'fixtures', 'frame-source');
const readFixture = (name: string) => new Uint8Array(readFileSync(join(FIXTURES, name)));

/** A reader/store backed by an in-memory map of path -> bytes. */
function memoryStore(files: Record<string, Uint8Array>): FrameFileStore & { removed: string[] } {
  const removed: string[] = [];
  return {
    removed,
    async read(path: string) {
      const bytes = files[path];
      if (!bytes) throw new Error(`no such file: ${path}`);
      return bytes;
    },
    async remove(path: string) {
      removed.push(path);
    },
  };
}

describe('decodeFrameFile', () => {
  it('decodes a 16-bit TIFF to normalized RGB with source bit depth 16', () => {
    const frame = decodeFrameFile(readFixture('synthetic.tif'), 'tiff');
    expect(frame.width).toBe(64);
    expect(frame.height).toBe(48);
    expect(frame.bitDepthOfSource).toBe(16);
    expect(frame.data.length).toBe(64 * 48 * 3);
    for (const v of frame.data) {
      expect(v).toBeGreaterThanOrEqual(0);
      expect(v).toBeLessThanOrEqual(1);
    }
  });

  it('decodes an 8-bit PNG to normalized RGB with source bit depth 8', () => {
    const frame = decodeFrameFile(readFixture('synthetic.png'), 'png');
    expect(frame.width).toBe(64);
    expect(frame.height).toBe(48);
    expect(frame.bitDepthOfSource).toBe(8);
    expect(frame.data.length).toBe(64 * 48 * 3);
  });

  it('decodes TIFF and PNG of the same picture to near-identical pixels', () => {
    const tiff = decodeFrameFile(readFixture('synthetic.tif'), 'tiff');
    const png = decodeFrameFile(readFixture('synthetic.png'), 'png');
    // The corner pixels are pure clip/skin, so quantization differences are tiny.
    for (const i of [0, 3000, tiff.data.length - 3]) {
      expect(png.data[i]!).toBeCloseTo(tiff.data[i]!, 1);
    }
  });
});

describe('createRenderFrameSource', () => {
  const renderer = (ref: RenderedFrameRef): FrameRenderer => ({
    renderFrame: vi.fn(async () => ref),
  });

  it('renders, reads back, and decodes the requested time', async () => {
    const store = memoryStore({ '/tmp/f.tif': readFixture('synthetic.tif') });
    const source = createRenderFrameSource(
      renderer({ path: '/tmp/f.tif', format: 'tiff' }),
      store,
    );
    const frame = await source.getFrame(1.5);
    expect(frame.width).toBe(64);
    expect(frame.bitDepthOfSource).toBe(16);
  });

  it('dispatches on the reported format (PNG fallback path)', async () => {
    const store = memoryStore({ '/tmp/f.png': readFixture('synthetic.png') });
    const source = createRenderFrameSource(
      renderer({ path: '/tmp/f.png', format: 'png' }),
      store,
    );
    const frame = await source.getFrame(0);
    expect(frame.bitDepthOfSource).toBe(8);
  });

  it('deletes the temporary render file after reading', async () => {
    const store = memoryStore({ '/tmp/f.tif': readFixture('synthetic.tif') });
    const source = createRenderFrameSource(
      renderer({ path: '/tmp/f.tif', format: 'tiff' }),
      store,
    );
    await source.getFrame(2);
    expect(store.removed).toEqual(['/tmp/f.tif']);
  });

  it('still deletes the temp file when decoding fails, and surfaces the error', async () => {
    const store = memoryStore({ '/tmp/bad.tif': new Uint8Array([1, 2, 3]) });
    const source = createRenderFrameSource(
      renderer({ path: '/tmp/bad.tif', format: 'tiff' }),
      store,
    );
    await expect(source.getFrame(0)).rejects.toThrow();
    expect(store.removed).toEqual(['/tmp/bad.tif']);
  });

  it('does not mask a decode error with a cleanup failure', async () => {
    const store: FrameFileStore = {
      async read() {
        return new Uint8Array([1, 2, 3]);
      },
      async remove() {
        throw new Error('cleanup blew up');
      },
    };
    const source = createRenderFrameSource(
      renderer({ path: '/tmp/x.tif', format: 'tiff' }),
      store,
    );
    // The decode error, not the cleanup error, must propagate.
    await expect(source.getFrame(0)).rejects.not.toThrow('cleanup blew up');
  });
});

describe('createFileFrameSource', () => {
  const nodeReader: FrameFileReader = {
    async read(path: string) {
      return new Uint8Array(readFileSync(path));
    },
  };

  it('reads and decodes a fixture per resolved time', async () => {
    const resolve = (time: number): RenderedFrameRef => ({
      path: join(FIXTURES, time === 0 ? 'synthetic.tif' : 'synthetic.png'),
      format: (time === 0 ? 'tiff' : 'png') as FrameFileFormat,
    });
    const source = createFileFrameSource(resolve, nodeReader);
    expect((await source.getFrame(0)).bitDepthOfSource).toBe(16);
    expect((await source.getFrame(1)).bitDepthOfSource).toBe(8);
  });
});
