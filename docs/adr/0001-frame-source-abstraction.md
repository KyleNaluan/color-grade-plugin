# 0001. Pixel access via render-to-file behind a FrameSource abstraction

Date: 2026-07-12
Status: Accepted

## Context

The panel needs rendered frame pixels for auto-grade analysis, custom scopes, skin-tone detection, and clipping warnings.
CEP/ExtendScript has no direct frame-buffer API.
The realistic options were:

1. ExtendScript render-to-file (render queue / `saveFrameToPng`), panel decodes the file.
2. A native C++ AEGP plugin checking out frames from AE's render pipeline and streaming them to the panel.

Option 1 is pure JS/ExtendScript, version-agnostic across AE releases, and takes days to build, but each frame costs ~0.5-2s, so scopes are on-demand snapshots rather than live.
Option 2 gives live, float-precision pixel access but requires a C++ toolchain, per-platform binaries, per-AE-version testing, and roughly 10x the engineering surface.

8-bit PNG output is too coarse for V-Log analysis (log footage packs wide dynamic range into a narrow code range), so within option 1 we prefer 16-bit TIFF via the render queue.

## Decision

- All pixel consumers (analysis, scopes, LUT baking) read from a single `FrameSource` interface: `getFrame(time) -> { width, height, data: Float32Array normalized 0-1, bitDepthOfSource }`.
- The v1 backend is render-to-file: 16-bit TIFF through the render queue as the primary path, 8-bit PNG as a fallback/debug path.
- Scopes are snapshot scopes: they refresh on demand or debounced on scrub-stop, not live.

## Consequences

- The project stays a pure CEP build for v1: no native toolchain, no per-version binaries.
- Analysis runs on 16-bit data, avoiding 8-bit quantization problems with V-Log.
- The editing workflow pays a ~1-2s pause whenever fresh pixels are requested.
- **An AEGP backend swap is an explicit, seriously considered future direction**, wanted for live scopes and a faster tweak-check loop.
  Downstream code must never bypass `FrameSource` or assume pixels came from a file, so the swap replaces only the backend, not the analysis/scopes code.
