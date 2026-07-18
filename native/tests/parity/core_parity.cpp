/*
 * core_parity.cpp - C++ side of the Phase 2 cross-engine golden harness.
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
 * Buffers are raw little-endian arrays (matching JS Float32Array / Float64Array)
 * on the shared x86-64 host; not a portable serialization, just a test channel.
 */
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
