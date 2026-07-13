/**
 * The "analyze frame" action: pull the selected clip's current frame through a
 * FrameSource, decode it to Rec.709, and measure Footage stats.
 *
 * This is pure panel logic over the `FrameSource` and `Bridge` interfaces, so
 * it is tested against a file-backed fake FrameSource fed with fixture frames -
 * no host, no CEP. The acquisition method (render-to-file today) is invisible
 * here, exactly as ADR 0001 requires.
 */
import type { LogProfile } from '../core/color/types.js';
import { computeStats, decodeToRec709, type FootageStats } from '../core/analysis/stats.js';
import type { Bridge } from '../host/bridge';
import type { Frame, FrameSource } from '../host/frameSource';

export interface AnalyzeResult {
  stats: FootageStats;
  /** Dimensions actually measured (after any downsample for speed). */
  width: number;
  height: number;
  /** Bit depth of the render the frame came from (16 TIFF / 8 PNG). */
  bitDepthOfSource: number;
  /** Comp time (seconds) that was analyzed. */
  time: number;
  /** Log profile the footage was decoded from. */
  profileName: string;
}

/**
 * Analyze the active comp's current frame with the given decode profile.
 * Throws if no comp is active or the frame cannot be acquired.
 */
export async function analyzeCurrentFrame(
  bridge: Pick<Bridge, 'getCurrentTime'>,
  frameSource: FrameSource,
  profile: LogProfile,
): Promise<AnalyzeResult> {
  const time = await bridge.getCurrentTime();
  if (time === null) throw new Error('No active comp to analyze');
  const frame = downsampleFrame(await frameSource.getFrame(time));
  const decoded = decodeToRec709(frame.data, profile);
  return {
    stats: computeStats(decoded),
    width: frame.width,
    height: frame.height,
    bitDepthOfSource: frame.bitDepthOfSource,
    time,
    profileName: profile.name,
  };
}

/**
 * Nearest-neighbor downsample to at most `maxWidth` wide, to keep per-pixel
 * analysis responsive on full-resolution frames. Frames already within the
 * budget (including tiny fixtures) are returned untouched.
 */
export function downsampleFrame(frame: Frame, maxWidth = 960): Frame {
  if (frame.width <= maxWidth) return frame;
  const scale = frame.width / maxWidth;
  const w = maxWidth;
  const h = Math.max(1, Math.round(frame.height / scale));
  const data = new Float32Array(w * h * 3);
  for (let y = 0; y < h; y++) {
    const sy = Math.min(frame.height - 1, Math.floor(y * scale));
    for (let x = 0; x < w; x++) {
      const sx = Math.min(frame.width - 1, Math.floor(x * scale));
      const si = (sy * frame.width + sx) * 3;
      const di = (y * w + x) * 3;
      data[di] = frame.data[si]!;
      data[di + 1] = frame.data[si + 1]!;
      data[di + 2] = frame.data[si + 2]!;
    }
  }
  return { width: w, height: h, data, bitDepthOfSource: frame.bitDepthOfSource };
}
