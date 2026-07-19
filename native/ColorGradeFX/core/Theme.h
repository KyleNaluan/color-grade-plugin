/*
 * Theme.h - C++ port of src/core/engine/theme.ts.
 *
 * A Theme is data only: target stats + optional authored overrides + exposed
 * knobs. Optionals are modelled with std::optional so the engine can branch on
 * presence exactly as the TS `?? / undefined` checks do.
 */
#pragma once
#ifndef CG_CORE_THEME_H
#define CG_CORE_THEME_H

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "Stats.h"

namespace cg {
namespace core {

// A curve control point [x, y].
using CurvePoint = std::array<double, 2>;

struct ChromaShape {
    std::optional<std::vector<CurvePoint>> byLuma;  // chroma multiplier vs output luma
    std::optional<double> vibrance;                 // nonlinear saturation, ~[-1,1]
    std::optional<double> softLimit;                // tanh ceiling on LAB chroma
};

struct ChannelCurves {
    std::optional<std::vector<CurvePoint>> r;
    std::optional<std::vector<CurvePoint>> g;
    std::optional<std::vector<CurvePoint>> b;
};

struct ThemeOverrides {
    // LAB [a,b] tints added per tonal band (weighted by the same feathered
    // shadow/mid/highlight split). Typically 1-20 LAB units; past ~20 reads as an
    // obvious cast. midtoneTint reaches the midtones that shadow/highlight tints
    // cannot (their band weights fall to zero where the midtone band peaks).
    std::optional<std::array<double, 2>> shadowTint;
    std::optional<std::array<double, 2>> highlightTint;
    std::optional<std::array<double, 2>> midtoneTint;
    std::optional<double> chromaGain;                    // global LAB chroma multiplier
    std::optional<std::vector<CurvePoint>> toneCurve;    // authored master tone curve
    std::optional<ChannelCurves> channelCurves;          // authored per-channel curves
    std::optional<ChromaShape> chromaShape;              // authored chroma shaping
};

struct ThemeKnobs {
    double strengthDefault;        // 0-1, interpolate whole transform toward identity
    double skinProtectionDefault;  // 0-1, skin-tone protection amount
};

struct Theme {
    std::string name;
    std::string description;
    FootageStats targetStats;
    std::optional<ThemeOverrides> overrides;
    ThemeKnobs knobs;
    // Whether the engine runs its automatic stat-matching look. True for every
    // shipping theme; the "None / Manual" theme sets it false so the stat-match
    // stages contribute exact identity and manual grading is the whole look
    // (Phase 6a). Ported from Theme.matchStats in theme.ts.
    bool matchStats = true;
};

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_THEME_H
