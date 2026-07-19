/*
 * ArriLogC.h - C++ port of src/core/color/arrilogc.ts (bit-exact oracle).
 * ARRI LogC3 (EI 800) / AWG3 and LogC4 / AWG4. LogC3 uses ARRI's published
 * EI 800 reflectance constants; LogC4 uses the published LogC4 spec constants.
 * decode: normalized code value -> reflectance; encode inverse.
 */
#pragma once
#ifndef CG_CORE_ARRILOGC_H
#define CG_CORE_ARRILOGC_H

#include <cmath>

#include "Mat3.h"

namespace cg {
namespace core {

namespace arri_detail {
// LogC3 EI 800 (reflectance form).
constexpr double C3_CUT = 0.010591;
constexpr double C3_A = 5.555556;
constexpr double C3_B = 0.052272;
constexpr double C3_C = 0.24719;
constexpr double C3_D = 0.385537;
constexpr double C3_E = 5.367655;
constexpr double C3_F = 0.092809;
constexpr double C3_ENCODED_CUT = C3_E * C3_CUT + C3_F;

// LogC4.
const double C4_A = (std::pow(2.0, 18.0) - 16.0) / 117.45;
const double C4_B = (1023.0 - 95.0) / 1023.0;
const double C4_C = 95.0 / 1023.0;
const double C4_LN2 = 0.6931471805599453;  // Math.LN2
const double C4_S = (7.0 * C4_LN2 * std::pow(2.0, 7.0 - (14.0 * C4_C) / C4_B)) / (C4_A * C4_B);
const double C4_T = (std::pow(2.0, (14.0 * -C4_C) / C4_B + 6.0) - 64.0) / C4_A;
}  // namespace arri_detail

inline double logC3Encode(double linear) {
    using namespace arri_detail;
    return linear > C3_CUT ? C3_C * std::log10(C3_A * linear + C3_B) + C3_D : C3_E * linear + C3_F;
}

inline double logC3Decode(double encoded) {
    using namespace arri_detail;
    return encoded > C3_ENCODED_CUT ? (std::pow(10.0, (encoded - C3_D) / C3_C) - C3_B) / C3_A
                                    : (encoded - C3_F) / C3_E;
}

inline double logC4Encode(double linear) {
    using namespace arri_detail;
    return linear >= C4_T ? ((std::log2(C4_A * linear + 64.0) - 6.0) / 14.0) * C4_B + C4_C
                          : (linear - C4_T) / C4_S;
}

inline double logC4Decode(double encoded) {
    using namespace arri_detail;
    return encoded >= 0.0 ? (std::pow(2.0, (14.0 * (encoded - C4_C)) / C4_B + 6.0) - 64.0) / C4_A
                          : encoded * C4_S + C4_T;
}

// ARRI Wide Gamut 3 primaries, D65 white.
inline const Chromaticities& AWG3_CHROMATICITIES() {
    static const Chromaticities c = {{0.684, 0.313}, {0.221, 0.848}, {0.0861, -0.102}, D65()};
    return c;
}

inline const Mat3& AWG3_GAMUT_TO_REC709() {
    static const Mat3 m = gamutToRec709Matrix(AWG3_CHROMATICITIES());
    return m;
}

// ARRI Wide Gamut 4 primaries, D65 white.
inline const Chromaticities& AWG4_CHROMATICITIES() {
    static const Chromaticities c = {{0.7347, 0.2653}, {0.1424, 0.8576}, {0.0991, -0.0308}, D65()};
    return c;
}

inline const Mat3& AWG4_GAMUT_TO_REC709() {
    static const Mat3 m = gamutToRec709Matrix(AWG4_CHROMATICITIES());
    return m;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_ARRILOGC_H
