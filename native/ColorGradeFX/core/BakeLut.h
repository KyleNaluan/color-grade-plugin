/*
 * BakeLut.h - C++ port of src/core/lut/cube.ts bakeLut plus the two bake entry
 * points (gradeLut.ts bakeGradeLut, decodeLut.ts bakeDecodeLut).
 *
 * bakeLut samples a transform over a size^3 grid and stores the result as
 * float32 into a cg::Lut3D (the same struct the ported sampleLut/parseCube in
 * ../lut/CubeLut.h use). Storing float32 mirrors the TS Float32Array grid, so
 * the grade/decode grids are the natural cross-engine golden vectors.
 *
 * Grid defaults match the TS: grade LUTs 33 points, decode LUTs 65 points.
 */
#pragma once
#ifndef CG_CORE_BAKELUT_H
#define CG_CORE_BAKELUT_H

#include <string>

#include "../lut/CubeLut.h"
#include "Decode.h"
#include "Engine.h"
#include "LogProfile.h"
#include "Mat3.h"
#include "Stats.h"
#include "Theme.h"

namespace cg {
namespace core {

// Bake a transform (encoded Rec.709 -> encoded Rec.709) into a 3D LUT grid.
// `transform` is any callable Vec3d(const Vec3d&). Output narrowed to float32.
template <typename Transform>
inline cg::Lut3D bakeLut(const Transform& transform, int size = 33, const std::string& title = "") {
    cg::Lut3D lut;
    lut.size = size;
    lut.title = title;
    lut.data.resize(static_cast<size_t>(size) * size * size * 3);
    const double denom = static_cast<double>(size - 1);
    size_t i = 0;
    for (int b = 0; b < size; b++) {
        for (int g = 0; g < size; g++) {
            for (int r = 0; r < size; r++) {
                const Vec3d out = transform(Vec3d{r / denom, g / denom, b / denom});
                lut.data[i++] = static_cast<float>(out[0]);
                lut.data[i++] = static_cast<float>(out[1]);
                lut.data[i++] = static_cast<float>(out[2]);
            }
        }
    }
    return lut;
}

// Bake a grade LUT from measured stats + a Theme (default 33 points).
inline cg::Lut3D bakeGradeLut(const FootageStats& stats, const Theme& theme,
                              const EngineOptions& opts = {}, int size = 33) {
    return bakeLut(buildTransform(stats, theme, opts), size, theme.name + " grade");
}

// Bake a Decode LUT from a log profile's decode (default 65 points).
inline cg::Lut3D bakeDecodeLut(const LogProfile& profile, int size = 65) {
    return bakeLut([&profile](const Vec3d& rgb) { return decodePixelToRec709(rgb, profile); }, size,
                   profile.name + " Decode");
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_BAKELUT_H
