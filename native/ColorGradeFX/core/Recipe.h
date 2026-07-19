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
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../lut/CubeLut.h"
#include "BakeLut.h"
#include "Engine.h"
#include "Stats.h"
#include "Theme.h"

namespace cg {
namespace core {

constexpr uint32_t RECIPE_MAGIC = 0x43475244;  // 'CGRD'
// v2 added midtoneTint (PR #24). v3 (Phase 6a) added the matchStats flag + the
// manual primary-correction block + lookMix. v4 (Phase 6c) added the DaVinci
// Lift/Gamma/Gain wheels + the editor wheels-mode flag. The unflatten handler
// MIGRATES older versions forward (copy the shared prefix, default the new fields)
// rather than reseeding, so grades saved by an earlier build survive - see ColorGrade.cpp.
constexpr uint32_t RECIPE_VERSION = 4;
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

    // --- v3 (Phase 6a) additions. Kept AFTER all v2 fields so a v2 blob's bytes
    //     land identically and the migration only defaults these. ---
    // Whether the theme runs the automatic stat-match look (0 = None/Manual theme).
    uint32_t matchStats = 1;
    // Manual primary correction (editor state). All neutral = identity. Exposure,
    // temperature, and lookMix are ALSO exposed as keyframeable PF params; the
    // effect overrides these three with the live PF value at bake time (decision D1).
    double manualExposure = 0.0;
    double manualContrast = 0.0;
    double manualPivot = 0.435;
    double manualHighlights = 0.0;
    double manualShadows = 0.0;
    double manualWhites = 0.0;
    double manualBlacks = 0.0;
    double manualTemperature = 0.0;
    double manualTint = 0.0;
    double manualSaturation = 1.0;
    double manualVibrance = 0.0;
    double lookMix = 1.0;

    // --- v4 (Phase 6b/6c) additions. Kept AFTER all v3 fields so a v3 blob's bytes
    //     land identically and the migration only defaults these. `lift` MUST stay the
    //     FIRST v4 field (RECIPE_V3_SIZE == offsetof(RecipeData, lift)). ---
    //
    // These are EDITOR-OWNED fields, distinct from the theme-seeded overrides above
    // (toneCurve/shadowTint/... which recipeFromTheme seeds from the picked theme).
    // The effect's Auto bake takes the theme LOOK from the Theme popup, then COMPOSES
    // these editor layers on top: user curves REPLACE the theme's authored curve per
    // slot; user 3-way tints ADD to the theme's authored band tints; LGG is its own
    // engine stage. All default to neutral/empty, so an old grade (v2/v3) or a
    // theme with no edits bakes exactly the theme look - and switching themes never
    // applies a stale editor override.
    //
    // DaVinci Lift/Gamma/Gain wheels. Neutral (lift 0, gamma 1, gain 1) = exact identity.
    double lift[3] = {0.0, 0.0, 0.0};
    double gamma[3] = {1.0, 1.0, 1.0};
    double gain[3] = {1.0, 1.0, 1.0};
    // Curves tab (6b): user master + per-channel curves. count 0 = absent (the theme's
    // authored curve is used for that slot); present = replaces it.
    CurveData userToneCurve{};
    CurveData userChannelR{};
    CurveData userChannelG{};
    CurveData userChannelB{};
    // Adobe 3-way (6c) per-band color discs -> LAB [a,b] tints, ADDED to the theme's
    // authored band tints. {0,0} = no user tint. Distinct from the theme-seeded
    // shadowTint/midtoneTint/highlightTint above so a theme switch stays clean.
    double userShadowTint[2] = {0.0, 0.0};
    double userMidTint[2] = {0.0, 0.0};
    double userHighTint[2] = {0.0, 0.0};
    // Which wheels face the editor last showed (0 = Lift/Gamma/Gain, 1 = Adobe
    // 3-way). Pure UI state; the engine ignores it.
    uint32_t wheelsMode = 0;
};

// Combine a theme-authored band tint (optional) with the editor's additive user tint.
inline std::optional<std::array<double, 2>> combineBandTint(
    const std::optional<std::array<double, 2>>& authored, const double user[2]) {
    const bool hasUser = user[0] != 0.0 || user[1] != 0.0;
    if (!authored && !hasUser) return std::nullopt;
    const double a0 = authored ? (*authored)[0] : 0.0;
    const double a1 = authored ? (*authored)[1] : 0.0;
    return std::array<double, 2>{a0 + user[0], a1 + user[1]};
}


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

// --- reference-match (Phase 7, "match this look" - data/cg-agents-study/report.md sec
//     1d): a reference still is just a Theme whose target stats are computed live. No
//     image codec lives in native/, so the native editor's minimal entry point is the
//     Theme popup's "Reference Match" choice (ColorGrade.cpp's ThemeFromPopup), which
//     loads a small text sidecar of pre-computed stats - the actual image decode +
//     computeStats happens on the TS side (src/panel/referenceMatch.ts), where the
//     tested FrameSource/TIFF/PNG pipeline already lives. ---------------------------

// Build the reference-matched Theme: NO authored overrides, matchStats left true, so
// the engine's full automatic look (tone-curve stat match, LAB transfer, per-band
// chroma scale, AND the chroma-overshoot guard) applies exactly as for any other
// theme - zero new engine math. See src/core/engine/referenceTheme.ts's doc comment
// (the TS oracle for this Theme shape) for the artifact-risk limitations this does
// NOT solve (that guard only engages on large tone-curve stretch, not small-stretch
// local chroma noise) - unchanged by the native port, since there is no new logic here.
inline Theme referenceMatchTheme(const FootageStats& stats) {
    Theme t;
    t.name = "reference-match";
    t.description = "Look matched from a reference image (stat transfer, no authored overrides).";
    t.targetStats = stats;
    t.matchStats = true;
    t.knobs = {1.0, 0.5};
    return t;
}

// Parse the reference-stats sidecar text format: STATS_FIELDS (21) whitespace/comma-
// separated decimal numbers in the canonical StatsData order (see statsToData). Mirrors
// (and round-trips against) the TS writer/parser (src/core/analysis/referenceStats.ts).
// Hand-rolled (no JSON dependency), same spirit as CubeLut.h's hand-rolled .cube parser.
// Returns false (leaving `out` untouched) on malformed input (wrong token count).
inline bool parseReferenceStatsText(const std::string& text, StatsData& out) {
    StatsData d{};
    int count = 0;
    size_t i = 0;
    while (i < text.size() && count < STATS_FIELDS) {
        while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ',')) i++;
        const size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != ',') i++;
        if (i == start) break;
        d.v[count++] = std::strtod(text.substr(start, i - start).c_str(), nullptr);
    }
    while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ',')) i++;
    if (count != STATS_FIELDS || i != text.size()) return false;  // wrong token count
    out = d;
    return true;
}

// Serialize a StatsData in the canonical order, one value per line - the counterpart to
// parseReferenceStatsText, exercised by the parity harness's round-trip self-test.
inline std::string formatReferenceStatsText(const StatsData& d) {
    std::string out;
    char buf[64];
    for (int i = 0; i < STATS_FIELDS; i++) {
        std::snprintf(buf, sizeof(buf), "%.17g\n", d.v[i]);
        out += buf;
    }
    return out;
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

// Compose the editor-owned look layers onto a set of theme overrides: the 6b user curves
// REPLACE the authored curve per slot; the 6c 3-way user tints ADD to the authored band
// tints. Shared by the pure themeFromRecipe path AND the effect's Auto bake (BakeAutoLut),
// so the render path and the golden harness exercise IDENTICAL composition - the regression
// guard for "editor curve/wheel edits actually reach the baked LUT". (LGG is a separate
// EngineOptions stage, folded in by the caller via lggFromRecipe.)
inline void applyEditorOverrides(ThemeOverrides& ov, const RecipeData& r) {
    ov.shadowTint = combineBandTint(ov.shadowTint, r.userShadowTint);
    ov.highlightTint = combineBandTint(ov.highlightTint, r.userHighTint);
    ov.midtoneTint = combineBandTint(ov.midtoneTint, r.userMidTint);
    if (r.userToneCurve.count > 0) ov.toneCurve = curveFromData(r.userToneCurve);
    ChannelCurves cc = ov.channelCurves.value_or(ChannelCurves{});
    if (r.userChannelR.count > 0) cc.r = curveFromData(r.userChannelR);
    if (r.userChannelG.count > 0) cc.g = curveFromData(r.userChannelG);
    if (r.userChannelB.count > 0) cc.b = curveFromData(r.userChannelB);
    if (cc.r || cc.g || cc.b) ov.channelCurves = cc;
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
    r.matchStats = theme.matchStats ? 1u : 0u;
    // Manual block + lookMix + LGG wheels keep their neutral defaults: manual grading
    // and the wheels are editor state, not theme data, so a freshly-seeded recipe
    // carries a neutral manual grade and neutral wheels.
    return r;
}

// Reconstruct a Theme (targetStats + overrides + knobs) from a recipe. The name
// is cosmetic; buildTransform reads only stats/overrides/knobs.
inline Theme themeFromRecipe(const RecipeData& r) {
    Theme t;
    t.name = "recipe";
    t.targetStats = statsFromData(r.targetStats);
    ThemeOverrides ov;
    // Start from the theme-authored overrides (as seeded by recipeFromTheme)...
    if (r.hasShadowTint) ov.shadowTint = std::array<double, 2>{r.shadowTint[0], r.shadowTint[1]};
    if (r.hasHighlightTint)
        ov.highlightTint = std::array<double, 2>{r.highlightTint[0], r.highlightTint[1]};
    if (r.hasMidtoneTint)
        ov.midtoneTint = std::array<double, 2>{r.midtoneTint[0], r.midtoneTint[1]};
    if (r.hasChromaGain) ov.chromaGain = r.chromaGain;
    ov.toneCurve = curveFromData(r.toneCurve);
    {
        ChannelCurves cc;
        cc.r = curveFromData(r.channelR);
        cc.g = curveFromData(r.channelG);
        cc.b = curveFromData(r.channelB);
        if (cc.r || cc.g || cc.b) ov.channelCurves = cc;
    }
    // ...then compose the editor-owned layers on top (the SAME helper the effect's Auto
    // bake uses, so both agree on how curve/wheel edits reach the LUT).
    applyEditorOverrides(ov, r);
    ChromaShape cs;
    cs.byLuma = curveFromData(r.chromaByLuma);
    if (r.hasVibrance) cs.vibrance = r.vibrance;
    if (r.hasSoftLimit) cs.softLimit = r.softLimit;
    if (cs.byLuma || cs.vibrance || cs.softLimit) ov.chromaShape = cs;
    t.overrides = ov;
    t.knobs = {r.strengthDefault, r.skinProtectionDefault};
    t.matchStats = r.matchStats != 0;
    return t;
}

// Reconstruct the manual primary correction (EngineOptions.manual) from a recipe.
inline ManualGrade manualFromRecipe(const RecipeData& r) {
    ManualGrade m;
    m.exposure = r.manualExposure;
    m.contrast = r.manualContrast;
    m.pivot = r.manualPivot;
    m.highlights = r.manualHighlights;
    m.shadows = r.manualShadows;
    m.whites = r.manualWhites;
    m.blacks = r.manualBlacks;
    m.temperature = r.manualTemperature;
    m.tint = r.manualTint;
    m.saturation = r.manualSaturation;
    m.vibrance = r.manualVibrance;
    return m;
}

// Reconstruct the Lift/Gamma/Gain wheels (EngineOptions.lgg) from a recipe (Phase 6c).
inline LiftGammaGain lggFromRecipe(const RecipeData& r) {
    LiftGammaGain lg;
    for (int c = 0; c < 3; ++c) {
        lg.lift[c] = r.lift[c];
        lg.gamma[c] = r.gamma[c];
        lg.gain[c] = r.gain[c];
    }
    return lg;
}

// In-effect bake: build the grade LUT the effect applies, straight from arb-data.
// opts overrides (strength / skinProtection sliders, and the live PF manual/lookMix)
// win over the recipe. When the caller leaves manual / lgg / lookMix unset, they are
// taken from the recipe, so the bake carries the persisted manual grade + wheels.
inline cg::Lut3D bakeFromRecipe(const RecipeData& r, const EngineOptions& opts = {}, int size = 33) {
    EngineOptions merged = opts;
    if (!merged.manual) merged.manual = manualFromRecipe(r);
    if (!merged.lgg) merged.lgg = lggFromRecipe(r);
    if (!merged.lookMix) merged.lookMix = r.lookMix;
    return bakeGradeLut(statsFromData(r.sourceStats), themeFromRecipe(r), merged, size);
}

// --- versioned arb-data migration (the Phase 6a landmine) ------------------
//
// Byte size of the v2 / v3 RecipeData layouts: every later field was appended AFTER
// the earlier fields, so an older field's offset is exactly the older struct size and
// an older blob's bytes are a clean prefix of the current struct. v2 ended before
// matchStats (first v3 field); v3 ended before lift (first v4 field).
constexpr std::size_t RECIPE_V2_SIZE = offsetof(RecipeData, matchStats);
constexpr std::size_t RECIPE_V3_SIZE = offsetof(RecipeData, lift);

// Migrate a persisted (flattened) recipe blob into a current RecipeData. Old grades
// must survive a version bump: a same-version blob copies verbatim; an older (v2/v3)
// blob copies its shared prefix over current defaults (new fields default to neutral:
// matchStats 1, manual neutral, lookMix 1, LGG lift 0 / gamma 1 / gain 1, wheelsMode 0)
// and is re-stamped to the current version; anything foreign/corrupt/unknown falls back
// to `fallback` (the caller supplies the default recipe, keeping this header free of a
// Themes.h dependency). Single source of truth for the AE arb-data UNFLATTEN handler and
// the parity harness.
inline void migrateRecipeInto(RecipeData* dst, const void* flat, std::size_t flatSize,
                              const RecipeData& fallback) {
    RecipeData result = fallback;  // used verbatim only on the foreign/corrupt path
    uint32_t magic = 0, ver = 0;
    if (flatSize >= 2 * sizeof(uint32_t)) {
        std::memcpy(&magic, flat, sizeof(uint32_t));
        std::memcpy(&ver, static_cast<const char*>(flat) + sizeof(uint32_t), sizeof(uint32_t));
    }
    if (magic == RECIPE_MAGIC && ver == RECIPE_VERSION && flatSize == sizeof(RecipeData)) {
        std::memcpy(&result, flat, sizeof(RecipeData));  // current version: verbatim
    } else if (magic == RECIPE_MAGIC && ver == 3 && flatSize == RECIPE_V3_SIZE) {
        result = RecipeData{};                       // start from current-version defaults
        std::memcpy(&result, flat, RECIPE_V3_SIZE);  // v3 prefix over those defaults
        result.version = RECIPE_VERSION;             // stamp forward; new fields defaulted
    } else if (magic == RECIPE_MAGIC && ver == 2 && flatSize == RECIPE_V2_SIZE) {
        result = RecipeData{};                       // start from current-version defaults
        std::memcpy(&result, flat, RECIPE_V2_SIZE);  // v2 prefix over those defaults
        result.version = RECIPE_VERSION;             // stamp forward; new fields defaulted
    }
    std::memcpy(dst, &result, sizeof(RecipeData));
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_RECIPE_H
