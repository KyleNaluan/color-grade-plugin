/*
 * FLog.h - C++ port of src/core/color/flog.ts (bit-exact oracle).
 * Fujifilm F-Log / F-Log2 / F-Gamut. Constants verbatim from Fujifilm's
 * published F-Log and F-Log2 data sheets. F-Gamut = ITU-R BT.2020.
 */
#pragma once
#ifndef CG_CORE_FLOG_H
#define CG_CORE_FLOG_H

#include <cmath>

#include "Mat3.h"

namespace cg {
namespace core {

struct FLogConstants {
    double cut1, cut2, a, b, c, d, e, f;
};

namespace flog_detail {
constexpr FLogConstants F_LOG = {0.00089, 0.100537775223865, 0.555556, 0.009468, 0.344676, 0.790453, 8.735631, 0.092864};
constexpr FLogConstants F_LOG2 = {0.000889, 0.100686685370811, 5.555556, 0.064829, 0.245281, 0.384316, 8.799461, 0.092864};

inline double encodeWith(const FLogConstants& k, double linear) {
    return linear < k.cut1 ? k.e * linear + k.f : k.c * std::log10(k.a * linear + k.b) + k.d;
}
inline double decodeWith(const FLogConstants& k, double encoded) {
    return encoded < k.cut2 ? (encoded - k.f) / k.e : std::pow(10.0, (encoded - k.d) / k.c) / k.a - k.b / k.a;
}
}  // namespace flog_detail

inline double flogEncode(double linear) { return flog_detail::encodeWith(flog_detail::F_LOG, linear); }
inline double flogDecode(double encoded) { return flog_detail::decodeWith(flog_detail::F_LOG, encoded); }
inline double flog2Encode(double linear) { return flog_detail::encodeWith(flog_detail::F_LOG2, linear); }
inline double flog2Decode(double encoded) { return flog_detail::decodeWith(flog_detail::F_LOG2, encoded); }

// F-Gamut = ITU-R BT.2020.
inline const Mat3& FLOG_GAMUT_TO_REC709() {
    static const Mat3 m = gamutToRec709Matrix(BT2020_CHROMATICITIES());
    return m;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_FLOG_H
