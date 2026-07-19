/*
 * Slog3.h - C++ port of src/core/color/slog3.ts (bit-exact oracle).
 * Sony S-Log3 / S-Gamut3.Cine. Constants verbatim from Sony's published S-Log3
 * transfer function. decode: normalized code value -> reflectance; encode inverse.
 */
#pragma once
#ifndef CG_CORE_SLOG3_H
#define CG_CORE_SLOG3_H

#include <cmath>

#include "Mat3.h"

namespace cg {
namespace core {

namespace slog3_detail {
constexpr double BREAK_ENCODED = 171.2102946929 / 1023.0;
}  // namespace slog3_detail

inline double slog3Encode(double linear) {
    return linear >= 0.01125
               ? (420.0 + std::log10((linear + 0.01) / (0.18 + 0.01)) * 261.5) / 1023.0
               : (linear * (171.2102946929 - 95.0) / 0.01125 + 95.0) / 1023.0;
}

inline double slog3Decode(double encoded) {
    using namespace slog3_detail;
    return encoded >= BREAK_ENCODED
               ? std::pow(10.0, (encoded * 1023.0 - 420.0) / 261.5) * (0.18 + 0.01) - 0.01
               : (encoded * 1023.0 - 95.0) * 0.01125 / (171.2102946929 - 95.0);
}

// S-Gamut3.Cine primaries, D65 white.
inline const Chromaticities& SGAMUT3_CINE_CHROMATICITIES() {
    static const Chromaticities c = {{0.766, 0.275}, {0.225, 0.8}, {0.089, -0.087}, D65()};
    return c;
}

inline const Mat3& SLOG3_GAMUT_TO_REC709() {
    static const Mat3 m = gamutToRec709Matrix(SGAMUT3_CINE_CHROMATICITIES());
    return m;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_SLOG3_H
