/*
 * Scopes.h - the pure, host-agnostic core of the Phase 5 editor scopes (waveform,
 * histogram, vectorscope). NO AE SDK, NO Win32, NO ImGui, NO D3D here on purpose:
 * everything is exercised headlessly by native:scopes-test
 * (native/tests/editor/scopes_test.cpp), so the binning + image synthesis is proven
 * WITHOUT a running After Effects.
 *
 * The scopes are computed from the SAME decoded+graded PreviewFrame the editor already
 * shows (the layer checked out downstream of this effect - the decode invariant holds by
 * construction, so V-Log is never scoped as raw log). Each scope is rendered here into a
 * small RGBA8 ScopeImage; EditorWindow uploads that image to a D3D texture and draws it
 * with ImDrawList::AddImage - the same GPU path the live preview uses, so "GPU-drawn"
 * falls out for free and the pixel math stays pure + testable.
 *
 * All input is 8-bit RGBA (PreviewFrame::rgba, byte order R,G,B,A), values 0..255. Luma
 * is Rec.709 on the encoded signal (scopes conventionally read the display signal).
 */
#pragma once
#ifndef CG_EDITOR_SCOPES_H
#define CG_EDITOR_SCOPES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace cg {
namespace editor {

// A synthesised scope as an RGBA8 image (same shape as PreviewFrame), ready to upload.
struct ScopeImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // width*height*4, byte order R,G,B,A

    bool valid() const {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    }
};

// Per-channel + luma bin counts (bins entries each). Kept separate from the image render
// so the binning can be asserted directly in the headless test.
struct Histogram {
    int bins = 0;
    std::vector<uint32_t> r, g, b, luma;
};

inline int luma8(uint8_t r, uint8_t g, uint8_t b) {
    // Rec.709 luma, rounded to [0,255].
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    int yi = static_cast<int>(y + 0.5);
    return yi < 0 ? 0 : (yi > 255 ? 255 : yi);
}

// --- histogram --------------------------------------------------------------

inline Histogram computeHistogram(const uint8_t* rgba, int w, int h, int bins = 256) {
    Histogram out;
    if (bins < 1) bins = 1;
    out.bins = bins;
    out.r.assign(bins, 0);
    out.g.assign(bins, 0);
    out.b.assign(bins, 0);
    out.luma.assign(bins, 0);
    if (!rgba || w <= 0 || h <= 0) return out;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    auto binOf = [bins](int v) {
        int idx = v * bins / 256;
        return idx < 0 ? 0 : (idx >= bins ? bins - 1 : idx);
    };
    for (size_t i = 0; i < n; ++i) {
        const uint8_t r = rgba[i * 4 + 0], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
        out.r[binOf(r)]++;
        out.g[binOf(g)]++;
        out.b[binOf(b)]++;
        out.luma[binOf(luma8(r, g, b))]++;
    }
    return out;
}

namespace scope_detail {
// Perceptual-ish compression so a few dominant bins don't flatten the rest: sqrt of the
// normalised count, mapped to 0..255. peak is the max count across the series drawn.
inline uint8_t intensity(uint32_t count, uint32_t peak) {
    if (peak == 0 || count == 0) return 0;
    const double norm = static_cast<double>(count) / static_cast<double>(peak);
    int v = static_cast<int>(std::sqrt(norm) * 255.0 + 0.5);
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}
inline void addClamped(uint8_t& ch, int add) {
    int v = ch + add;
    ch = static_cast<uint8_t>(v > 255 ? 255 : v);
}
}  // namespace scope_detail

// Render the histogram as an RGBA image: each channel drawn as its own colour, filled
// from the baseline up to its (compressed) count, additively blended so overlaps read as
// white. outW columns are sampled from the `bins` histogram; outH is the vertical range.
inline ScopeImage renderHistogram(const Histogram& hist, int outW, int outH) {
    ScopeImage img;
    if (outW <= 0 || outH <= 0 || hist.bins <= 0) return img;
    img.width = outW;
    img.height = outH;
    img.rgba.assign(static_cast<size_t>(outW) * outH * 4u, 0);
    for (int i = 0; i < static_cast<int>(img.rgba.size()); i += 4) img.rgba[i + 3] = 255;  // opaque

    uint32_t peak = 0;
    for (int i = 0; i < hist.bins; ++i) {
        peak = std::max(peak, hist.r[i]);
        peak = std::max(peak, hist.g[i]);
        peak = std::max(peak, hist.b[i]);
    }
    if (peak == 0) return img;

    auto drawChannel = [&](const std::vector<uint32_t>& ch, int cr, int cg, int cb) {
        for (int x = 0; x < outW; ++x) {
            const int bin = x * hist.bins / outW;
            const uint8_t mag = scope_detail::intensity(ch[bin], peak);
            const int top = outH - 1 - (mag * (outH - 1)) / 255;
            for (int y = outH - 1; y >= top; --y) {
                uint8_t* p = &img.rgba[(static_cast<size_t>(y) * outW + x) * 4u];
                scope_detail::addClamped(p[0], cr);
                scope_detail::addClamped(p[1], cg);
                scope_detail::addClamped(p[2], cb);
            }
        }
    };
    // Muted primaries so overlaps sum toward white without any single channel clipping.
    drawChannel(hist.r, 170, 40, 40);
    drawChannel(hist.g, 40, 170, 40);
    drawChannel(hist.b, 40, 40, 170);
    return img;
}

// --- waveform ---------------------------------------------------------------
//
// Luma waveform: horizontal position tracks image column, vertical position tracks luma
// (bright at top), pixel intensity tracks how many source pixels fall there - the classic
// broadcast luma waveform. Rendered green (traditional), sqrt-compressed for visibility.
inline ScopeImage renderWaveform(const uint8_t* rgba, int w, int h, int outW, int outH) {
    ScopeImage img;
    if (!rgba || w <= 0 || h <= 0 || outW <= 0 || outH <= 0) return img;
    img.width = outW;
    img.height = outH;
    img.rgba.assign(static_cast<size_t>(outW) * outH * 4u, 0);
    for (int i = 0; i < static_cast<int>(img.rgba.size()); i += 4) img.rgba[i + 3] = 255;

    std::vector<uint32_t> accum(static_cast<size_t>(outW) * outH, 0);
    uint32_t peak = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = rgba + static_cast<size_t>(y) * w * 4u;
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = row + static_cast<size_t>(x) * 4u;
            const int col = x * outW / w;
            const int yv = luma8(p[0], p[1], p[2]);
            const int rowIdx = (255 - yv) * (outH - 1) / 255;  // 255 -> top row 0
            uint32_t& c = accum[static_cast<size_t>(rowIdx) * outW + col];
            ++c;
            peak = std::max(peak, c);
        }
    }
    for (int i = 0; i < outW * outH; ++i) {
        const uint8_t v = scope_detail::intensity(accum[i], peak);
        if (v == 0) continue;
        uint8_t* p = &img.rgba[static_cast<size_t>(i) * 4u];
        p[0] = static_cast<uint8_t>(v / 4);  // slight green bias, broadcast look
        p[1] = v;
        p[2] = static_cast<uint8_t>(v / 4);
    }
    return img;
}

// --- vectorscope ------------------------------------------------------------
//
// Plots each pixel's chroma on the Cb/Cr plane (JPEG/601 coefficients): neutral pixels
// land at the centre, saturated pixels push outward toward their hue. Square `size`x`size`
// image, green accumulation (traditional graticule colour). The scale maps full-scale
// chroma (|Cb|,|Cr| ~ 0.5*255) to roughly the image edge.
inline ScopeImage renderVectorscope(const uint8_t* rgba, int w, int h, int size) {
    ScopeImage img;
    if (!rgba || w <= 0 || h <= 0 || size <= 0) return img;
    img.width = size;
    img.height = size;
    img.rgba.assign(static_cast<size_t>(size) * size * 4u, 0);
    for (int i = 0; i < static_cast<int>(img.rgba.size()); i += 4) img.rgba[i + 3] = 255;

    const double center = size * 0.5;
    const double gain = (size * 0.5) / 140.0;  // ~full-scale chroma reaches near the edge
    std::vector<uint32_t> accum(static_cast<size_t>(size) * size, 0);
    uint32_t peak = 0;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < n; ++i) {
        const double r = rgba[i * 4 + 0], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
        const double cb = -0.168736 * r - 0.331264 * g + 0.5 * b;      // blue-yellow axis
        const double cr = 0.5 * r - 0.418688 * g - 0.081312 * b;       // red-cyan axis
        int px = static_cast<int>(center + cb * gain + 0.5);
        int py = static_cast<int>(center - cr * gain + 0.5);           // Cr up
        if (px < 0 || px >= size || py < 0 || py >= size) continue;
        uint32_t& c = accum[static_cast<size_t>(py) * size + px];
        ++c;
        peak = std::max(peak, c);
    }
    for (int i = 0; i < size * size; ++i) {
        const uint8_t v = scope_detail::intensity(accum[i], peak);
        if (v == 0) continue;
        uint8_t* p = &img.rgba[static_cast<size_t>(i) * 4u];
        p[0] = static_cast<uint8_t>(v / 3);
        p[1] = v;
        p[2] = static_cast<uint8_t>(v / 3);
    }
    return img;
}

}  // namespace editor
}  // namespace cg

#endif  // CG_EDITOR_SCOPES_H
