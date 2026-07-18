/*
 * Engine.h - C++ port of src/core/engine/engine.ts.
 *
 * buildTransform: measured footage stats -> a per-pixel transform toward a
 * theme's target stats, scaled by knobs, with skin-tone protection. Domain and
 * range are gamma-encoded Rec.709 (the Correct stack, incl. any Decode LUT, has
 * already run). The TS returns a closure `(rgb)=>rgb`; here it is a
 * `GradeTransform` value object whose operator() is the ported closure body,
 * literally, so a baked grid matches the oracle within float tolerance.
 *
 * Strength 0 stays exact identity and output is bounded in [0,1] - the same
 * invariants the TS tests enforce.
 */
#pragma once
#ifndef CG_CORE_ENGINE_H
#define CG_CORE_ENGINE_H

#include <cmath>
#include <optional>
#include <vector>

#include "Lab.h"
#include "Mat3.h"
#include "MonotoneCurve.h"
#include "Rec709.h"
#include "Stats.h"
#include "Theme.h"

namespace cg {
namespace core {

struct EngineOptions {
    std::optional<double> strength;
    std::optional<double> skinProtection;
};

namespace engine_detail {
inline double clampv(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }

// Smooth 0->1 ramp between edges.
inline double smoothstep(double e0, double e1, double x) {
    const double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

// A tone-override curve: identity, or a monotone curve whose output is clamped
// to [0,1] (mirrors makeToneOverride in engine.ts).
struct ClampedCurve {
    bool active = false;
    MonotoneCurve curve;
    double operator()(double x) const {
        if (!active) return x;
        return clamp01(curve(x));
    }
    static ClampedCurve make(const std::optional<std::vector<CurvePoint>>& pts) {
        ClampedCurve out;
        if (!pts || pts->size() < 2) return out;  // identity
        std::vector<double> xs, ys;
        xs.reserve(pts->size());
        ys.reserve(pts->size());
        for (const auto& p : *pts) {
            xs.push_back(p[0]);
            ys.push_back(p[1]);
        }
        out.active = true;
        out.curve = MonotoneCurve::make(xs, ys, /*forceMonotoneY=*/true);
        return out;
    }
};

// Chroma-by-luma multiplier: 1.0 everywhere, or max(0, shapeCurve(y)).
struct ChromaByLuma {
    bool active = false;
    MonotoneCurve curve;
    double operator()(double y) const {
        if (!active) return 1.0;
        return std::max(0.0, curve(y));
    }
};
}  // namespace engine_detail

// Soft band membership weights [shadows, mids, highlights] with feathered edges.
inline Vec3d bandWeights(double y) {
    using engine_detail::smoothstep;
    const double feather = 0.08;
    const double s = 1.0 - smoothstep(SHADOW_MID_SPLIT - feather, SHADOW_MID_SPLIT + feather, y);
    const double h = smoothstep(MID_HIGHLIGHT_SPLIT - feather, MID_HIGHLIGHT_SPLIT + feather, y);
    return {s, std::max(0.0, 1.0 - s - h), h};
}

/** Result of the automatic chroma-overshoot guard (see toneStretchChromaGuard). */
struct ChromaGuard {
    double stretch;                     // output/input luma-range ratio (p5..p95)
    double severity;                    // 0 below the knee rising to 1
    double gain;                        // multiplier (<=1) folded into auto chroma amplification
    std::optional<double> autoSoftLimit;  // LAB-chroma ceiling when the theme authored none
};

namespace engine_detail {
// Guard tuning (ported verbatim from engine.ts). Knee above any ordinary
// source's stretch so the guard is exactly inert for ordinary footage.
constexpr double STRETCH_KNEE = 1.5;
constexpr double STRETCH_FULL = 3.0;
constexpr double GUARD_GAIN_FLOOR = 0.2;
constexpr double AUTO_CEIL_LOOSE = 6.0;  // multiple of target band chroma near the knee
constexpr double AUTO_CEIL_TIGHT = 3.0;  // multiple of target band chroma at full severity
}  // namespace engine_detail

/**
 * Automatic chroma-overshoot guard. A large tone-curve stretch (target luma
 * range far wider than the source's) relocates mid-luma pixels into the
 * shadow/highlight bands where the source-fit auto chroma amplification would
 * neon-blow-up; the stretch ratio is the proxy that damps it. Inert (gain 1, no
 * ceiling) for ordinary sources, keeping their output bit-identical.
 */
inline ChromaGuard toneStretchChromaGuard(const FootageStats& src, const FootageStats& tgt) {
    using namespace engine_detail;
    const double srcRange = std::max(src.lumaPercentiles.p95 - src.lumaPercentiles.p5, 1e-3);
    const double tgtRange = std::max(tgt.lumaPercentiles.p95 - tgt.lumaPercentiles.p5, 1e-3);
    const double stretch = tgtRange / srcRange;
    const double severity = smoothstep(STRETCH_KNEE, STRETCH_FULL, stretch);
    const double gain = lerp(1.0, GUARD_GAIN_FLOOR, severity);
    const double tgtBandChromaMax =
        std::max({tgt.bandChroma.shadows, tgt.bandChroma.mids, tgt.bandChroma.highlights});
    ChromaGuard g;
    g.stretch = stretch;
    g.severity = severity;
    g.gain = gain;
    g.autoSoftLimit = (severity > 0.0 && tgtBandChromaMax > 0.0)
                          ? std::optional<double>(tgtBandChromaMax *
                                                  lerp(AUTO_CEIL_LOOSE, AUTO_CEIL_TIGHT, severity))
                          : std::nullopt;
    return g;
}

struct GradeTransform {
    // Precomputed grade state (built by buildTransform).
    MonotoneCurve toneCurve;
    double strength = 0.0;
    double protection = 0.0;
    bool skinActive = false;
    double srcMeanA = 0.0, srcMeanB = 0.0;
    double shiftA = 0.0, shiftB = 0.0;
    double kA = 1.0, kB = 1.0;
    Vec3d bandScale{1.0, 1.0, 1.0};
    double chromaGain = 1.0;
    double guardGain = 1.0;  // chroma-overshoot guard gain, folded into `scale`
    engine_detail::ClampedCurve masterCurve, rCurve, gCurve, bCurve;
    engine_detail::ChromaByLuma chromaByLuma;
    double vibrance = 0.0;
    bool hasSoftLimit = false;
    double softLimit = 0.0;
    bool hasShadowTint = false;
    std::array<double, 2> shadowTint{0.0, 0.0};
    bool hasHighlightTint = false;
    std::array<double, 2> highlightTint{0.0, 0.0};
    bool hasMidtoneTint = false;
    std::array<double, 2> midtoneTint{0.0, 0.0};

    Vec3d operator()(const Vec3d& rgb) const {
        using engine_detail::lerp;
        const double VIBRANCE_FALLOFF = 25.0;
        const double rIn = clamp01(rgb[0]);
        const double gIn = clamp01(rgb[1]);
        const double bIn = clamp01(rgb[2]);

        // 1. Tone: stat-matching curve per channel, then authored curves.
        const double r1 = rCurve(masterCurve(toneCurve(rIn)));
        const double g1 = gCurve(masterCurve(toneCurve(gIn)));
        const double b1 = bCurve(masterCurve(toneCurve(bIn)));

        // 2. Color in LAB.
        const Vec3d labIn = linearRec709ToLab({rec709Decode(rIn), rec709Decode(gIn), rec709Decode(bIn)});
        const Vec3d lab = linearRec709ToLab({rec709Decode(r1), rec709Decode(g1), rec709Decode(b1)});
        double a = (lab[1] - srcMeanA) * kA + srcMeanA + shiftA;
        double bb = (lab[2] - srcMeanB) * kB + srcMeanB + shiftB;

        // 3. Per-band chroma scaling + overrides.
        const double y1 = luma709(r1, g1, b1);
        const Vec3d w = bandWeights(y1);
        const double ws = w[0], wm = w[1], wh = w[2];
        // guardGain bounds the total auto-driven amplification here, including
        // the authored chromaGain, when a large stretch would otherwise blow up.
        const double scale =
            (ws * bandScale[0] + wm * bandScale[1] + wh * bandScale[2]) * chromaGain * guardGain;
        a *= scale;
        bb *= scale;
        if (hasShadowTint) {
            a += shadowTint[0] * ws;
            bb += shadowTint[1] * ws;
        }
        if (hasHighlightTint) {
            a += highlightTint[0] * wh;
            bb += highlightTint[1] * wh;
        }
        if (hasMidtoneTint) {
            a += midtoneTint[0] * wm;
            bb += midtoneTint[1] * wm;
        }

        // 3b. Authored chroma shaping on the final chroma vector.
        double chroma = std::hypot(a, bb);
        if (chroma > 1e-6) {
            double target = chroma * chromaByLuma(y1);
            if (vibrance != 0.0) {
                target *= std::max(0.0, 1.0 + vibrance * std::exp(-chroma / VIBRANCE_FALLOFF));
            }
            if (hasSoftLimit && softLimit > 0.0) {
                target = softLimit * std::tanh(target / softLimit);
            }
            const double k = target / chroma;
            a *= k;
            bb *= k;
        }

        const Vec3d linOut = labToLinearRec709({lab[0], a, bb});
        Vec3d out = {
            clamp01(rec709Encode(std::max(0.0, linOut[0]))),
            clamp01(rec709Encode(std::max(0.0, linOut[1]))),
            clamp01(rec709Encode(std::max(0.0, linOut[2]))),
        };

        // 4/5. Strength + skin-tone protection: interpolate toward identity.
        double identityMix = 1.0 - strength;
        if (skinActive && protection > 0.0) {
            const double sw = skinWedgeWeight(labIn[0], labIn[1], labIn[2]);
            identityMix = std::max(identityMix, 1.0 - strength * (1.0 - protection * sw));
        }
        if (identityMix > 0.0) {
            out = {lerp(out[0], rIn, identityMix), lerp(out[1], gIn, identityMix),
                   lerp(out[2], bIn, identityMix)};
        }
        return out;
    }
};

inline GradeTransform buildTransform(const FootageStats& src, const Theme& theme,
                                     const EngineOptions& opts = {}) {
    using engine_detail::clampv;
    const FootageStats& tgt = theme.targetStats;

    GradeTransform t;
    t.strength = clamp01(opts.strength.value_or(theme.knobs.strengthDefault));
    t.protection = clamp01(opts.skinProtection.value_or(theme.knobs.skinProtectionDefault));
    t.skinActive = src.skinPresence > 0.02;  // SKIN_PRESENCE_THRESHOLD
    t.srcMeanA = src.labMean[1];
    t.srcMeanB = src.labMean[2];

    // Stat-matching tone curve from luma percentiles.
    {
        const auto& sp = src.lumaPercentiles;
        const auto& tp = tgt.lumaPercentiles;
        std::vector<double> xs = {0, sp.p5, sp.p25, sp.p50, sp.p75, sp.p95, 1};
        std::vector<double> ys = {0, tp.p5, tp.p25, tp.p50, tp.p75, tp.p95, 1};
        t.toneCurve = MonotoneCurve::make(xs, ys, /*forceMonotoneY=*/true);
    }

    // Damped LAB mean transfer (tanh soft-clamp on the mean-shift magnitude).
    const double MAX_MEAN_SHIFT = 10.0;
    const double dA = tgt.labMean[1] - src.labMean[1];
    const double dB = tgt.labMean[2] - src.labMean[2];
    const double dist = std::hypot(dA, dB);
    const double damp =
        dist > 1e-6 ? (MAX_MEAN_SHIFT * std::tanh(dist / MAX_MEAN_SHIFT)) / dist : 1.0;
    t.shiftA = dA * damp;
    t.shiftB = dB * damp;
    t.kA = clampv(tgt.labStd[1] / std::max({tgt.labStd[1] * 0.1, src.labStd[1], 1e-3}), 0.6, 1.8);
    t.kB = clampv(tgt.labStd[2] / std::max({tgt.labStd[2] * 0.1, src.labStd[2], 1e-3}), 0.6, 1.8);

    t.bandScale = {
        clampv(tgt.bandChroma.shadows / std::max(src.bandChroma.shadows, 1.0), 0.5, 2.0),
        clampv(tgt.bandChroma.mids / std::max(src.bandChroma.mids, 1.0), 0.5, 2.0),
        clampv(tgt.bandChroma.highlights / std::max(src.bandChroma.highlights, 1.0), 0.5, 2.0),
    };

    // Automatic chroma-overshoot guard: damp the stat-derived auto chroma
    // amplification for a large low->wide tone stretch, and adopt an auto soft
    // ceiling if the theme authored none. Inert for ordinary sources.
    const ChromaGuard guard = toneStretchChromaGuard(src, tgt);
    t.guardGain = guard.gain;

    // Overrides (all optional; absent -> identity behaviour).
    ThemeOverrides ov = theme.overrides.value_or(ThemeOverrides{});
    t.chromaGain = ov.chromaGain.value_or(1.0);

    t.masterCurve = engine_detail::ClampedCurve::make(ov.toneCurve);
    if (ov.channelCurves) {
        t.rCurve = engine_detail::ClampedCurve::make(ov.channelCurves->r);
        t.gCurve = engine_detail::ClampedCurve::make(ov.channelCurves->g);
        t.bCurve = engine_detail::ClampedCurve::make(ov.channelCurves->b);
    }

    ChromaShape shape = ov.chromaShape.value_or(ChromaShape{});
    if (shape.byLuma && shape.byLuma->size() >= 2) {
        std::vector<double> xs, ys;
        for (const auto& p : *shape.byLuma) {
            xs.push_back(p[0]);
            ys.push_back(p[1]);
        }
        t.chromaByLuma.active = true;
        t.chromaByLuma.curve = MonotoneCurve::make(xs, ys, /*forceMonotoneY=*/false);
    }
    t.vibrance = shape.vibrance.value_or(0.0);
    // Authored ceiling wins; otherwise the guard's auto ceiling (nullopt = none).
    const std::optional<double> softLimit = shape.softLimit ? shape.softLimit : guard.autoSoftLimit;
    if (softLimit) {
        t.hasSoftLimit = true;
        t.softLimit = *softLimit;
    }

    if (ov.shadowTint) {
        t.hasShadowTint = true;
        t.shadowTint = *ov.shadowTint;
    }
    if (ov.highlightTint) {
        t.hasHighlightTint = true;
        t.highlightTint = *ov.highlightTint;
    }
    if (ov.midtoneTint) {
        t.hasMidtoneTint = true;
        t.midtoneTint = *ov.midtoneTint;
    }

    return t;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_ENGINE_H
