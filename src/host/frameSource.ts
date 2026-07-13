/**
 * The FrameSource abstraction (ADR 0001).
 *
 * Every pixel consumer - analysis, scopes, LUT baking - reads frames through
 * this one interface: comp time in, normalized Float32 pixels out. Consumers
 * never learn how the pixels were acquired, so the v1 render-to-file backend
 * can be swapped for a future AEGP backend without touching them. Tests feed
 * consumers a file-backed FrameSource (see `fileFrameSource.ts`) so they run
 * without a host at all.
 */

/** A decoded frame: interleaved RGB, normalized to [0,1], row-major. */
export interface Frame {
  width: number;
  height: number;
  /** Interleaved RGB floats in [0,1], length `width * height * 3`. */
  data: Float32Array;
  /** Bit depth of the render this frame was decoded from (16 for TIFF, 8 for PNG). */
  bitDepthOfSource: number;
}

export interface FrameSource {
  /**
   * Acquire the frame at the given comp time (seconds) as normalized pixels.
   * The acquisition method (render-to-file today, maybe native later) is
   * deliberately invisible to the caller.
   */
  getFrame(time: number): Promise<Frame>;
}

/** On-disk formats the render-to-file backend can produce and decode. */
export type FrameFileFormat = 'tiff' | 'png';

/**
 * A frame the bridge has rendered to disk. The backend reads and then deletes
 * `path`; `format` selects the decoder.
 */
export interface RenderedFrameRef {
  path: string;
  format: FrameFileFormat;
}

/** Reads raw bytes of a rendered frame file (CEP `cep.fs`, Node `fs`, ...). */
export interface FrameFileReader {
  read(path: string): Promise<Uint8Array>;
}

/** A reader that can also delete files, for cleaning up temporary renders. */
export interface FrameFileStore extends FrameFileReader {
  remove(path: string): Promise<void>;
}
