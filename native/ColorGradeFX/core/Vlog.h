/*
 * Vlog.h - C++ port of src/core/color/vlog.ts.
 * Panasonic V-Log/V-Gamut encode/decode + the V-Gamut->Rec.709 gamut matrix.
 * Constants verbatim from the published V-Log/V-Gamut Reference Manual.
 */
#pragma once
#ifndef CG_CORE_VLOG_H
#define CG_CORE_VLOG_H

#include <cmath>

#include "Mat3.h"

namespace cg {
namespace core {

namespace vlog_detail {
constexpr double CUT1 = 0.01;    // linear-domain breakpoint
constexpr double CUT2 = 0.181;   // encoded-domain breakpoint (= encode(CUT1))
constexpr double B = 0.00873;
constexpr double C = 0.241514;
constexpr double D = 0.598206;
}  // namespace vlog_detail

inline double vlogEncode(double linear) {
    using namespace vlog_detail;
    return linear < CUT1 ? 5.6 * linear + 0.125 : C * std::log10(linear + B) + D;
}

inline double vlogDecode(double encoded) {
    using namespace vlog_detail;
    return encoded < CUT2 ? (encoded - 0.125) / 5.6 : std::pow(10.0, (encoded - D) / C) - B;
}

// V-Gamut primaries from the same manual, D65 white.
inline const Chromaticities& VGAMUT_CHROMATICITIES() {
    static const Chromaticities c = {{0.73, 0.28}, {0.165, 0.84}, {0.1, -0.03}, D65()};
    return c;
}

// V-Gamut (linear) -> Rec.709 (linear): inv(REC709->XYZ) * (VGAMUT->XYZ).
inline const Mat3& VLOG_GAMUT_TO_REC709() {
    static const Mat3 m = mat3Mul(mat3Inv(REC709_TO_XYZ()), rgbToXyzMatrix(VGAMUT_CHROMATICITIES()));
    return m;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_VLOG_H
