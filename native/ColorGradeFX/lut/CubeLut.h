/*
 * CubeLut.h - portable C++ port of the TypeScript LUT core (src/core/lut/cube.ts).
 *
 * This is a Phase 2-preview slice: only the pieces the LUT-apply path needs are
 * ported here (parseCube + trilinear sampleLut). The full engine port lands in
 * Phase 2. The TS implementation in src/core/lut/cube.ts is the reference oracle;
 * native/tests/parity/parity_test.cpp asserts these agree to ~1e-4.
 *
 * Deliberately dependency-free (only <string>, <vector>, <cmath>, <sstream>,
 * <stdexcept>) so the exact same header compiles into the AE plugin (MSVC) and
 * into the WSL/CI parity harness (g++/clang). No AE SDK types leak in here.
 *
 * Parity contract with cube.ts - keep these identical:
 *   - .cube ordering: interleaved RGB, red fastest; index of node (r,g,b), channel c
 *     is ((b*size + g)*size + r)*3 + c.
 *   - sampleLut: clamp each input channel to [0,1], scale by (size-1), take the
 *     lower node as min(floor(v), size-2) so the +1 neighbour is always in range,
 *     then trilinear-interpolate (r fastest, then g, then b).
 */
#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <array>
#include <cctype>

namespace cg {

using Vec3 = std::array<float, 3>;

struct Lut3D {
    int size = 0;
    // Interleaved RGB, red fastest (standard .cube ordering), length size^3 * 3.
    std::vector<float> data;
    std::string title;
};

// Parse Adobe/Resolve .cube text into a Lut3D. Mirrors parseCube() in cube.ts:
// ignores blanks/comments/DOMAIN_*/LUT_1D lines, reads TITLE and LUT_3D_SIZE, and
// collects every 3-number data row. Throws on a missing size or a row-count mismatch.
inline Lut3D parseCube(const std::string& text) {
    Lut3D lut;
    int size = 0;
    std::vector<float> values;

    std::istringstream stream(text);
    std::string raw;
    while (std::getline(stream, raw)) {
        // trim whitespace (incl. trailing \r from CRLF files)
        size_t begin = raw.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) continue;
        size_t end = raw.find_last_not_of(" \t\r\n");
        std::string line = raw.substr(begin, end - begin + 1);

        if (line.empty() || line[0] == '#') continue;

        if (line.rfind("TITLE", 0) == 0) {
            std::string rest = line.substr(5);
            size_t s = rest.find_first_not_of(" \t");
            if (s != std::string::npos) rest = rest.substr(s);
            else rest.clear();
            if (!rest.empty() && rest.front() == '"') rest.erase(rest.begin());
            if (!rest.empty() && rest.back() == '"') rest.pop_back();
            lut.title = rest;
            continue;
        }
        if (line.rfind("LUT_3D_SIZE", 0) == 0) {
            std::istringstream ls(line.substr(11));
            ls >> size;
            continue;
        }
        if (line.rfind("DOMAIN_", 0) == 0 || line.rfind("LUT_1D", 0) == 0) continue;

        // A data row: exactly three floats.
        std::istringstream ds(line);
        float r, g, b;
        std::string extra;
        if ((ds >> r >> g >> b) && !(ds >> extra)) {
            values.push_back(r);
            values.push_back(g);
            values.push_back(b);
        }
    }

    if (size == 0) throw std::runtime_error("parseCube: missing LUT_3D_SIZE");
    const size_t expected = static_cast<size_t>(size) * size * size * 3;
    if (values.size() != expected) {
        throw std::runtime_error("parseCube: expected " + std::to_string(size * size * size) +
                                 " entries, got " + std::to_string(values.size() / 3));
    }
    lut.size = size;
    lut.data = std::move(values);
    return lut;
}

// Trilinear sample of a 3D LUT at an encoded RGB point. Byte-for-byte mirror of
// sampleLut() in cube.ts (same clamp, same lower-node selection, same weights).
inline Vec3 sampleLut(const Lut3D& lut, const Vec3& rgb) {
    const int size = lut.size;
    const int n = size - 1;
    const float* data = lut.data.data();

    auto clamp01 = [](float x) -> float { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };

    // Returns {lowerIndex, frac}. Lower node capped at n-1 so index+1 stays valid.
    auto coord = [&](float x, int& i0, float& frac) {
        const float v = clamp01(x) * static_cast<float>(n);
        int f = static_cast<int>(std::floor(v));
        i0 = f < (n - 1) ? f : (n - 1);
        frac = v - static_cast<float>(i0);
    };

    int ri, gi, bi;
    float rf, gf, bf;
    coord(rgb[0], ri, rf);
    coord(rgb[1], gi, gf);
    coord(rgb[2], bi, bf);

    auto at = [&](int r, int g, int b, int c) -> float {
        return data[((static_cast<size_t>(b) * size + g) * size + r) * 3 + c];
    };

    Vec3 out{};
    for (int c = 0; c < 3; ++c) {
        const float c00 = at(ri, gi, bi, c) * (1.0f - rf) + at(ri + 1, gi, bi, c) * rf;
        const float c10 = at(ri, gi + 1, bi, c) * (1.0f - rf) + at(ri + 1, gi + 1, bi, c) * rf;
        const float c01 = at(ri, gi, bi + 1, c) * (1.0f - rf) + at(ri + 1, gi, bi + 1, c) * rf;
        const float c11 = at(ri, gi + 1, bi + 1, c) * (1.0f - rf) + at(ri + 1, gi + 1, bi + 1, c) * rf;
        const float c0 = c00 * (1.0f - gf) + c10 * gf;
        const float c1 = c01 * (1.0f - gf) + c11 * gf;
        out[c] = c0 * (1.0f - bf) + c1 * bf;
    }
    return out;
}

}  // namespace cg
