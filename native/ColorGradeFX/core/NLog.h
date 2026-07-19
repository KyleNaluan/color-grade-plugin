/*
 * NLog.h - C++ port of src/core/color/nlog.ts (bit-exact oracle).
 * Nikon N-Log / N-Gamut. Constants verbatim from Nikon's published "N-Log
 * Specification Document" (10-bit code values normalized to [0,1]). A cube root
 * below mid-tones, a natural log above. N-Gamut = ITU-R BT.2020.
 */
#pragma once
#ifndef CG_CORE_NLOG_H
#define CG_CORE_NLOG_H

#include <cmath>

#include "Mat3.h"

namespace cg {
namespace core {

namespace nlog_detail {
constexpr double A = 650.0 / 1023.0;
constexpr double C = 150.0 / 1023.0;
constexpr double D = 619.0 / 1023.0;
constexpr double LIN_CUT = 0.328;
constexpr double ENCODED_CUT = 452.0 / 1023.0;
}  // namespace nlog_detail

inline double nLogEncode(double linear) {
    using namespace nlog_detail;
    return linear < LIN_CUT ? A * std::cbrt(linear + 0.0075) : C * std::log(linear) + D;
}

inline double nLogDecode(double encoded) {
    using namespace nlog_detail;
    return encoded < ENCODED_CUT ? std::pow(encoded / A, 3.0) - 0.0075 : std::exp((encoded - D) / C);
}

// N-Gamut = ITU-R BT.2020.
inline const Mat3& NLOG_GAMUT_TO_REC709() {
    static const Mat3 m = gamutToRec709Matrix(BT2020_CHROMATICITIES());
    return m;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_NLOG_H
