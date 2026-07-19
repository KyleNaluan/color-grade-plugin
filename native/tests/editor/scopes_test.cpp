/*
 * scopes_test.cpp - headless unit test for the pure Phase 5 editor scopes core
 * (editor/Scopes.h). Compiled and run by `npm run native:scopes-test`
 * (native/scripts/scopes-test.ts) with g++/clang, OUTSIDE After Effects, so the scope
 * binning + image synthesis is proven without a running host. Same convention as
 * native:preview-test (local compiler, NOT in CI).
 *
 * What it covers:
 *   - luma8: Rec.709 luma of primaries + neutrals,
 *   - computeHistogram: bin placement for black/white/mid-gray/colour, counts sum to N,
 *   - renderHistogram / renderWaveform / renderVectorscope: correct image dimensions,
 *     opaque, non-empty for real input, empty/degenerate inputs are safe,
 *   - waveform: a top-bright / bottom-dark split lands bright pixels high and dark low,
 *   - vectorscope: neutral gray concentrates at the centre; saturated red pushes off it.
 *
 * Self-asserting: returns non-zero and prints the first failure on any mismatch.
 */
#include "../../ColorGradeFX/editor/Scopes.h"

#include <cstdio>
#include <vector>

using namespace cg::editor;

static int g_failures = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

// Build a solid-colour w x h RGBA image.
static std::vector<uint8_t> solid(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> v(static_cast<size_t>(w) * h * 4u);
    for (int i = 0; i < w * h; ++i) {
        v[i * 4 + 0] = r;
        v[i * 4 + 1] = g;
        v[i * 4 + 2] = b;
        v[i * 4 + 3] = 255;
    }
    return v;
}

static bool anyLit(const ScopeImage& img) {
    for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        if (img.rgba[i] || img.rgba[i + 1] || img.rgba[i + 2]) return true;
    }
    return false;
}

static void test_luma() {
    CHECK(luma8(0, 0, 0) == 0, "black luma 0");
    CHECK(luma8(255, 255, 255) == 255, "white luma 255");
    CHECK(luma8(128, 128, 128) == 128, "mid gray luma 128");
    CHECK(luma8(255, 0, 0) == 54, "red luma ~0.2126*255");
    CHECK(luma8(0, 255, 0) == 182, "green luma ~0.7152*255");
}

static void test_histogram() {
    auto black = solid(8, 8, 0, 0, 0);
    Histogram hb = computeHistogram(black.data(), 8, 8, 256);
    CHECK(hb.bins == 256, "256 bins");
    CHECK(hb.luma[0] == 64, "all 64 black pixels in luma bin 0");
    CHECK(hb.r[0] == 64 && hb.g[0] == 64 && hb.b[0] == 64, "all channels in bin 0");

    auto white = solid(4, 4, 255, 255, 255);
    Histogram hw = computeHistogram(white.data(), 4, 4, 256);
    CHECK(hw.luma[255] == 16, "white in luma bin 255");

    auto mid = solid(4, 4, 128, 128, 128);
    Histogram hm = computeHistogram(mid.data(), 4, 4, 256);
    CHECK(hm.luma[128] == 16, "mid gray in luma bin 128");

    // Counts sum to N in every channel.
    uint32_t sum = 0;
    for (int i = 0; i < 256; ++i) sum += hm.r[i];
    CHECK(sum == 16, "histogram counts sum to pixel count");

    // Red image: red channel peaks at 255, green/blue at 0.
    auto red = solid(4, 4, 255, 0, 0);
    Histogram hr = computeHistogram(red.data(), 4, 4, 256);
    CHECK(hr.r[255] == 16 && hr.g[0] == 16 && hr.b[0] == 16, "red splits channels");

    // Degenerate inputs are safe.
    Histogram hn = computeHistogram(nullptr, 0, 0, 256);
    CHECK(hn.bins == 256 && hn.luma[0] == 0, "null input -> empty histogram");
}

static void test_histogram_image() {
    auto mid = solid(16, 16, 128, 128, 128);
    Histogram hm = computeHistogram(mid.data(), 16, 16, 256);
    ScopeImage img = renderHistogram(hm, 256, 100);
    CHECK(img.valid(), "histogram image valid");
    CHECK(img.width == 256 && img.height == 100, "histogram image dimensions");
    CHECK(anyLit(img), "histogram image has lit pixels");
    // Alpha is opaque everywhere.
    bool opaque = true;
    for (size_t i = 3; i < img.rgba.size(); i += 4)
        if (img.rgba[i] != 255) opaque = false;
    CHECK(opaque, "histogram image opaque");

    CHECK(!renderHistogram(hm, 0, 100).valid(), "zero-width histogram image empty");
}

static void test_waveform() {
    // Top half white, bottom half black (row 0 = top).
    const int w = 16, h = 16;
    std::vector<uint8_t> img(static_cast<size_t>(w) * h * 4u, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t v = (y < h / 2) ? 255 : 0;
            uint8_t* p = &img[(static_cast<size_t>(y) * w + x) * 4u];
            p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
        }
    ScopeImage wf = renderWaveform(img.data(), w, h, 64, 64);
    CHECK(wf.valid() && wf.width == 64 && wf.height == 64, "waveform dimensions");
    CHECK(anyLit(wf), "waveform has lit pixels");

    // Bright pixels (luma 255) map to the TOP rows; dark (luma 0) to the BOTTOM rows.
    auto litInRow = [&](int y) {
        for (int x = 0; x < wf.width; ++x)
            if (wf.rgba[(static_cast<size_t>(y) * wf.width + x) * 4u + 1]) return true;
        return false;
    };
    CHECK(litInRow(0), "white maps to the top waveform row");
    CHECK(litInRow(wf.height - 1), "black maps to the bottom waveform row");

    CHECK(!renderWaveform(nullptr, 0, 0, 64, 64).valid(), "null waveform input safe");
}

static void test_vectorscope() {
    // Neutral gray: everything at the centre.
    auto gray = solid(16, 16, 120, 120, 120);
    ScopeImage vg = renderVectorscope(gray.data(), 16, 16, 128);
    CHECK(vg.valid() && vg.width == 128 && vg.height == 128, "vectorscope dimensions");
    const int c = 128 / 2;
    auto lit = [&](const ScopeImage& s, int x, int y) {
        return s.rgba[(static_cast<size_t>(y) * s.width + x) * 4u + 1] != 0;
    };
    CHECK(lit(vg, c, c), "neutral gray lands at the vectorscope centre");
    // Off-centre should be dark for a neutral image.
    CHECK(!lit(vg, c + 30, c), "neutral has no off-centre energy");

    // Saturated red pushes OFF centre (into the red target quadrant, Cr>0 -> upward-ish).
    auto red = solid(16, 16, 255, 0, 0);
    ScopeImage vr = renderVectorscope(red.data(), 16, 16, 128);
    bool centerLit = lit(vr, c, c);
    bool offCenterLit = anyLit(vr) && !centerLit;
    CHECK(offCenterLit, "saturated red plots away from the centre");

    CHECK(!renderVectorscope(nullptr, 0, 0, 128).valid(), "null vectorscope input safe");
}

int main() {
    test_luma();
    test_histogram();
    test_histogram_image();
    test_waveform();
    test_vectorscope();

    if (g_failures == 0) {
        std::printf("scopes-test: PASS (all editor scopes core checks)\n");
        return 0;
    }
    std::printf("scopes-test: FAIL (%d checks)\n", g_failures);
    return 1;
}
