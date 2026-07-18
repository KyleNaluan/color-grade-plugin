/*
 * Mat3.h - C++ port of src/core/color/matrices.ts (the reference oracle).
 *
 * Part of the Phase 2 core port. All engine math is done in `double` to mirror
 * JavaScript's single `number` type; the port only narrows to float at the two
 * storage boundaries the TS narrows at (Lut3D float grid, computeStats' luma
 * Float32Array). See native/scripts/core-parity-test.ts for the parity gate.
 *
 * Dependency-free (only <array>, <cmath>, <stdexcept>) so the identical header
 * compiles into the AE plugin (MSVC) and the WSL parity harness (g++/clang).
 *
 * Derived matrices (REC709_TO_XYZ, XYZ_TO_REC709) are exposed via accessor
 * functions with function-local statics, not inline variables: the TS builds
 * them at module load from mat3Inv/rgbToXyzMatrix, and Meyers-singleton
 * accessors give the same "computed once, before first use" semantics without
 * depending on cross-TU dynamic-init ordering.
 */
#pragma once
#ifndef CG_CORE_MAT3_H
#define CG_CORE_MAT3_H

#include <array>
#include <cmath>
#include <stdexcept>

namespace cg {
namespace core {

using Vec3d = std::array<double, 3>;
using Mat3 = std::array<Vec3d, 3>;

// JS Math.PI, spelled out so the port never depends on <cmath>'s non-standard M_PI.
constexpr double PI = 3.141592653589793;

inline Vec3d mat3MulVec(const Mat3& m, const Vec3d& v) {
    return {
        m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
        m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
        m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2],
    };
}

inline Mat3 mat3Mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    return r;
}

inline Mat3 mat3Inv(const Mat3& m) {
    const double a = m[0][0], b = m[0][1], c = m[0][2];
    const double d = m[1][0], e = m[1][1], f = m[1][2];
    const double g = m[2][0], h = m[2][1], i = m[2][2];
    const double A = e * i - f * h;
    const double B = -(d * i - f * g);
    const double C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    if (std::abs(det) < 1e-12) throw std::runtime_error("singular matrix");
    const double s = 1.0 / det;
    return Mat3{{
        {A * s, -(b * i - c * h) * s, (b * f - c * e) * s},
        {B * s, (a * i - c * g) * s, -(a * f - c * d) * s},
        {C * s, -(a * h - b * g) * s, (a * e - b * d) * s},
    }};
}

struct Chromaticities {
    std::array<double, 2> r;
    std::array<double, 2> g;
    std::array<double, 2> b;
    std::array<double, 2> white;
};

// RGB (linear) -> XYZ matrix from primaries + white point. Mirrors rgbToXyzMatrix.
inline Mat3 rgbToXyzMatrix(const Chromaticities& c) {
    auto xyzOf = [](const std::array<double, 2>& p) -> Vec3d {
        const double x = p[0], y = p[1];
        return {x / y, 1.0, (1.0 - x - y) / y};
    };
    const Vec3d xr = xyzOf(c.r), xg = xyzOf(c.g), xb = xyzOf(c.b);
    const Mat3 P{{
        {xr[0], xg[0], xb[0]},
        {xr[1], xg[1], xb[1]},
        {xr[2], xg[2], xb[2]},
    }};
    const Vec3d w = xyzOf(c.white);
    const Vec3d s = mat3MulVec(mat3Inv(P), w);
    return Mat3{{
        {P[0][0] * s[0], P[0][1] * s[1], P[0][2] * s[2]},
        {P[1][0] * s[0], P[1][1] * s[1], P[1][2] * s[2]},
        {P[2][0] * s[0], P[2][1] * s[1], P[2][2] * s[2]},
    }};
}

inline const std::array<double, 2>& D65() {
    static const std::array<double, 2> v = {0.3127, 0.329};
    return v;
}

inline const Chromaticities& REC709_CHROMATICITIES() {
    static const Chromaticities c = {{0.64, 0.33}, {0.3, 0.6}, {0.15, 0.06}, D65()};
    return c;
}

inline const Mat3& REC709_TO_XYZ() {
    static const Mat3 m = rgbToXyzMatrix(REC709_CHROMATICITIES());
    return m;
}

inline const Mat3& XYZ_TO_REC709() {
    static const Mat3 m = mat3Inv(REC709_TO_XYZ());
    return m;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_MAT3_H
