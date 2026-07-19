/*
 * CanonLog.h - C++ port of src/core/color/canonlog.ts (bit-exact oracle).
 * Canon C-Log2 / C-Log3 / Cinema Gamut. Constants verbatim from Canon's
 * published "Canon Log Gamma Curves" white paper (v1.2 normalized code value).
 * Canon's log operates on reflectance/0.9, so decode scales by 0.9 to return
 * reflectance; encode divides by 0.9 first.
 */
#pragma once
#ifndef CG_CORE_CANONLOG_H
#define CG_CORE_CANONLOG_H

#include <cmath>

#include "Mat3.h"

namespace cg {
namespace core {

inline double canonLog2Decode(double encoded) {
    const double raw =
        encoded < 0.092864125
            ? -(std::pow(10.0, (0.092864125 - encoded) / 0.24136077) - 1.0) / 87.09937546
            : (std::pow(10.0, (encoded - 0.092864125) / 0.24136077) - 1.0) / 87.09937546;
    return raw * 0.9;
}

inline double canonLog2Encode(double linear) {
    const double u = linear / 0.9;
    return u < 0.0 ? 0.092864125 - 0.24136077 * std::log10(1.0 - u * 87.09937546)
                   : 0.092864125 + 0.24136077 * std::log10(1.0 + u * 87.09937546);
}

inline double canonLog3Decode(double encoded) {
    double raw;
    if (encoded < 0.097465473) {
        raw = -(std::pow(10.0, (0.12783901 - encoded) / 0.36726845) - 1.0) / 14.98325;
    } else if (encoded <= 0.15277891) {
        raw = (encoded - 0.12512219) / 1.9754798;
    } else {
        raw = (std::pow(10.0, (encoded - 0.12240537) / 0.36726845) - 1.0) / 14.98325;
    }
    return raw * 0.9;
}

inline double canonLog3Encode(double linear) {
    const double u = linear / 0.9;
    if (u < -0.014) return 0.12783901 - 0.36726845 * std::log10(1.0 - u * 14.98325);
    if (u <= 0.014) return 1.9754798 * u + 0.12512219;
    return 0.12240537 + 0.36726845 * std::log10(1.0 + u * 14.98325);
}

// Canon Cinema Gamut primaries, D65 white.
inline const Chromaticities& CINEMA_GAMUT_CHROMATICITIES() {
    static const Chromaticities c = {{0.74, 0.27}, {0.17, 1.14}, {0.08, -0.1}, D65()};
    return c;
}

inline const Mat3& CINEMA_GAMUT_TO_REC709() {
    static const Mat3 m = gamutToRec709Matrix(CINEMA_GAMUT_CHROMATICITIES());
    return m;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_CANONLOG_H
