/*
 * MonotoneCurve.h - C++ port of src/core/engine/monotoneCurve.ts.
 *
 * Shape-preserving PCHIP (Fritsch-Carlson). `forceMonotoneY` (makeMonotoneCurve)
 * clamps y to be non-decreasing so a tone curve cannot reverse; when false
 * (makeShapeCurve) local extrema get a zero tangent so an authored shape (e.g. a
 * chroma-by-luma peak) never overshoots its control points.
 *
 * The construction and the operator() binary search are ported literally,
 * including the search's index bookkeeping, so evaluation matches the oracle.
 */
#pragma once
#ifndef CG_CORE_MONOTONECURVE_H
#define CG_CORE_MONOTONECURVE_H

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace cg {
namespace core {

class MonotoneCurve {
  public:
    // Degenerate identity curve (returns x). Used when there are <2 usable points.
    MonotoneCurve() : identity_(true) {}

    static MonotoneCurve make(const std::vector<double>& xsIn, const std::vector<double>& ysIn,
                              bool forceMonotoneY) {
        if (xsIn.size() != ysIn.size() || xsIn.size() < 2) {
            throw std::runtime_error("makeMonotoneCurve: need >= 2 matching points");
        }
        // Sort by x, then drop points that are not strictly increasing in x.
        std::vector<std::pair<double, double>> pts;
        pts.reserve(xsIn.size());
        for (size_t i = 0; i < xsIn.size(); i++) pts.emplace_back(xsIn[i], ysIn[i]);
        std::stable_sort(pts.begin(), pts.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });

        std::vector<double> xs{pts[0].first};
        std::vector<double> ys{pts[0].second};
        for (size_t k = 1; k < pts.size(); k++) {
            const double x = pts[k].first, y = pts[k].second;
            if (x > xs.back() + 1e-6) {
                xs.push_back(x);
                ys.push_back(forceMonotoneY ? std::max(y, ys.back()) : y);
            }
        }
        const int n = static_cast<int>(xs.size());
        if (n < 2) return MonotoneCurve();  // identity

        std::vector<double> h(n - 1), delta(n - 1);
        for (int i = 0; i < n - 1; i++) {
            h[i] = xs[i + 1] - xs[i];
            delta[i] = (ys[i + 1] - ys[i]) / h[i];
        }
        std::vector<double> m(n, 0.0);
        m[0] = delta[0];
        m[n - 1] = delta[n - 2];
        for (int i = 1; i < n - 1; i++) {
            m[i] = delta[i - 1] * delta[i] <= 0.0 ? 0.0 : (delta[i - 1] + delta[i]) / 2.0;
        }
        // Fritsch-Carlson limiter.
        for (int i = 0; i < n - 1; i++) {
            if (delta[i] == 0.0) {
                m[i] = 0.0;
                m[i + 1] = 0.0;
            } else {
                const double a = m[i] / delta[i];
                const double b = m[i + 1] / delta[i];
                const double s = a * a + b * b;
                if (s > 9.0) {
                    const double t = 3.0 / std::sqrt(s);
                    m[i] = t * a * delta[i];
                    m[i + 1] = t * b * delta[i];
                }
            }
        }

        MonotoneCurve c;
        c.identity_ = false;
        c.n_ = n;
        c.xs_ = std::move(xs);
        c.ys_ = std::move(ys);
        c.h_ = std::move(h);
        c.m_ = std::move(m);
        return c;
    }

    double operator()(double x) const {
        if (identity_) return x;
        const int n = n_;
        if (x <= xs_[0]) return ys_[0];
        if (x >= xs_[n - 1]) return ys_[n - 1];
        int i = 0;
        int lo = 0;
        int hi = n - 2;
        while (lo <= hi) {
            const int mid = (lo + hi) >> 1;
            if (xs_[mid + 1] < x) {
                lo = mid + 1;
            } else if (xs_[mid] > x) {
                hi = mid - 1;
            } else {
                i = mid;
                break;
            }
            i = std::min(std::max(lo, 0), n - 2);
        }
        const double t = (x - xs_[i]) / h_[i];
        const double t2 = t * t;
        const double t3 = t2 * t;
        return ys_[i] * (2 * t3 - 3 * t2 + 1) + h_[i] * m_[i] * (t3 - 2 * t2 + t) +
               ys_[i + 1] * (-2 * t3 + 3 * t2) + h_[i] * m_[i + 1] * (t3 - t2);
    }

  private:
    bool identity_ = true;
    int n_ = 0;
    std::vector<double> xs_, ys_, h_, m_;
};

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_MONOTONECURVE_H
