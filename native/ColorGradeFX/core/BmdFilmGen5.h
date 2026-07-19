/*
 * BmdFilmGen5.h - C++ port of src/core/color/filmgen5.ts (bit-exact oracle).
 * Blackmagic Film Generation 5 / Blackmagic Wide Gamut (Gen 5). Constants
 * verbatim from Blackmagic's "Generation 5 Color Science" white paper. The log
 * region uses natural log.
 */
#pragma once
#ifndef CG_CORE_BMDFILMGEN5_H
#define CG_CORE_BMDFILMGEN5_H

#include <cmath>

#include "Mat3.h"

namespace cg {
namespace core {

namespace filmgen5_detail {
constexpr double A = 0.08692876065491224;
constexpr double B = 0.005494072432257808;
constexpr double C = 0.5300133392291939;
constexpr double D = 8.283605932402494;
constexpr double E = 0.09246575342465753;
constexpr double LIN_CUT = 0.005;
constexpr double ENCODED_CUT = D * LIN_CUT + E;
}  // namespace filmgen5_detail

inline double filmGen5Encode(double linear) {
    using namespace filmgen5_detail;
    return linear < LIN_CUT ? D * linear + E : A * std::log(linear + B) + C;
}

inline double filmGen5Decode(double encoded) {
    using namespace filmgen5_detail;
    return encoded < ENCODED_CUT ? (encoded - E) / D : std::exp((encoded - C) / A) - B;
}

// Blackmagic Wide Gamut (Gen 5) primaries, D65 white.
inline const Chromaticities& BMD_WIDE_GAMUT_GEN5_CHROMATICITIES() {
    static const Chromaticities c = {{0.7177215, 0.3171181}, {0.228041, 0.861569}, {0.1005841, -0.0820452}, D65()};
    return c;
}

inline const Mat3& FILM_GEN5_GAMUT_TO_REC709() {
    static const Mat3 m = gamutToRec709Matrix(BMD_WIDE_GAMUT_GEN5_CHROMATICITIES());
    return m;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_BMDFILMGEN5_H
