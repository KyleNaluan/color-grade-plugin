/**
 * The v1 FrameSource backend (ADR 0001): render-to-file.
 *
 * It asks the bridge to render the requested comp time to a temporary file
 * (16-bit TIFF primary, 8-bit PNG fallback - the ExtendScript side owns that
 * choice and reports which format it produced), reads the bytes back, decodes
 * them to normalized pixels, and always deletes the temp file afterwards.
 *
 * Everything here is host glue with no color math and no CEP specifics, so it
 * runs under test with a scripted renderer and an in-memory file store.
 */
import type { Frame, FrameFileStore, FrameSource, RenderedFrameRef } from './frameSource';
import { decodeFrameFile } from './frameFile';

/** The one bridge capability this backend needs: render a comp time to disk. */
export interface FrameRenderer {
  renderFrame(time: number): Promise<RenderedFrameRef>;
}

/**
 * Build a render-to-file FrameSource from a renderer (the bridge) and a file
 * store (reads + deletes temp renders).
 */
export function createRenderFrameSource(
  renderer: FrameRenderer,
  store: FrameFileStore,
): FrameSource {
  return {
    async getFrame(time: number): Promise<Frame> {
      const rendered = await renderer.renderFrame(time);
      try {
        const bytes = await store.read(rendered.path);
        return decodeFrameFile(bytes, rendered.format);
      } finally {
        // The render is transient scratch: clean it up even if decode threw.
        // Cleanup failure must not mask a real decode error, so swallow it.
        await store.remove(rendered.path).catch(() => {});
      }
    },
  };
}
