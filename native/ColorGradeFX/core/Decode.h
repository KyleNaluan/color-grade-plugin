/*
 * Decode.h - C++ port of src/core/color/decode.ts.
 * The single decode-to-Rec.709 composition (decode -> gamut -> Rec.709 encode),
 * shared by stats (bulk footage decode) and the Decode LUT bake. Rec.709 input
 * is returned unchanged, matching the TS name-based short-circuit.
 */
#pragma once
#ifndef CG_CORE_DECODE_H
#define CG_CORE_DECODE_H

#include <algorithm>

#include "LogProfile.h"
#include "Mat3.h"
#include "Rec709.h"

namespace cg {
namespace core {

inline double clamp01(double x) {
    return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
}

inline Vec3d decodePixelToRec709(const Vec3d& rgb, const LogProfile& profile) {
    if (profile.name == "Rec.709") return rgb;
    const Vec3d lin = mat3MulVec(profile.gamutToRec709, {
        profile.decode(rgb[0]),
        profile.decode(rgb[1]),
        profile.decode(rgb[2]),
    });
    return {
        clamp01(rec709Encode(std::max(0.0, lin[0]))),
        clamp01(rec709Encode(std::max(0.0, lin[1]))),
        clamp01(rec709Encode(std::max(0.0, lin[2]))),
    };
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_DECODE_H
