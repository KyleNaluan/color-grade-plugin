/*
 * Recipe.h - the POD grade recipe that the AE effect persists as arb-data, plus
 * the pure conversion to the ported engine's Theme/FootageStats and the
 * in-effect bake. Host-agnostic (no AE SDK types), so this whole path - the
 * flatten-safe recipe, the round-trip, and the bake - is exercised by the
 * cross-engine golden harness, not only in AE.
 *
 * Disk-safety (the AE arb-data requirement): RecipeData is a flat POD with no
 * pointers and only fixed-size members, so the effect's PF_Arbitrary FLATTEN /
 * UNFLATTEN handlers are a straight byte copy. A magic + version guard rejects a
 * stale or foreign blob. Curves are fixed-capacity (RECIPE_MAX_POINTS); the
 * shipping themes use <= 5 control points, so this is ample headroom.
 *
 * Canonical FootageStats flattening (statsToData / statsFromData) is the single
 * source of truth shared with core_parity.cpp and core-parity-test.ts.
 */
#pragma once
#ifndef CG_CORE_RECIPE_H
#define CG_CORE_RECIPE_H

#include <array>
#include <cstdint>
#include <vector>

#include "../lut/CubeLut.h"
#include "BakeLut.h"
#include "Engine.h"
#include "Stats.h"
#include "Theme.h"

namespace cg {
namespace core {

constexpr uint32_t RECIPE_MAGIC = 0x43475244;  // 'CGRD'
constexpr uint32_t RECIPE_VERSION = 2;  // v2 added midtoneTint (PR #24)
constexpr int RECIPE_MAX_POINTS = 16;
constexpr int STATS_FIELDS = 21;

// Flat, POD stat vector in canonical order (mirrors flattenStats in the harness).
struct StatsData {
    double v[STATS_FIELDS];
};

struct CurveData {
    int32_t count = 0;  // 0 => absent
    CurvePoint pts[RECIPE_MAX_POINTS] = {};
};

// The full grade recipe. POD: trivially flatten/unflatten-able for AE arb-data.
struct RecipeData {
    uint32_t magic = RECIPE_MAGIC;
    uint32_t version = RECIPE_VERSION;

    StatsData sourceStats{};  // measured footage (drives buildTransform's `src`)
    StatsData targetStats{};  // theme target

    uint32_t hasShadowTint = 0;
    double shadowTint[2] = {0, 0};
    uint32_t hasHighlightTint = 0;
    double highlightTint[2] = {0, 0};
    uint32_t hasMidtoneTint = 0;
    double midtoneTint[2] = {0, 0};
    uint32_t hasChromaGain = 0;
    double chromaGain = 1.0;

    CurveData toneCurve{};
    CurveData channelR{};
    CurveData channelG{};
    CurveData channelB{};
    CurveData chromaByLuma{};

    uint32_t hasVibrance = 0;
    double vibrance = 0.0;
    uint32_t hasSoftLimit = 0;
    double softLimit = 0.0;

    double strengthDefault = 0.8;
    double skinProtectionDefault = 0.75;
};

// --- canonical FootageStats <-> flat 21-double vector ---------------------

inline StatsData statsToData(const FootageStats& s) {
    const auto& p = s.lumaPercentiles;
    StatsData d{};
    const double vals[STATS_FIELDS] = {
        p.p1, p.p5, p.p25, p.p50, p.p75, p.p95, p.p99,
        s.labMean[0], s.labMean[1], s.labMean[2],
        s.labStd[0], s.labStd[1], s.labStd[2],
        s.bandChroma.shadows, s.bandChroma.mids, s.bandChroma.highlights,
        s.saturation.mean, s.saturation.std,
        s.skinPresence,
        s.clipping.low, s.clipping.high,
    };
    for (int i = 0; i < STATS_FIELDS; i++) d.v[i] = vals[i];
    return d;
}

inline FootageStats statsFromData(const StatsData& d) {
    FootageStats s;
    s.lumaPercentiles = {d.v[0], d.v[1], d.v[2], d.v[3], d.v[4], d.v[5], d.v[6]};
    s.labMean = {d.v[7], d.v[8], d.v[9]};
    s.labStd = {d.v[10], d.v[11], d.v[12]};
    s.bandChroma = {d.v[13], d.v[14], d.v[15]};
    s.saturation = {d.v[16], d.v[17]};
    s.skinPresence = d.v[18];
    s.clipping = {d.v[19], d.v[20]};
    return s;
}

// --- curve <-> CurveData ---------------------------------------------------

inline CurveData curveToData(const std::optional<std::vector<CurvePoint>>& pts) {
    CurveData c{};
    if (!pts) return c;
    const int n = std::min(static_cast<int>(pts->size()), RECIPE_MAX_POINTS);
    c.count = n;
    for (int i = 0; i < n; i++) c.pts[i] = (*pts)[i];
    return c;
}

inline std::optional<std::vector<CurvePoint>> curveFromData(const CurveData& c) {
    if (c.count <= 0) return std::nullopt;
    const int n = std::min(c.count, RECIPE_MAX_POINTS);
    std::vector<CurvePoint> pts(n);
    for (int i = 0; i < n; i++) pts[i] = c.pts[i];
    return pts;
}

// --- Theme <-> RecipeData --------------------------------------------------

// Seed a recipe from a built-in Theme + a measured source-stats analysis. Used
// by the effect when the user picks a theme (before/without a custom edit) and
// by the golden harness to prove the round-trip.
inline RecipeData recipeFromTheme(const Theme& theme, const FootageStats& source) {
    RecipeData r;
    r.sourceStats = statsToData(source);
    r.targetStats = statsToData(theme.targetStats);
    const ThemeOverrides ov = theme.overrides.value_or(ThemeOverrides{});
    if (ov.shadowTint) {
        r.hasShadowTint = 1;
        r.shadowTint[0] = (*ov.shadowTint)[0];
        r.shadowTint[1] = (*ov.shadowTint)[1];
    }
    if (ov.highlightTint) {
        r.hasHighlightTint = 1;
        r.highlightTint[0] = (*ov.highlightTint)[0];
        r.highlightTint[1] = (*ov.highlightTint)[1];
    }
    if (ov.midtoneTint) {
        r.hasMidtoneTint = 1;
        r.midtoneTint[0] = (*ov.midtoneTint)[0];
        r.midtoneTint[1] = (*ov.midtoneTint)[1];
    }
    if (ov.chromaGain) {
        r.hasChromaGain = 1;
        r.chromaGain = *ov.chromaGain;
    }
    r.toneCurve = curveToData(ov.toneCurve);
    if (ov.channelCurves) {
        r.channelR = curveToData(ov.channelCurves->r);
        r.channelG = curveToData(ov.channelCurves->g);
        r.channelB = curveToData(ov.channelCurves->b);
    }
    if (ov.chromaShape) {
        r.chromaByLuma = curveToData(ov.chromaShape->byLuma);
        if (ov.chromaShape->vibrance) {
            r.hasVibrance = 1;
            r.vibrance = *ov.chromaShape->vibrance;
        }
        if (ov.chromaShape->softLimit) {
            r.hasSoftLimit = 1;
            r.softLimit = *ov.chromaShape->softLimit;
        }
    }
    r.strengthDefault = theme.knobs.strengthDefault;
    r.skinProtectionDefault = theme.knobs.skinProtectionDefault;
    return r;
}

// Reconstruct a Theme (targetStats + overrides + knobs) from a recipe. The name
// is cosmetic; buildTransform reads only stats/overrides/knobs.
inline Theme themeFromRecipe(const RecipeData& r) {
    Theme t;
    t.name = "recipe";
    t.targetStats = statsFromData(r.targetStats);
    ThemeOverrides ov;
    if (r.hasShadowTint) ov.shadowTint = std::array<double, 2>{r.shadowTint[0], r.shadowTint[1]};
    if (r.hasHighlightTint)
        ov.highlightTint = std::array<double, 2>{r.highlightTint[0], r.highlightTint[1]};
    if (r.hasMidtoneTint)
        ov.midtoneTint = std::array<double, 2>{r.midtoneTint[0], r.midtoneTint[1]};
    if (r.hasChromaGain) ov.chromaGain = r.chromaGain;
    ov.toneCurve = curveFromData(r.toneCurve);
    ChannelCurves cc;
    cc.r = curveFromData(r.channelR);
    cc.g = curveFromData(r.channelG);
    cc.b = curveFromData(r.channelB);
    if (cc.r || cc.g || cc.b) ov.channelCurves = cc;
    ChromaShape cs;
    cs.byLuma = curveFromData(r.chromaByLuma);
    if (r.hasVibrance) cs.vibrance = r.vibrance;
    if (r.hasSoftLimit) cs.softLimit = r.softLimit;
    if (cs.byLuma || cs.vibrance || cs.softLimit) ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {r.strengthDefault, r.skinProtectionDefault};
    return t;
}

// In-effect bake: build the grade LUT the effect applies, straight from arb-data.
// opts overrides (strength / skinProtection sliders) win over the recipe knobs.
inline cg::Lut3D bakeFromRecipe(const RecipeData& r, const EngineOptions& opts = {}, int size = 33) {
    return bakeGradeLut(statsFromData(r.sourceStats), themeFromRecipe(r), opts, size);
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_RECIPE_H
