/*
 * DjiDlog.h - C++ port of src/core/color/dlog.ts (bit-exact oracle).
 * DJI D-Log / D-Gamut. Constants verbatim from DJI's published D-Log/D-Gamut
 * white paper; decode is the exact algebraic inverse of the published encode.
 */
#pragma once
#ifndef CG_CORE_DJIDLOG_H
#define CG_CORE_DJIDLOG_H

#include <cmath>

#include "Mat3.h"

namespace cg {
namespace core {

namespace dlog_detail {
constexpr double LIN_CUT = 0.0078;
constexpr double ENCODED_CUT = 6.025 * LIN_CUT + 0.0929;
}  // namespace dlog_detail

inline double dLogEncode(double linear) {
    using namespace dlog_detail;
    return linear <= LIN_CUT ? 6.025 * linear + 0.0929
                             : std::log10(linear * 0.9892 + 0.0108) * 0.256663 + 0.584555;
}

inline double dLogDecode(double encoded) {
    using namespace dlog_detail;
    return encoded < ENCODED_CUT
               ? (encoded - 0.0929) / 6.025
               : (std::pow(10.0, (encoded - 0.584555) / 0.256663) - 0.0108) / 0.9892;
}

// DJI D-Gamut primaries, D65 white.
inline const Chromaticities& D_GAMUT_CHROMATICITIES() {
    static const Chromaticities c = {{0.71, 0.31}, {0.21, 0.88}, {0.09, -0.08}, D65()};
    return c;
}

inline const Mat3& DLOG_GAMUT_TO_REC709() {
    static const Mat3 m = gamutToRec709Matrix(D_GAMUT_CHROMATICITIES());
    return m;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_DJIDLOG_H
