/*
 * Themes.h - C++ port of all 24 registry looks (src/themes/*.ts).
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

// ---------------------------------------------------------------------------
// Expanded curated library (fm/cg-theme-library, PR #36). Each builder is
// transcribed verbatim from the matching src/themes/*.ts file; the cross-engine
// parity harness (`npm run native:core-parity`) bakes every one by name in both
// engines, so any transcription drift here surfaces as a LUT-parity failure.
// ---------------------------------------------------------------------------

// Transcribed from src/themes/golden-hour.ts.
inline Theme goldenHourTheme() {
    Theme t;
    t.name = "golden-hour";
    t.description = "Warm amber glow, lifted warm shadows, soft honeyed highlights.";
    t.targetStats = FootageStats{
        {0.03, 0.06, 0.25, 0.47, 0.69, 0.88, 0.95},
        {54, 4, 11},
        {20, 11, 14},
        {12, 18, 16},
        {0.32, 0.16},
        0,
        {0.002, 0.002},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{2, 3};
    ov.midtoneTint = std::array<double, 2>{2, 4};
    ov.highlightTint = std::array<double, 2>{3, 7};
    ov.chromaGain = 1.0;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0.02}, {0.25, 0.27}, {0.5, 0.51}, {0.75, 0.73}, {1, 0.97}};
    ChromaShape cs;
    cs.byLuma = std::vector<CurvePoint>{{0, 0.9}, {0.5, 1.05}, {1, 0.95}};
    cs.vibrance = 0.08;
    cs.softLimit = 60;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.8, 0.8};
    return t;
}

// Transcribed from src/themes/bleach-bypass.ts.
inline Theme bleachBypassTheme() {
    Theme t;
    t.name = "bleach-bypass";
    t.description = "High contrast, crushed blacks, harsh highlights, silver desaturation.";
    t.targetStats = FootageStats{
        {0.005, 0.02, 0.16, 0.42, 0.72, 0.94, 0.99},
        {46, 2, 4},
        {28, 7, 8},
        {6, 9, 6},
        {0.16, 0.1},
        0,
        {0.006, 0.006},
    };
    ThemeOverrides ov;
    ov.chromaGain = 0.55;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0}, {0.2, 0.13}, {0.5, 0.5}, {0.8, 0.9}, {1, 1}};
    ChromaShape cs;
    cs.vibrance = -0.2;
    cs.softLimit = 30;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.8, 0.72};
    return t;
}

// Transcribed from src/themes/vintage-fade.ts.
inline Theme vintageFadeTheme() {
    Theme t;
    t.name = "vintage-fade";
    t.description = "Milky lifted blacks, low contrast, muted chroma, faded print.";
    t.targetStats = FootageStats{
        {0.08, 0.12, 0.3, 0.5, 0.68, 0.82, 0.9},
        {54, 2, 8},
        {16, 8, 10},
        {8, 12, 10},
        {0.24, 0.13},
        0,
        {0.001, 0.001},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{-2, 4};
    ov.highlightTint = std::array<double, 2>{2, 6};
    ov.chromaGain = 0.8;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0.08}, {0.25, 0.31}, {0.5, 0.5}, {0.75, 0.68}, {1, 0.92}};
    ChromaShape cs;
    cs.vibrance = -0.1;
    cs.softLimit = 45;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.75, 0.78};
    return t;
}

// Transcribed from src/themes/high-key-clean.ts.
inline Theme highKeyCleanTheme() {
    Theme t;
    t.name = "high-key-clean";
    t.description = "Bright, airy, low-contrast, clean near-neutral colour.";
    t.targetStats = FootageStats{
        {0.06, 0.12, 0.35, 0.58, 0.78, 0.94, 0.99},
        {62, 1, 5},
        {18, 8, 9},
        {8, 12, 10},
        {0.26, 0.13},
        0,
        {0.001, 0.003},
    };
    ThemeOverrides ov;
    ov.chromaGain = 0.9;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0.04}, {0.25, 0.34}, {0.5, 0.58}, {0.75, 0.8}, {1, 0.99}};
    ChromaShape cs;
    cs.softLimit = 45;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.72, 0.8};
    return t;
}

// Transcribed from src/themes/low-key-moody.ts.
inline Theme lowKeyMoodyTheme() {
    Theme t;
    t.name = "low-key-moody";
    t.description = "Dark, crushed shadows, controlled highlights, warm-neutral mood.";
    t.targetStats = FootageStats{
        {0.005, 0.02, 0.13, 0.33, 0.58, 0.82, 0.92},
        {38, 2, 5},
        {22, 9, 11},
        {8, 13, 10},
        {0.26, 0.14},
        0,
        {0.005, 0.002},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{1, 1};
    ov.chromaGain = 0.9;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0}, {0.2, 0.13}, {0.5, 0.42}, {0.75, 0.66}, {1, 0.9}};
    ChromaShape cs;
    cs.byLuma = std::vector<CurvePoint>{{0, 0.75}, {0.5, 1}, {1, 0.9}};
    cs.vibrance = -0.1;
    cs.softLimit = 45;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.78, 0.76};
    return t;
}

// Transcribed from src/themes/winter-blue.ts.
inline Theme winterBlueTheme() {
    Theme t;
    t.name = "winter-blue";
    t.description = "Cool, clean, crisp steel-blue with a bright base.";
    t.targetStats = FootageStats{
        {0.02, 0.05, 0.22, 0.46, 0.7, 0.9, 0.97},
        {50, -3, -8},
        {22, 9, 11},
        {8, 12, 10},
        {0.24, 0.13},
        0,
        {0.003, 0.002},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{-2, -8};
    ov.highlightTint = std::array<double, 2>{-2, -6};
    ov.chromaGain = 0.85;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0.01}, {0.25, 0.23}, {0.5, 0.49}, {0.75, 0.74}, {1, 0.98}};
    ChromaShape cs;
    cs.softLimit = 45;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.78, 0.8};
    return t;
}

// Transcribed from src/themes/warm-portrait.ts.
inline Theme warmPortraitTheme() {
    Theme t;
    t.name = "warm-portrait";
    t.description = "Flattering skin, soft warm mids, gentle contrast.";
    t.targetStats = FootageStats{
        {0.03, 0.06, 0.25, 0.48, 0.7, 0.88, 0.95},
        {54, 6, 12},
        {19, 10, 12},
        {10, 16, 13},
        {0.3, 0.15},
        0,
        {0.002, 0.002},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{2, 2};
    ov.midtoneTint = std::array<double, 2>{2, 4};
    ov.highlightTint = std::array<double, 2>{4, 8};
    ov.chromaGain = 0.98;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0.01}, {0.25, 0.26}, {0.5, 0.5}, {0.75, 0.72}, {1, 0.97}};
    ChromaShape cs;
    cs.byLuma = std::vector<CurvePoint>{{0, 0.95}, {0.5, 1.03}, {1, 0.92}};
    cs.softLimit = 55;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.75, 0.82};
    return t;
}

// Transcribed from src/themes/pastel-dream.ts.
inline Theme pastelDreamTheme() {
    Theme t;
    t.name = "pastel-dream";
    t.description = "Soft cool-mint pastel, lifted blacks, low saturation, airy.";
    t.targetStats = FootageStats{
        {0.08, 0.13, 0.33, 0.54, 0.74, 0.9, 0.96},
        {58, -2, 2},
        {16, 7, 8},
        {7, 10, 8},
        {0.2, 0.11},
        0,
        {0.001, 0.002},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{-2, 2};
    ov.highlightTint = std::array<double, 2>{-1, 3};
    ov.chromaGain = 0.72;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0.07}, {0.25, 0.32}, {0.5, 0.54}, {0.75, 0.76}, {1, 0.96}};
    ChromaShape cs;
    cs.vibrance = 0.05;
    cs.softLimit = 35;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.7, 0.78};
    return t;
}

// Transcribed from src/themes/neon-cyberpunk.ts.
inline Theme neonCyberpunkTheme() {
    Theme t;
    t.name = "neon-cyberpunk";
    t.description = "Teal/cyan shadows, magenta highlights, punchy neon saturation.";
    t.targetStats = FootageStats{
        {0.005, 0.02, 0.14, 0.36, 0.64, 0.9, 0.98},
        {42, 4, -4},
        {24, 14, 15},
        {14, 20, 16},
        {0.4, 0.2},
        0,
        {0.006, 0.004},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{-5, -9};
    ov.highlightTint = std::array<double, 2>{5, -3};
    ov.chromaGain = 1.1;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0}, {0.2, 0.14}, {0.5, 0.5}, {0.8, 0.88}, {1, 1}};
    ChromaShape cs;
    cs.byLuma = std::vector<CurvePoint>{{0, 0.95}, {0.5, 1.08}, {1, 1.0}};
    cs.vibrance = 0.15;
    cs.softLimit = 60;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.8, 0.85};
    return t;
}

// Transcribed from src/themes/day-for-night.ts.
inline Theme dayForNightTheme() {
    Theme t;
    t.name = "day-for-night";
    t.description = "Deep underexposed moonlight, cool blue cast, crushed shadows.";
    t.targetStats = FootageStats{
        {0.003, 0.015, 0.1, 0.26, 0.48, 0.72, 0.85},
        {30, -2, -9},
        {18, 8, 11},
        {8, 11, 9},
        {0.22, 0.12},
        0,
        {0.006, 0.001},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{-2, -9};
    ov.midtoneTint = std::array<double, 2>{-2, -6};
    ov.highlightTint = std::array<double, 2>{-1, -4};
    ov.chromaGain = 0.8;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0}, {0.25, 0.16}, {0.5, 0.32}, {0.75, 0.52}, {1, 0.78}};
    ChromaShape cs;
    cs.vibrance = -0.1;
    cs.softLimit = 40;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.8, 0.83};
    return t;
}

// Transcribed from src/themes/autumn.ts.
inline Theme autumnTheme() {
    Theme t;
    t.name = "autumn";
    t.description = "Warm earthy golds and reds, rich saturated mids.";
    t.targetStats = FootageStats{
        {0.02, 0.04, 0.2, 0.44, 0.68, 0.88, 0.96},
        {50, 7, 14},
        {21, 12, 15},
        {12, 20, 16},
        {0.36, 0.18},
        0,
        {0.003, 0.002},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{3, 4};
    ov.midtoneTint = std::array<double, 2>{3, 5};
    ov.highlightTint = std::array<double, 2>{4, 8};
    ov.chromaGain = 1.0;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0}, {0.25, 0.22}, {0.5, 0.49}, {0.75, 0.74}, {1, 0.97}};
    ChromaShape cs;
    cs.byLuma = std::vector<CurvePoint>{{0, 0.95}, {0.5, 1.05}, {1, 0.92}};
    cs.vibrance = 0.1;
    cs.softLimit = 60;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.8, 0.8};
    return t;
}

// Transcribed from src/themes/summer-blockbuster.ts.
inline Theme summerBlockbusterTheme() {
    Theme t;
    t.name = "summer-blockbuster";
    t.description = "Bright, punchy teal-orange - bold tentpole daytime look.";
    t.targetStats = FootageStats{
        {0.02, 0.05, 0.23, 0.48, 0.72, 0.92, 0.98},
        {52, 8, 12},
        {24, 15, 17},
        {15, 21, 14},
        {0.38, 0.19},
        0,
        {0.002, 0.003},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{-8, -12};
    ov.highlightTint = std::array<double, 2>{5, 9};
    ov.chromaGain = 1.08;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0}, {0.25, 0.22}, {0.5, 0.51}, {0.75, 0.8}, {1, 1}};
    ChannelCurves cc;
    cc.r = std::vector<CurvePoint>{{0, 0}, {0.7, 0.73}, {1, 1}};
    ov.channelCurves = cc;
    ChromaShape cs;
    cs.byLuma = std::vector<CurvePoint>{{0, 0.95}, {0.5, 1.05}, {1, 0.9}};
    cs.vibrance = 0.14;
    cs.softLimit = 60;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.82, 0.8};
    return t;
}

// Transcribed from src/themes/muted-teal-orange.ts.
inline Theme mutedTealOrangeTheme() {
    Theme t;
    t.name = "muted-teal-orange";
    t.description = "Understated desaturated teal-orange, refined and modern.";
    t.targetStats = FootageStats{
        {0.03, 0.06, 0.24, 0.47, 0.69, 0.87, 0.94},
        {50, 4, 6},
        {20, 10, 12},
        {10, 14, 10},
        {0.26, 0.14},
        0,
        {0.002, 0.002},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{-5, -8};
    ov.highlightTint = std::array<double, 2>{3, 6};
    ov.chromaGain = 0.82;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0.01}, {0.25, 0.25}, {0.5, 0.49}, {0.75, 0.72}, {1, 0.97}};
    ChromaShape cs;
    cs.vibrance = -0.05;
    cs.softLimit = 45;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.75, 0.8};
    return t;
}

// Transcribed from src/themes/monochrome-bw.ts. chromaGain 0 + skinProtection 0
// + strength 1.0 force a true grayscale (the engine has no desaturate knob); the
// large cast/skin numbers are the look working as designed.
inline Theme monochromeBwTheme() {
    Theme t;
    t.name = "monochrome-bw";
    t.description = "True black-and-white, punchy silver-print contrast.";
    t.targetStats = FootageStats{
        {0.01, 0.03, 0.2, 0.44, 0.7, 0.93, 0.99},
        {46, 0, 0},
        {26, 1, 1},
        {0, 0, 0},
        {0, 0},
        0,
        {0.003, 0.004},
    };
    ThemeOverrides ov;
    ov.chromaGain = 0;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0}, {0.25, 0.21}, {0.5, 0.5}, {0.75, 0.79}, {1, 1}};
    ChromaShape cs;
    cs.softLimit = 1;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {1.0, 0};
    return t;
}

// Transcribed from src/themes/sepia.ts. Like monochrome-bw (chromaGain 0,
// skinProtection 0, strength 1.0) but the band tints paint one warm tone back in.
inline Theme sepiaTheme() {
    Theme t;
    t.name = "sepia";
    t.description = "Warm-toned antique monochrome print.";
    t.targetStats = FootageStats{
        {0.03, 0.06, 0.24, 0.48, 0.72, 0.9, 0.97},
        {48, 6, 18},
        {22, 1, 1},
        {0, 0, 0},
        {0, 0},
        0,
        {0.002, 0.002},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{5, 12};
    ov.midtoneTint = std::array<double, 2>{7, 16};
    ov.highlightTint = std::array<double, 2>{8, 20};
    ov.chromaGain = 0;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0.03}, {0.25, 0.27}, {0.5, 0.5}, {0.75, 0.73}, {1, 0.98}};
    ChromaShape cs;
    cs.softLimit = 30;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {1.0, 0};
    return t;
}

// Transcribed from src/themes/cinematic-green.ts. Intentionally recolours skin
// (best for non-portrait footage); skinProtection 0.72 by design.
inline Theme cinematicGreenTheme() {
    Theme t;
    t.name = "cinematic-green";
    t.description = "Sickly digital-rain green cast, desaturated and clinical.";
    t.targetStats = FootageStats{
        {0.01, 0.03, 0.18, 0.42, 0.68, 0.9, 0.97},
        {46, -6, 8},
        {22, 8, 10},
        {8, 12, 10},
        {0.22, 0.12},
        0,
        {0.003, 0.003},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{-8, 6};
    ov.midtoneTint = std::array<double, 2>{-6, 8};
    ov.highlightTint = std::array<double, 2>{-4, 10};
    ov.chromaGain = 0.7;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0}, {0.25, 0.22}, {0.5, 0.48}, {0.75, 0.73}, {1, 0.97}};
    ChromaShape cs;
    cs.vibrance = -0.1;
    cs.softLimit = 40;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.8, 0.72};
    return t;
}

// Transcribed from src/themes/desaturated-doc.ts.
inline Theme desaturatedDocTheme() {
    Theme t;
    t.name = "desaturated-doc";
    t.description = "Muted, natural, neutral documentary colour.";
    t.targetStats = FootageStats{
        {0.02, 0.05, 0.23, 0.46, 0.68, 0.87, 0.95},
        {48, 1, 5},
        {20, 8, 10},
        {8, 12, 10},
        {0.22, 0.12},
        0,
        {0.002, 0.002},
    };
    ThemeOverrides ov;
    ov.chromaGain = 0.75;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0.01}, {0.25, 0.25}, {0.5, 0.48}, {0.75, 0.71}, {1, 0.97}};
    ChromaShape cs;
    cs.vibrance = -0.1;
    cs.softLimit = 40;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.7, 0.8};
    return t;
}

// Transcribed from src/themes/punchy-social.ts.
inline Theme punchySocialTheme() {
    Theme t;
    t.name = "punchy-social";
    t.description = "Bright, vivid, high-energy social/vlog pop.";
    t.targetStats = FootageStats{
        {0.02, 0.05, 0.24, 0.5, 0.74, 0.94, 0.99},
        {54, 3, 8},
        {24, 14, 15},
        {13, 20, 15},
        {0.4, 0.2},
        0,
        {0.002, 0.003},
    };
    ThemeOverrides ov;
    ov.chromaGain = 1.12;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0}, {0.25, 0.23}, {0.5, 0.51}, {0.75, 0.79}, {1, 1}};
    ChromaShape cs;
    cs.byLuma = std::vector<CurvePoint>{{0, 0.95}, {0.5, 1.05}, {1, 0.95}};
    cs.vibrance = 0.2;
    cs.softLimit = 60;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.82, 0.8};
    return t;
}

// Transcribed from src/themes/cross-process.ts.
inline Theme crossProcessTheme() {
    Theme t;
    t.name = "cross-process";
    t.description = "Cyan shadows, yellow-green highlights, high contrast, boosted chroma.";
    t.targetStats = FootageStats{
        {0.005, 0.02, 0.16, 0.42, 0.72, 0.94, 0.99},
        {48, -2, 6},
        {26, 13, 14},
        {12, 16, 14},
        {0.34, 0.18},
        0,
        {0.005, 0.004},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{-6, -10};
    ov.highlightTint = std::array<double, 2>{-4, 14};
    ov.chromaGain = 1.05;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0}, {0.2, 0.14}, {0.5, 0.5}, {0.8, 0.88}, {1, 1}};
    ChromaShape cs;
    cs.vibrance = 0.1;
    cs.softLimit = 55;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.8, 0.78};
    return t;
}

// Transcribed from src/themes/rose-romance.ts.
inline Theme roseRomanceTheme() {
    Theme t;
    t.name = "rose-romance";
    t.description = "Soft warm rosy glow, lifted blacks, gentle contrast.";
    t.targetStats = FootageStats{
        {0.06, 0.1, 0.3, 0.52, 0.73, 0.9, 0.96},
        {56, 8, 8},
        {17, 9, 10},
        {9, 14, 12},
        {0.28, 0.14},
        0,
        {0.001, 0.002},
    };
    ThemeOverrides ov;
    ov.shadowTint = std::array<double, 2>{4, 2};
    ov.midtoneTint = std::array<double, 2>{5, 3};
    ov.highlightTint = std::array<double, 2>{6, 6};
    ov.chromaGain = 0.9;
    ov.toneCurve =
        std::vector<CurvePoint>{{0, 0.05}, {0.25, 0.3}, {0.5, 0.52}, {0.75, 0.74}, {1, 0.97}};
    ChromaShape cs;
    cs.byLuma = std::vector<CurvePoint>{{0, 0.95}, {0.5, 1.02}, {1, 0.95}};
    cs.softLimit = 50;
    ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {0.72, 0.8};
    return t;
}

// Look a theme up by the same key strings src/themes/index.ts uses.
inline bool getTheme(const std::string& key, Theme& out) {
    if (key == "teal-orange") { out = tealOrangeTheme(); return true; }
    if (key == "warm-film") { out = warmFilmTheme(); return true; }
    if (key == "cool-noir") { out = coolNoirTheme(); return true; }
    if (key == "none-manual") { out = noneManualTheme(); return true; }
    if (key == "golden-hour") { out = goldenHourTheme(); return true; }
    if (key == "bleach-bypass") { out = bleachBypassTheme(); return true; }
    if (key == "vintage-fade") { out = vintageFadeTheme(); return true; }
    if (key == "high-key-clean") { out = highKeyCleanTheme(); return true; }
    if (key == "low-key-moody") { out = lowKeyMoodyTheme(); return true; }
    if (key == "winter-blue") { out = winterBlueTheme(); return true; }
    if (key == "warm-portrait") { out = warmPortraitTheme(); return true; }
    if (key == "pastel-dream") { out = pastelDreamTheme(); return true; }
    if (key == "neon-cyberpunk") { out = neonCyberpunkTheme(); return true; }
    if (key == "day-for-night") { out = dayForNightTheme(); return true; }
    if (key == "autumn") { out = autumnTheme(); return true; }
    if (key == "summer-blockbuster") { out = summerBlockbusterTheme(); return true; }
    if (key == "muted-teal-orange") { out = mutedTealOrangeTheme(); return true; }
    if (key == "monochrome-bw") { out = monochromeBwTheme(); return true; }
    if (key == "sepia") { out = sepiaTheme(); return true; }
    if (key == "cinematic-green") { out = cinematicGreenTheme(); return true; }
    if (key == "desaturated-doc") { out = desaturatedDocTheme(); return true; }
    if (key == "punchy-social") { out = punchySocialTheme(); return true; }
    if (key == "cross-process") { out = crossProcessTheme(); return true; }
    if (key == "rose-romance") { out = roseRomanceTheme(); return true; }
    return false;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_THEMES_H
