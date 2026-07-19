/*
 * Themes.h - C++ port of the three shipping themes (src/themes/*.ts).
 *
 * Data only, transcribed verbatim from the TS theme files. The theme data is
 * intentionally duplicated (not generated) so it is a second, independent
 * expression of the same numbers; the cross-engine golden harness bakes each
 * theme by name in BOTH engines, so any transcription drift here surfaces as a
 * LUT-parity failure rather than a silent divergence.
 *
 * When a theme file changes in src/themes, update the matching builder here and
 * re-run `npm run native:core-parity`.
 */
#pragma once
#ifndef CG_CORE_THEMES_H
#define CG_CORE_THEMES_H

#include <string>

#include "Stats.h"
#include "Theme.h"

namespace cg {
namespace core {

inline Theme tealOrangeTheme() {
    Theme t;
    t.name = "teal-orange";
    t.description = "Punchy contrast, warm skin/highlights, teal shadows.";
    t.targetStats = FootageStats{
        {0.02, 0.05, 0.22, 0.45, 0.68, 0.9, 0.97},  // lumaPercentiles
        {48, 7, 10},                                 // labMean
        {23, 14, 16},                                // labStd
        {14, 20, 13},                                // bandChroma
        {0.35, 0.18},                                // saturation
        0,                                           // skinPresence
        {0.002, 0.002},                              // clipping
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{-9, -13};
    ov.highlightTint = std::array<double, 2>{3, 7};
    ov.chromaGain = 1.0;
    ov.toneCurve = std::vector<CurvePoint>{{0, 0}, {0.25, 0.21}, {0.5, 0.5}, {0.75, 0.79}, {1, 1}};
    ChannelCurves cc;
    cc.r = std::vector<CurvePoint>{{0, 0}, {0.7, 0.72}, {1, 1}};
    ov.channelCurves = cc;
    ChromaShape cs;
    cs.byLuma = std::vector<CurvePoint>{{0, 0.95}, {0.5, 1.05}, {1, 0.9}};
    cs.vibrance = 0.12;
    cs.softLimit = 60;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.8, 0.78};
    return t;
}

inline Theme warmFilmTheme() {
    Theme t;
    t.name = "warm-film";
    t.description = "Soft contrast, lifted blacks, golden warmth, muted chroma.";
    t.targetStats = FootageStats{
        {0.04, 0.07, 0.26, 0.48, 0.7, 0.86, 0.93},
        {52, 6, 13},
        {20, 10, 12},
        {10, 18, 14},
        {0.3, 0.15},
        0,
        {0.001, 0.001},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{2, 4};
    ov.highlightTint = std::array<double, 2>{3, 9};
    ov.chromaGain = 0.95;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0.02}, {0.25, 0.27}, {0.5, 0.51}, {0.75, 0.73}, {1, 0.97}};
    ChromaShape cs;
    cs.byLuma = std::vector<CurvePoint>{{0, 0.9}, {0.5, 1}, {1, 0.8}};
    cs.softLimit = 55;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.8, 0.75};
    return t;
}

inline Theme coolNoirTheme() {
    Theme t;
    t.name = "cool-noir";
    t.description = "Deep shadows, desaturated, cool blue cast.";
    t.targetStats = FootageStats{
        {0.01, 0.03, 0.15, 0.36, 0.6, 0.85, 0.94},
        {40, 0, -7},
        {24, 8, 10},
        {8, 12, 8},
        {0.22, 0.12},
        0,
        {0.004, 0.001},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{0, -7};
    ov.chromaGain = 0.85;
    ov.toneCurve = std::vector<CurvePoint>{{0, 0}, {0.2, 0.15}, {0.5, 0.48}, {1, 0.96}};
    ChromaShape cs;
    cs.byLuma = std::vector<CurvePoint>{{0, 0.7}, {0.5, 0.95}, {1, 0.85}};
    cs.vibrance = -0.2;
    cs.softLimit = 40;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.75, 0.75};
    return t;
}

// "None / Manual" theme (Phase 6a): matchStats == false, no authored overrides.
// The stat-match stages contribute exact identity so manual grading is the whole
// look. targetStats are unused while matchStats is false (carried only in the
// recipe); they hold plausible neutral placeholders. Transcribed from
// src/themes/none-manual.ts.
inline Theme noneManualTheme() {
    Theme t;
    t.name = "none-manual";
    t.description = "No automatic look - manual grading only.";
    t.matchStats = false;
    t.targetStats = FootageStats{
        {0.02, 0.05, 0.25, 0.5, 0.75, 0.95, 0.98},  // lumaPercentiles
        {50, 0, 0},                                  // labMean
        {22, 10, 10},                                // labStd
        {10, 12, 10},                                // bandChroma
        {0.3, 0.15},                                 // saturation
        0,                                           // skinPresence
        {0.002, 0.002},                              // clipping
    };
    // No overrides.
    t.knobs = {1.0, 0.75};
    return t;
}

// Look a theme up by the same key strings src/themes/index.ts uses.
inline bool getTheme(const std::string& key, Theme& out) {
    if (key == "teal-orange") { out = tealOrangeTheme(); return true; }
    if (key == "warm-film") { out = warmFilmTheme(); return true; }
    if (key == "cool-noir") { out = coolNoirTheme(); return true; }
    if (key == "none-manual") { out = noneManualTheme(); return true; }
    return false;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_THEMES_H
