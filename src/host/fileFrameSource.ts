/**
 * A FrameSource backed by frame files already on disk, keyed by comp time.
 *
 * This is the seam that makes pixel consumers testable without a host: point
 * it at fixture frames and every consumer (analysis, scopes, LUT baking) runs
 * exactly as it would against the real render-to-file backend, because both
 * satisfy the same `FrameSource` interface. Unlike the render backend it owns
 * no temp files, so it never deletes anything.
 */
import type { FrameFileReader, FrameSource, RenderedFrameRef } from './frameSource';
import { decodeFrameFile } from './frameFile';

/** Resolve a comp time to the frame file that stands in for it. */
export type FrameFileResolver = (time: number) => RenderedFrameRef | Promise<RenderedFrameRef>;

/**
 * Build a FrameSource that reads and decodes a resolved frame file per time.
 * `resolve` maps a comp time to a file + format; `reader` supplies the bytes
 * (Node `fs` in tests, `cep.fs` in a browser-side debug harness).
 */
export function createFileFrameSource(
  resolve: FrameFileResolver,
  reader: FrameFileReader,
): FrameSource {
  return {
    async getFrame(time: number) {
      const ref = await resolve(time);
      const bytes = await reader.read(ref.path);
      return decodeFrameFile(bytes, ref.format);
    },
  };
}
