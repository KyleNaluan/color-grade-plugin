/*
 * Stats.h - C++ port of src/core/analysis/stats.ts.
 *
 * FootageStats schema + computeStats. All stats are measured on the decoded
 * (display-referred, gamma-encoded) Rec.709 signal. To match the TS exactly:
 *   - luma values are stored as float32 (the TS uses a Float32Array), so
 *     percentiles return the float-rounded luma; every other accumulation is in
 *     double (JS `number`).
 *   - percentile index uses round-half-up on non-negative values (std::llround),
 *     matching JS Math.round.
 */
#pragma once
#ifndef CG_CORE_STATS_H
#define CG_CORE_STATS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "Lab.h"
#include "Decode.h"
#include "LogProfile.h"
#include "Mat3.h"
#include "Rec709.h"

namespace cg {
namespace core {

struct LumaPercentiles {
    double p1, p5, p25, p50, p75, p95, p99;
};

struct FootageStats {
    LumaPercentiles lumaPercentiles;
    Vec3d labMean;  // [L, a, b]
    Vec3d labStd;
    struct {
        double shadows, mids, highlights;
    } bandChroma;
    struct {
        double mean, std;
    } saturation;
    double skinPresence;
    struct {
        double low, high;
    } clipping;
};

constexpr double SHADOW_MID_SPLIT = 0.25;
constexpr double MID_HIGHLIGHT_SPLIT = 0.7;

// Rec.709 luma from gamma-encoded RGB.
inline double luma709(double r, double g, double b) {
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

// LAB hue angle in degrees, [0, 360).
inline double labHueDeg(double a, double b) {
    const double h = std::atan2(b, a) * 180.0 / PI;
    return h < 0.0 ? h + 360.0 : h;
}

// Soft membership [0,1] in the skin-tone wedge around the vectorscope skin line.
inline double skinWedgeWeight(double l, double a, double b) {
    const double chroma = std::hypot(a, b);
    if (chroma < 6.0 || l < 15.0 || l > 92.0) return 0.0;
    const double SKIN_HUE = 42.0;
    double dh = std::abs(labHueDeg(a, b) - SKIN_HUE);
    if (dh > 180.0) dh = 360.0 - dh;
    const double hueW = dh <= 18.0 ? 1.0 : (dh >= 32.0 ? 0.0 : 1.0 - (dh - 18.0) / 14.0);
    const double chromaW = std::min(1.0, (chroma - 6.0) / 6.0);
    return hueW * chromaW;
}

// Encoded Rec.709 RGB pixel -> LAB.
inline Vec3d encodedRec709ToLab(double r, double g, double b) {
    return linearRec709ToLab({rec709Decode(r), rec709Decode(g), rec709Decode(b)});
}

namespace stats_detail {
// percentile() over a sorted float32 vector; mirrors the TS index math.
inline double percentile(const std::vector<float>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const long long len = static_cast<long long>(sorted.size());
    long long idx = std::llround((p / 100.0) * static_cast<double>(len - 1));
    if (idx < 0) idx = 0;
    if (idx > len - 1) idx = len - 1;
    return static_cast<double>(sorted[static_cast<size_t>(idx)]);
}
}  // namespace stats_detail

/*
 * Compute footage stats from decoded (gamma-encoded Rec.709) interleaved RGB
 * pixels. `pixels` is length `length` (a multiple of 3), values in [0,1].
 */
inline FootageStats computeStats(const float* pixels, size_t length, double clipEps = 0.001) {
    const size_t n = length / 3;
    if (n == 0) throw std::runtime_error("computeStats: empty pixel buffer");

    std::vector<float> lumas(n);
    double labSum[3] = {0, 0, 0};
    double labSqSum[3] = {0, 0, 0};
    double satSum = 0, satSqSum = 0, skinSum = 0;
    double clipLow = 0, clipHigh = 0;
    double bandChromaSum[3] = {0, 0, 0};
    double bandCount[3] = {0, 0, 0};

    for (size_t i = 0, px = 0; i < length; i += 3, px++) {
        const double r = pixels[i];
        const double g = pixels[i + 1];
        const double b = pixels[i + 2];
        const double y = luma709(r, g, b);
        lumas[px] = static_cast<float>(y);

        const Vec3d lab = encodedRec709ToLab(r, g, b);
        for (int c = 0; c < 3; c++) {
            labSum[c] += lab[c];
            labSqSum[c] += lab[c] * lab[c];
        }

        const double chroma = std::hypot(lab[1], lab[2]);
        const int band = y < SHADOW_MID_SPLIT ? 0 : (y < MID_HIGHLIGHT_SPLIT ? 1 : 2);
        bandChromaSum[band] += chroma;
        bandCount[band] += 1;

        const double mx = std::max(r, std::max(g, b));
        const double mn = std::min(r, std::min(g, b));
        const double sat = mx > 1e-6 ? (mx - mn) / mx : 0.0;
        satSum += sat;
        satSqSum += sat * sat;

        skinSum += skinWedgeWeight(lab[0], lab[1], lab[2]);

        if (mn <= clipEps) clipLow += 1;
        if (mx >= 1.0 - clipEps) clipHigh += 1;
    }

    std::sort(lumas.begin(), lumas.end());
    const double nd = static_cast<double>(n);
    auto mean = [nd](double s) { return s / nd; };
    auto std_ = [nd](double sq, double m) { return std::sqrt(std::max(0.0, sq / nd - m * m)); };
    const Vec3d labMean = {mean(labSum[0]), mean(labSum[1]), mean(labSum[2])};

    using stats_detail::percentile;
    FootageStats out;
    out.lumaPercentiles = {
        percentile(lumas, 1),  percentile(lumas, 5),  percentile(lumas, 25), percentile(lumas, 50),
        percentile(lumas, 75), percentile(lumas, 95), percentile(lumas, 99),
    };
    out.labMean = labMean;
    out.labStd = {std_(labSqSum[0], labMean[0]), std_(labSqSum[1], labMean[1]),
                  std_(labSqSum[2], labMean[2])};
    out.bandChroma = {
        bandCount[0] > 0 ? bandChromaSum[0] / bandCount[0] : 0.0,
        bandCount[1] > 0 ? bandChromaSum[1] / bandCount[1] : 0.0,
        bandCount[2] > 0 ? bandChromaSum[2] / bandCount[2] : 0.0,
    };
    out.saturation = {satSum / nd, std_(satSqSum, satSum / nd)};
    out.skinPresence = skinSum / nd;
    out.clipping = {clipLow / nd, clipHigh / nd};
    return out;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_STATS_H
