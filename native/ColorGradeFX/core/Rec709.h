/*
 * Rec709.h - C++ port of src/core/color/rec709.ts.
 * BT.709 OETF and its inverse (scene-linear <-> gamma-encoded). Math in double.
 */
#pragma once
#ifndef CG_CORE_REC709_H
#define CG_CORE_REC709_H

#include <cmath>

namespace cg {
namespace core {

// BT.709 OETF (scene linear -> encoded).
inline double rec709Encode(double linear) {
    return linear < 0.018 ? 4.5 * linear : 1.099 * std::pow(linear, 0.45) - 0.099;
}

// Inverse BT.709 OETF (encoded -> scene linear).
inline double rec709Decode(double encoded) {
    return encoded < 0.081 ? encoded / 4.5 : std::pow((encoded + 0.099) / 1.099, 1.0 / 0.45);
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_REC709_H
