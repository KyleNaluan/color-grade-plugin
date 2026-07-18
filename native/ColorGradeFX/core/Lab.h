/*
 * Lab.h - C++ port of src/core/color/lab.ts.
 * CIELAB (D65) <-> linear Rec.709. The reference white is derived from the same
 * Rec.709 matrix used for RGB->XYZ so RGB white maps exactly to L=100, a=b=0.
 */
#pragma once
#ifndef CG_CORE_LAB_H
#define CG_CORE_LAB_H

#include <cmath>

#include "Mat3.h"

namespace cg {
namespace core {

namespace lab_detail {
constexpr double EPS = 216.0 / 24389.0;
constexpr double KAPPA = 24389.0 / 27.0;

inline double fwd(double t) {
    return t > EPS ? std::cbrt(t) : (KAPPA * t + 16.0) / 116.0;
}

inline double inv(double f) {
    const double f3 = f * f * f;
    return f3 > EPS ? f3 : (116.0 * f - 16.0) / KAPPA;
}

// D65 reference white = REC709_TO_XYZ * [1,1,1]. Computed once.
inline const Vec3d& white() {
    static const Vec3d w = mat3MulVec(REC709_TO_XYZ(), {1.0, 1.0, 1.0});
    return w;
}
}  // namespace lab_detail

inline Vec3d xyzToLab(const Vec3d& xyz) {
    const Vec3d& n = lab_detail::white();
    const double fx = lab_detail::fwd(xyz[0] / n[0]);
    const double fy = lab_detail::fwd(xyz[1] / n[1]);
    const double fz = lab_detail::fwd(xyz[2] / n[2]);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

inline Vec3d labToXyz(const Vec3d& lab) {
    const Vec3d& n = lab_detail::white();
    const double fy = (lab[0] + 16.0) / 116.0;
    const double fx = fy + lab[1] / 500.0;
    const double fz = fy - lab[2] / 200.0;
    return {lab_detail::inv(fx) * n[0], lab_detail::inv(fy) * n[1], lab_detail::inv(fz) * n[2]};
}

// Linear Rec.709 RGB -> CIELAB (D65).
inline Vec3d linearRec709ToLab(const Vec3d& rgb) {
    return xyzToLab(mat3MulVec(REC709_TO_XYZ(), rgb));
}

// CIELAB (D65) -> linear Rec.709 RGB.
inline Vec3d labToLinearRec709(const Vec3d& lab) {
    return mat3MulVec(XYZ_TO_REC709(), labToXyz(lab));
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_LAB_H
