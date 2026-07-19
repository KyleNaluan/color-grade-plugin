/*
 * core_parity.cpp - C++ side of the Phase 2-6c cross-engine golden harness.
 *
 * Replays the ported core (computeStats -> buildTransform -> bakeLut, and the
 * Decode LUT bake) over inputs supplied by the TS oracle
 * (native/scripts/core-parity-test.ts) and writes the results as binary so the
 * oracle can diff them within ~1e-4. No AE SDK: compiled with host g++/clang.
 *
 * Subcommands (all write their result to <out>):
 *   stats  <pixels.f32> <nFloats> <out.f64>
 *       computeStats over the pixel buffer -> 21 doubles in canonical order.
 *   grade  <pixels.f32> <nFloats> <theme> <hasStr> <str> <hasSkin> <skin> <size> <out.f32>
 *       computeStats then bakeGradeLut(stats, theme, opts, size) -> LUT float32 grid.
 *   decode <profile> <size> <out.f32>
 *       bakeDecodeLut(profile, size) -> LUT float32 grid.
 *   recipe <pixels.f32> <nFloats> <theme> <hasStr> <str> <hasSkin> <skin> <size> <out.f32>
 *       computeStats then recipeFromTheme -> bakeFromRecipe (the arb-data path);
 *       must match the plain `grade` result, proving the POD recipe round-trip.
 *
 * Later phases add further subcommands over the same channel (see the TS oracle for
 * their exact arg lists): `grademid`/`chromaslider` (theme override + knob), the
 * `*decode` family (Phase 3 decode composition), `lutcorrect` (Correct/Basics folded
 * under a user LUT, fm/cg-lut-correct-stack), `grademanual`/`recipemanual` (Phase 6a
 * manual stage + arb round-trip), `gradelgg`/`recipelgg` (Phase 6c DaVinci LGG stage),
 * `recipeeditor` (the Phase 6b/6c `applyEditorOverrides` render-path composition guard),
 * `migrate` (v2/v3 -> v4 recipe migration), `referencematch` (Phase 7 reference-image
 * look matching, report sec 1d), and `referencestatsformat` (the reference-stats
 * sidecar text format self-test).
 *
 * Buffers are raw little-endian arrays (matching JS Float32Array / Float64Array)
 * on the shared x86-64 host; not a portable serialization, just a test channel.
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../../ColorGradeFX/core/BakeLut.h"
#include "../../ColorGradeFX/core/LogProfile.h"
#include "../../ColorGradeFX/core/Recipe.h"
#include "../../ColorGradeFX/core/Stats.h"
#include "../../ColorGradeFX/core/Theme.h"
#include "../../ColorGradeFX/core/Themes.h"

using namespace cg::core;

static std::vector<float> readF32(const char* path, size_t nFloats) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "core_parity: cannot open %s\n", path); std::exit(2); }
    std::vector<float> v(nFloats);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(nFloats * sizeof(float)));
    if (static_cast<size_t>(f.gcount()) != nFloats * sizeof(float)) {
        std::fprintf(stderr, "core_parity: short read on %s\n", path);
        std::exit(2);
    }
    return v;
}

static void writeF32(const char* path, const std::vector<float>& v) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(v.data()),
            static_cast<std::streamsize>(v.size() * sizeof(float)));
}

static void writeF64(const char* path, const std::vector<double>& v) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(v.data()),
            static_cast<std::streamsize>(v.size() * sizeof(double)));
}

// Canonical FootageStats flattening - shared with Recipe.h::statsToData so the
// harness, arb-data recipe, and effect all agree on the field order.
static std::vector<double> flattenStats(const FootageStats& s) {
    const StatsData d = statsToData(s);
    return std::vector<double>(d.v, d.v + STATS_FIELDS);
}

// Parse a manual-grade CSV "exp,con,piv,hi,sh,wh,bl,temp,tint,sat,vib,lookMix" into a
// ManualGrade + lookMix (mirrors the TS harness manualToCsv order).
static void parseManual(const char* csv, ManualGrade& m, double& lookMix) {
    double v[12] = {0, 0, 0.435, 0, 0, 0, 0, 0, 0, 1, 0, 1};
    const char* p = csv;
    for (int i = 0; i < 12 && p && *p; i++) {
        v[i] = std::atof(p);
        const char* c = std::strchr(p, ',');
        p = c ? c + 1 : nullptr;
    }
    m.exposure = v[0];
    m.contrast = v[1];
    m.pivot = v[2];
    m.highlights = v[3];
    m.shadows = v[4];
    m.whites = v[5];
    m.blacks = v[6];
    m.temperature = v[7];
    m.tint = v[8];
    m.saturation = v[9];
    m.vibrance = v[10];
    lookMix = v[11];
}

// Parse an LGG CSV "liftR,liftG,liftB,gammaR,gammaG,gammaB,gainR,gainG,gainB" into a
// LiftGammaGain (mirrors the TS harness lggCsv order).
static void parseLgg(const char* csv, LiftGammaGain& lg) {
    double v[9] = {0, 0, 0, 1, 1, 1, 1, 1, 1};
    const char* p = csv;
    for (int i = 0; i < 9 && p && *p; i++) {
        v[i] = std::atof(p);
        const char* c = std::strchr(p, ',');
        p = c ? c + 1 : nullptr;
    }
    for (int c = 0; c < 3; c++) {
        lg.lift[c] = v[c];
        lg.gamma[c] = v[3 + c];
        lg.gain[c] = v[6 + c];
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: core_parity <stats|grade|decode> ...\n"); return 2; }
    const std::string cmd = argv[1];

    if (cmd == "stats" && argc == 5) {
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        writeF64(argv[4], flattenStats(computeStats(px.data(), px.size())));
        return 0;
    }

    if (cmd == "grade" && argc == 11) {
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        Theme theme;
        if (!getTheme(argv[4], theme)) {
            std::fprintf(stderr, "core_parity: unknown theme %s\n", argv[4]);
            return 2;
        }
        EngineOptions opts;
        if (std::atoi(argv[5])) opts.strength = std::atof(argv[6]);
        if (std::atoi(argv[7])) opts.skinProtection = std::atof(argv[8]);
        const int size = std::atoi(argv[9]);
        FootageStats stats = computeStats(px.data(), px.size());
        cg::Lut3D lut = bakeGradeLut(stats, theme, opts, size);
        writeF32(argv[10], lut.data);
        return 0;
    }

    if (cmd == "recipe" && argc == 11) {
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        Theme theme;
        if (!getTheme(argv[4], theme)) {
            std::fprintf(stderr, "core_parity: unknown theme %s\n", argv[4]);
            return 2;
        }
        EngineOptions opts;
        if (std::atoi(argv[5])) opts.strength = std::atof(argv[6]);
        if (std::atoi(argv[7])) opts.skinProtection = std::atof(argv[8]);
        const int size = std::atoi(argv[9]);
        FootageStats stats = computeStats(px.data(), px.size());
        // Round-trip through the flat POD arb-data recipe, then bake from it.
        RecipeData recipe = recipeFromTheme(theme, stats);
        cg::Lut3D lut = bakeFromRecipe(recipe, opts, size);
        writeF32(argv[10], lut.data);
        return 0;
    }

    if (cmd == "grademid" && argc == 13) {
        // Inject a midtoneTint into the theme, round-trip through the arb recipe,
        // and bake - exercising both the engine's midtone-tint block (PR #24) and
        // the recipe's carry of it (no shipping theme sets midtoneTint).
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        Theme theme;
        if (!getTheme(argv[4], theme)) {
            std::fprintf(stderr, "core_parity: unknown theme %s\n", argv[4]);
            return 2;
        }
        EngineOptions opts;
        if (std::atoi(argv[5])) opts.strength = std::atof(argv[6]);
        if (std::atoi(argv[7])) opts.skinProtection = std::atof(argv[8]);
        const int size = std::atoi(argv[9]);
        cg::core::ThemeOverrides ov = theme.overrides.value_or(cg::core::ThemeOverrides{});
        ov.midtoneTint = std::array<double, 2>{std::atof(argv[10]), std::atof(argv[11])};
        theme.overrides = ov;
        FootageStats stats = computeStats(px.data(), px.size());
        RecipeData recipe = recipeFromTheme(theme, stats);
        cg::Lut3D lut = bakeFromRecipe(recipe, opts, size);
        writeF32(argv[12], lut.data);
        return 0;
    }

    if (cmd == "chromaslider" && argc == 12) {
        // Exercise the effect's Chroma Gain slider path (BakeAutoLut): the slider is
        // a RELATIVE multiplier on the theme's authored chromaGain, not an override.
        // Mirrors BakeAutoLut's `ov.chromaGain = authored * factor` so factor=1.0
        // must reproduce the plain by-name grade bit-exact.
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        Theme theme;
        if (!getTheme(argv[4], theme)) {
            std::fprintf(stderr, "core_parity: unknown theme %s\n", argv[4]);
            return 2;
        }
        EngineOptions opts;
        if (std::atoi(argv[5])) opts.strength = std::atof(argv[6]);
        if (std::atoi(argv[7])) opts.skinProtection = std::atof(argv[8]);
        const int size = std::atoi(argv[9]);
        const double factor = std::atof(argv[10]);
        cg::core::ThemeOverrides ov = theme.overrides.value_or(cg::core::ThemeOverrides{});
        const double authored = ov.chromaGain.value_or(1.0);
        ov.chromaGain = authored * factor;
        theme.overrides = ov;
        FootageStats stats = computeStats(px.data(), px.size());
        cg::Lut3D lut = bakeGradeLut(stats, theme, opts, size);
        writeF32(argv[11], lut.data);
        return 0;
    }

    if (cmd == "gradedecode" && argc == 12) {
        // Exercise the effect's Correct+Grade Auto path (BakeAutoLut, log profile):
        // decode each grid node to Rec.709, then grade, baked into one LUT. Mirrors
        // BakeAutoLut's compose-when-V-Log branch so the Correct decode stage is
        // proven against the TS oracle, not just eyeballed.
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        Theme theme;
        if (!getTheme(argv[4], theme)) {
            std::fprintf(stderr, "core_parity: unknown theme %s\n", argv[4]);
            return 2;
        }
        EngineOptions opts;
        if (std::atoi(argv[5])) opts.strength = std::atof(argv[6]);
        if (std::atoi(argv[7])) opts.skinProtection = std::atof(argv[8]);
        const int size = std::atoi(argv[9]);
        const LogProfile* profile = getProfile(argv[10]);
        if (!profile) { std::fprintf(stderr, "core_parity: unknown profile %s\n", argv[10]); return 2; }
        FootageStats stats = computeStats(px.data(), px.size());
        auto grade = buildTransform(stats, theme, opts);
        cg::Lut3D lut = bakeLut(
            [&grade, profile](const Vec3d& x) { return grade(decodePixelToRec709(x, *profile)); },
            size);
        writeF32(argv[11], lut.data);
        return 0;
    }

    if (cmd == "lutdecode" && argc == 12) {
        // Exercise the effect's Embedded/External Correct path (ComposeDecodeIntoLut):
        // a baked raw LUT resampled through the decode - newLut(x) = rawLut(decode(x)).
        // Uses a baked grade LUT as the stand-in raw LUT (both engines bake it
        // identically), then composes decode via the ported sampleLut.
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        Theme theme;
        if (!getTheme(argv[4], theme)) {
            std::fprintf(stderr, "core_parity: unknown theme %s\n", argv[4]);
            return 2;
        }
        EngineOptions opts;
        if (std::atoi(argv[5])) opts.strength = std::atof(argv[6]);
        if (std::atoi(argv[7])) opts.skinProtection = std::atof(argv[8]);
        const int size = std::atoi(argv[9]);
        const LogProfile* profile = getProfile(argv[10]);
        if (!profile) { std::fprintf(stderr, "core_parity: unknown profile %s\n", argv[10]); return 2; }
        FootageStats stats = computeStats(px.data(), px.size());
        cg::Lut3D raw = bakeGradeLut(stats, theme, opts, size);
        cg::Lut3D lut = bakeLut(
            [&raw, profile](const Vec3d& x) -> Vec3d {
                const Vec3d dec = decodePixelToRec709(x, *profile);
                const cg::Vec3 s = cg::sampleLut(
                    raw, cg::Vec3{static_cast<float>(dec[0]), static_cast<float>(dec[1]),
                                  static_cast<float>(dec[2])});
                return Vec3d{s[0], s[1], s[2]};
            },
            size);
        writeF32(argv[11], lut.data);
        return 0;
    }

    if (cmd == "lutdecodestrength" && argc == 13) {
        // Exercise the effect's Embedded/External V-Log path with Strength baked into
        // the composed LUT (ComposeDecodeIntoLut + strength):
        //   newLut(x) = lerp(decode(x), rawLut(decode(x)), s)
        //             = decode(x)*(1-s) + rawLut(decode(x))*s.
        // Proves the decoded-space blend agrees with the TS oracle at partial Strength.
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        Theme theme;
        if (!getTheme(argv[4], theme)) {
            std::fprintf(stderr, "core_parity: unknown theme %s\n", argv[4]);
            return 2;
        }
        EngineOptions opts;
        if (std::atoi(argv[5])) opts.strength = std::atof(argv[6]);
        if (std::atoi(argv[7])) opts.skinProtection = std::atof(argv[8]);
        const int size = std::atoi(argv[9]);
        const LogProfile* profile = getProfile(argv[10]);
        if (!profile) { std::fprintf(stderr, "core_parity: unknown profile %s\n", argv[10]); return 2; }
        const double s01 = std::atof(argv[11]);
        FootageStats stats = computeStats(px.data(), px.size());
        cg::Lut3D raw = bakeGradeLut(stats, theme, opts, size);
        cg::Lut3D lut = bakeLut(
            [&raw, profile, s01](const Vec3d& x) -> Vec3d {
                const Vec3d dec = decodePixelToRec709(x, *profile);
                const cg::Vec3 s = cg::sampleLut(
                    raw, cg::Vec3{static_cast<float>(dec[0]), static_cast<float>(dec[1]),
                                  static_cast<float>(dec[2])});
                return Vec3d{
                    dec[0] * (1.0 - s01) + s[0] * s01,
                    dec[1] * (1.0 - s01) + s[1] * s01,
                    dec[2] * (1.0 - s01) + s[2] * s01,
                };
            },
            size);
        writeF32(argv[12], lut.data);
        return 0;
    }

    if (cmd == "lutcorrect" && argc == 11) {
        // Exercise the effect's "External .cube + Correct/Basics" mode
        // (ComposeDecodeIntoLut with ManualPrimaries; fm/cg-lut-correct-stack): fold the
        // footage decode AND the manual primaries (Basics + LGG wheels) UNDER the user LUT,
        // with Strength baked as a decoded-space blend:
        //   newLut(x) = lerp(decode(x), rawLut(primaries(decode(x))), s).
        // A baked grade LUT stands in for the user .cube. Mirrors ResolveRenderData's
        // CG_SRC_EXTERNAL_CORRECT path exactly.
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        Theme theme;
        if (!getTheme(argv[4], theme)) {
            std::fprintf(stderr, "core_parity: unknown theme %s\n", argv[4]);
            return 2;
        }
        const int size = std::atoi(argv[5]);
        const LogProfile* profile = getProfile(argv[6]);
        if (!profile) { std::fprintf(stderr, "core_parity: unknown profile %s\n", argv[6]); return 2; }
        ManualGrade m;
        double lookMix = 1.0;
        parseManual(argv[7], m, lookMix);  // lookMix unused: primaries carry no theme look
        LiftGammaGain lg;
        parseLgg(argv[8], lg);
        const double s01 = std::atof(argv[9]);
        FootageStats stats = computeStats(px.data(), px.size());
        cg::Lut3D raw = bakeGradeLut(stats, theme, EngineOptions{}, size);
        const ManualPrimaries primaries = makeManualPrimaries(m, lg);
        cg::Lut3D lut = bakeLut(
            [&raw, profile, &primaries, s01](const Vec3d& x) -> Vec3d {
                const Vec3d dec = decodePixelToRec709(x, *profile);
                const Vec3d corrected = primaries.apply(dec);
                const cg::Vec3 s = cg::sampleLut(
                    raw, cg::Vec3{static_cast<float>(corrected[0]), static_cast<float>(corrected[1]),
                                  static_cast<float>(corrected[2])});
                return Vec3d{
                    dec[0] * (1.0 - s01) + s[0] * s01,
                    dec[1] * (1.0 - s01) + s[1] * s01,
                    dec[2] * (1.0 - s01) + s[2] * s01,
                };
            },
            size);
        writeF32(argv[10], lut.data);
        return 0;
    }

    if (cmd == "grademanual" && argc == 12) {
        // Exercise the Phase 6a manual primary-correction stage + Look Mix directly in
        // buildTransform (bakeGradeLut), against the TS oracle's same manual+lookMix opts.
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        Theme theme;
        if (!getTheme(argv[4], theme)) {
            std::fprintf(stderr, "core_parity: unknown theme %s\n", argv[4]);
            return 2;
        }
        EngineOptions opts;
        if (std::atoi(argv[5])) opts.strength = std::atof(argv[6]);
        if (std::atoi(argv[7])) opts.skinProtection = std::atof(argv[8]);
        const int size = std::atoi(argv[9]);
        ManualGrade m;
        double lookMix = 1.0;
        parseManual(argv[10], m, lookMix);
        opts.manual = m;
        opts.lookMix = lookMix;
        FootageStats stats = computeStats(px.data(), px.size());
        cg::Lut3D lut = bakeGradeLut(stats, theme, opts, size);
        writeF32(argv[11], lut.data);
        return 0;
    }

    if (cmd == "recipemanual" && argc == 12) {
        // Exercise the manual grade carried THROUGH the arb-data recipe: seed a recipe
        // from the theme, stamp the manual block + lookMix into it, and bakeFromRecipe
        // (no manual in opts, so it is taken from the recipe). Must match the plain
        // grademanual/oracle result - proving the POD recipe carries the manual grade.
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        Theme theme;
        if (!getTheme(argv[4], theme)) {
            std::fprintf(stderr, "core_parity: unknown theme %s\n", argv[4]);
            return 2;
        }
        EngineOptions opts;
        if (std::atoi(argv[5])) opts.strength = std::atof(argv[6]);
        if (std::atoi(argv[7])) opts.skinProtection = std::atof(argv[8]);
        const int size = std::atoi(argv[9]);
        ManualGrade m;
        double lookMix = 1.0;
        parseManual(argv[10], m, lookMix);
        FootageStats stats = computeStats(px.data(), px.size());
        RecipeData recipe = recipeFromTheme(theme, stats);
        recipe.manualExposure = m.exposure;
        recipe.manualContrast = m.contrast;
        recipe.manualPivot = m.pivot;
        recipe.manualHighlights = m.highlights;
        recipe.manualShadows = m.shadows;
        recipe.manualWhites = m.whites;
        recipe.manualBlacks = m.blacks;
        recipe.manualTemperature = m.temperature;
        recipe.manualTint = m.tint;
        recipe.manualSaturation = m.saturation;
        recipe.manualVibrance = m.vibrance;
        recipe.lookMix = lookMix;
        cg::Lut3D lut = bakeFromRecipe(recipe, opts, size);
        writeF32(argv[11], lut.data);
        return 0;
    }

    if (cmd == "gradelgg" && argc == 12) {
        // Exercise the Phase 6c Lift/Gamma/Gain wheels directly in buildTransform
        // (bakeGradeLut), against the TS oracle's same lgg opts.
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        Theme theme;
        if (!getTheme(argv[4], theme)) {
            std::fprintf(stderr, "core_parity: unknown theme %s\n", argv[4]);
            return 2;
        }
        EngineOptions opts;
        if (std::atoi(argv[5])) opts.strength = std::atof(argv[6]);
        if (std::atoi(argv[7])) opts.skinProtection = std::atof(argv[8]);
        const int size = std::atoi(argv[9]);
        LiftGammaGain lg;
        parseLgg(argv[10], lg);
        opts.lgg = lg;
        FootageStats stats = computeStats(px.data(), px.size());
        cg::Lut3D lut = bakeGradeLut(stats, theme, opts, size);
        writeF32(argv[11], lut.data);
        return 0;
    }

    if (cmd == "recipelgg" && argc == 12) {
        // Exercise the LGG wheels carried THROUGH the arb-data recipe: seed a recipe from
        // the theme, stamp the lift/gamma/gain triples into it, and bakeFromRecipe (no lgg
        // in opts, so it is taken from the recipe). Must match the plain gradelgg/oracle
        // result - proving the POD recipe carries the wheels.
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        Theme theme;
        if (!getTheme(argv[4], theme)) {
            std::fprintf(stderr, "core_parity: unknown theme %s\n", argv[4]);
            return 2;
        }
        EngineOptions opts;
        if (std::atoi(argv[5])) opts.strength = std::atof(argv[6]);
        if (std::atoi(argv[7])) opts.skinProtection = std::atof(argv[8]);
        const int size = std::atoi(argv[9]);
        LiftGammaGain lg;
        parseLgg(argv[10], lg);
        FootageStats stats = computeStats(px.data(), px.size());
        RecipeData recipe = recipeFromTheme(theme, stats);
        for (int c = 0; c < 3; c++) {
            recipe.lift[c] = lg.lift[c];
            recipe.gamma[c] = lg.gamma[c];
            recipe.gain[c] = lg.gain[c];
        }
        cg::Lut3D lut = bakeFromRecipe(recipe, opts, size);
        writeF32(argv[11], lut.data);
        return 0;
    }

    if (cmd == "recipeeditor" && argc == 9) {
        // Exercise the Phase 6b/6c editor-layer composition through the recipe (the effect's
        // Auto-bake render path uses the SAME applyEditorOverrides helper): seed a recipe from
        // the theme, stamp a user master curve (REPLACES authored) + a user shadow tint (ADDS
        // to authored) into the USER fields, and bakeFromRecipe. Must match the oracle theme
        // built with the composed overrides - the regression guard for "edits reach the LUT".
        const size_t n = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> px = readF32(argv[2], n);
        Theme theme;
        if (!getTheme(argv[4], theme)) {
            std::fprintf(stderr, "core_parity: unknown theme %s\n", argv[4]);
            return 2;
        }
        FootageStats stats = computeStats(px.data(), px.size());
        RecipeData recipe = recipeFromTheme(theme, stats);
        // Parse the user master curve CSV "x0,y0,x1,y1,..." into userToneCurve.
        {
            double vals[2 * RECIPE_MAX_POINTS];
            int cnt = 0;
            const char* p = argv[5];
            while (cnt < 2 * RECIPE_MAX_POINTS && p && *p) {
                vals[cnt++] = std::atof(p);
                const char* c = std::strchr(p, ',');
                p = c ? c + 1 : nullptr;
            }
            const int pts = cnt / 2;
            recipe.userToneCurve.count = pts;
            for (int i = 0; i < pts; i++) {
                recipe.userToneCurve.pts[i][0] = vals[2 * i];
                recipe.userToneCurve.pts[i][1] = vals[2 * i + 1];
            }
        }
        recipe.userShadowTint[0] = std::atof(argv[6]);
        recipe.userShadowTint[1] = std::atof(argv[7]);
        cg::Lut3D lut = bakeFromRecipe(recipe, {}, 33);
        writeF32(argv[8], lut.data);
        return 0;
    }

    if (cmd == "migrate" && argc == 2) {
        // Self-checking versioned arb-data migration test (the Phase 6a landmine): a v2
        // blob migrates forward with its fields intact and the new fields defaulted; a
        // current blob round-trips verbatim; a foreign blob reseeds. Exit nonzero on any
        // failure so the harness (stdio inherit) surfaces it.
        int fail = 0;
        RecipeData fallback = recipeFromTheme(tealOrangeTheme(), tealOrangeTheme().targetStats);
        // Base recipe with distinctive values in the shared (v2) prefix.
        RecipeData base = recipeFromTheme(coolNoirTheme(), coolNoirTheme().targetStats);
        base.chromaGain = 0.777;
        base.hasChromaGain = 1;
        base.strengthDefault = 0.66;
        base.skinProtectionDefault = 0.44;

        // --- v2 blob (truncated to the v2 prefix, version=2) migrates to current. ---
        std::vector<char> bufv2(RECIPE_V2_SIZE);
        std::memcpy(bufv2.data(), &base, RECIPE_V2_SIZE);
        uint32_t two = 2;
        std::memcpy(bufv2.data() + sizeof(uint32_t), &two, sizeof(uint32_t));
        RecipeData out;
        migrateRecipeInto(&out, bufv2.data(), bufv2.size(), fallback);
        if (out.version != RECIPE_VERSION) { std::fprintf(stderr, "migrate: v2 version\n"); fail = 1; }
        if (out.magic != RECIPE_MAGIC) { std::fprintf(stderr, "migrate: v2 magic\n"); fail = 1; }
        if (out.chromaGain != 0.777) { std::fprintf(stderr, "migrate: v2 chromaGain\n"); fail = 1; }
        if (out.strengthDefault != 0.66) { std::fprintf(stderr, "migrate: v2 strength\n"); fail = 1; }
        if (out.skinProtectionDefault != 0.44) { std::fprintf(stderr, "migrate: v2 skin\n"); fail = 1; }
        if (out.matchStats != 1) { std::fprintf(stderr, "migrate: v2 matchStats default\n"); fail = 1; }
        if (out.manualSaturation != 1.0) { std::fprintf(stderr, "migrate: v2 sat default\n"); fail = 1; }
        if (out.manualExposure != 0.0) { std::fprintf(stderr, "migrate: v2 exp default\n"); fail = 1; }
        if (out.manualPivot != 0.435) { std::fprintf(stderr, "migrate: v2 pivot default\n"); fail = 1; }
        if (out.lookMix != 1.0) { std::fprintf(stderr, "migrate: v2 lookMix default\n"); fail = 1; }
        // v4 (Phase 6b/6c) fields default to neutral LGG + empty user curves/tints on a v2 migration.
        if (out.lift[0] != 0.0 || out.gamma[0] != 1.0 || out.gain[0] != 1.0) {
            std::fprintf(stderr, "migrate: v2 lgg default\n"); fail = 1;
        }
        if (out.userToneCurve.count != 0 || out.userShadowTint[0] != 0.0) {
            std::fprintf(stderr, "migrate: v2 user-editor default\n"); fail = 1;
        }
        if (out.wheelsMode != 0) { std::fprintf(stderr, "migrate: v2 wheelsMode default\n"); fail = 1; }

        // --- v3 blob (truncated to the v3 prefix, version=3) migrates to current: the
        //     v3 manual block survives, only the v4 LGG fields default. ---
        RecipeData v3src = base;
        v3src.manualExposure = 1.2;
        v3src.manualSaturation = 1.3;
        v3src.lookMix = 0.5;
        v3src.matchStats = 0;
        std::vector<char> bufv3(RECIPE_V3_SIZE);
        std::memcpy(bufv3.data(), &v3src, RECIPE_V3_SIZE);
        uint32_t three = 3;
        std::memcpy(bufv3.data() + sizeof(uint32_t), &three, sizeof(uint32_t));
        RecipeData outv3;
        migrateRecipeInto(&outv3, bufv3.data(), bufv3.size(), fallback);
        if (outv3.version != RECIPE_VERSION) { std::fprintf(stderr, "migrate: v3 version\n"); fail = 1; }
        if (outv3.chromaGain != 0.777) { std::fprintf(stderr, "migrate: v3 chromaGain\n"); fail = 1; }
        if (outv3.manualExposure != 1.2) { std::fprintf(stderr, "migrate: v3 exposure\n"); fail = 1; }
        if (outv3.manualSaturation != 1.3) { std::fprintf(stderr, "migrate: v3 sat\n"); fail = 1; }
        if (outv3.lookMix != 0.5) { std::fprintf(stderr, "migrate: v3 lookMix\n"); fail = 1; }
        if (outv3.matchStats != 0) { std::fprintf(stderr, "migrate: v3 matchStats\n"); fail = 1; }
        if (outv3.lift[0] != 0.0 || outv3.gamma[1] != 1.0 || outv3.gain[2] != 1.0) {
            std::fprintf(stderr, "migrate: v3 lgg default\n"); fail = 1;
        }
        if (outv3.userToneCurve.count != 0 || outv3.userShadowTint[0] != 0.0) {
            std::fprintf(stderr, "migrate: v3 user-editor default\n"); fail = 1;
        }
        if (outv3.wheelsMode != 0) { std::fprintf(stderr, "migrate: v3 wheelsMode default\n"); fail = 1; }

        // --- Current-version blob copies verbatim (incl. non-neutral LGG + user fields). ---
        RecipeData current = base;
        current.version = RECIPE_VERSION;
        current.lift[0] = 0.05;
        current.gamma[1] = 1.2;
        current.gain[2] = 0.9;
        current.userToneCurve.count = 2;
        current.userToneCurve.pts[0][0] = 0.1;
        current.userShadowTint[0] = 7.0;
        current.wheelsMode = 1;
        RecipeData out2;
        migrateRecipeInto(&out2, &current, sizeof(RecipeData), fallback);
        if (std::memcmp(&out2, &current, sizeof(RecipeData)) != 0) {
            std::fprintf(stderr, "migrate: verbatim\n");
            fail = 1;
        }
        // Foreign blob reseeds to the fallback.
        RecipeData out3;
        uint32_t junk = 0xDEADBEEF;
        migrateRecipeInto(&out3, &junk, sizeof(junk), fallback);
        if (std::memcmp(&out3, &fallback, sizeof(RecipeData)) != 0) {
            std::fprintf(stderr, "migrate: reseed\n");
            fail = 1;
        }
        std::printf(fail ? "  migrate  FAIL\n"
                         : "  migrate  PASS (v2->v4, v3->v4 forward, verbatim, reseed)\n");
        return fail;
    }

    if (cmd == "referencematch" && argc == 12) {
        // Phase 7 "match this look" (data/cg-agents-study/report.md sec 1d): a Theme
        // built from ONE frame's stats (referenceMatchTheme), applied to a DIFFERENT
        // frame - proves the C++ port matches the TS oracle (themeFromReferenceStats)
        // bit-exact, cross-content, not just self-identity.
        const size_t nSrc = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
        std::vector<float> srcPx = readF32(argv[2], nSrc);
        const size_t nRef = static_cast<size_t>(std::strtoull(argv[5], nullptr, 10));
        std::vector<float> refPx = readF32(argv[4], nRef);
        EngineOptions opts;
        if (std::atoi(argv[6])) opts.strength = std::atof(argv[7]);
        if (std::atoi(argv[8])) opts.skinProtection = std::atof(argv[9]);
        const int size = std::atoi(argv[10]);
        FootageStats srcStats = computeStats(srcPx.data(), srcPx.size());
        FootageStats refStats = computeStats(refPx.data(), refPx.size());
        Theme theme = referenceMatchTheme(refStats);
        cg::Lut3D lut = bakeGradeLut(srcStats, theme, opts, size);
        writeF32(argv[11], lut.data);
        return 0;
    }

    if (cmd == "referencestatsformat" && argc == 2) {
        // Self-checking round-trip test for the reference-stats sidecar text format (the
        // native editor's minimal entry point, ColorGrade.cpp's LoadReferenceStats): write
        // -> parse recovers the same values; malformed input is rejected; comma-separated
        // input (matching the TS tokenizer) parses too. Exit nonzero on any failure.
        int fail = 0;
        StatsData d{};
        for (int i = 0; i < STATS_FIELDS; i++) d.v[i] = (i + 1) * 0.123456789 - 3.0;
        const std::string text = formatReferenceStatsText(d);
        StatsData parsed{};
        if (!parseReferenceStatsText(text, parsed)) {
            std::fprintf(stderr, "referencestatsformat: parse failed\n");
            fail = 1;
        }
        for (int i = 0; i < STATS_FIELDS; i++) {
            if (std::fabs(parsed.v[i] - d.v[i]) > 1e-9) {
                std::fprintf(stderr, "referencestatsformat: mismatch at field %d\n", i);
                fail = 1;
            }
        }
        StatsData bogus{};
        if (parseReferenceStatsText("1 2 3", bogus)) {
            std::fprintf(stderr, "referencestatsformat: accepted malformed (too few tokens)\n");
            fail = 1;
        }
        std::string tooMany = text + "1.0\n";
        if (parseReferenceStatsText(tooMany, bogus)) {
            std::fprintf(stderr, "referencestatsformat: accepted malformed (too many tokens)\n");
            fail = 1;
        }
        std::string csv;
        for (int i = 0; i < STATS_FIELDS; i++) csv += "0.5, ";
        StatsData csvParsed{};
        if (!parseReferenceStatsText(csv, csvParsed) || csvParsed.v[0] != 0.5) {
            std::fprintf(stderr, "referencestatsformat: comma-separated parse failed\n");
            fail = 1;
        }
        std::printf(fail ? "  referencestatsformat  FAIL\n"
                         : "  referencestatsformat  PASS (round-trip, malformed rejected, csv)\n");
        return fail;
    }

    if (cmd == "decode" && argc == 5) {
        const LogProfile* profile = getProfile(argv[2]);
        if (!profile) { std::fprintf(stderr, "core_parity: unknown profile %s\n", argv[2]); return 2; }
        const int size = std::atoi(argv[3]);
        cg::Lut3D lut = bakeDecodeLut(*profile, size);
        writeF32(argv[4], lut.data);
        return 0;
    }

    std::fprintf(stderr, "core_parity: bad args for '%s'\n", cmd.c_str());
    return 2;
}
