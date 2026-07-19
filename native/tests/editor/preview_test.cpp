/*
 * preview_test.cpp - headless unit test for the pure Phase 4 live-preview core
 * (editor/PreviewCache.h). Compiled and run by `npm run native:preview-test`
 * (native/scripts/preview-test.ts) with g++/clang, OUTSIDE After Effects, so the
 * risky logic - cache keying/eviction, the scrub/param-change decision, the letterbox
 * fit math, and the "always check the frame back in" guarantee - is proven without a
 * running host. Same convention as native:editor-test (local compiler, NOT in CI).
 *
 * What it covers:
 *   - previewParamFingerprint: stable under float noise <1e-4, sensitive to every
 *     grade-affecting param, so cache keys neither churn nor collide,
 *   - PreviewKey equality across time + params,
 *   - PreviewCache LRU: hit promotes to MRU, capacity bound holds, LRU is evicted,
 *   - decidePreviewAction: UpToDate / ServeCached / Render for scrub + knob nudges,
 *   - letterboxFit: aspect preserved, centered, both letterbox and pillarbox, degenerate,
 *   - ScopedCheckin: checkin fires exactly once, including when the scope unwinds on an
 *     exception (the leak-prevention invariant).
 *
 * Self-asserting: returns non-zero and prints the first failure on any mismatch.
 */
#include "../../ColorGradeFX/editor/PreviewCache.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace cg::editor;

static int g_failures = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static bool approx(double a, double b, double tol = 1e-4) { return std::fabs(a - b) <= tol; }

static std::shared_ptr<const PreviewFrame> makeFrame(int w, int h, uint8_t fill) {
    auto f = std::make_shared<PreviewFrame>();
    f->width = w;
    f->height = h;
    f->rgba.assign(static_cast<size_t>(w) * h * 4, fill);
    return f;
}

// --- fingerprint: stable to noise, sensitive to real changes ----------------
static void test_fingerprint() {
    const uint64_t base = previewParamFingerprint(1, 1, 1, 0.80, 0.75, 1.00);

    // Sub-quantum float noise must NOT change the fingerprint (no cache churn on a
    // re-published-but-identical snapshot).
    CHECK(previewParamFingerprint(1, 1, 1, 0.80 + 1e-6, 0.75 - 1e-6, 1.00 + 1e-6) == base,
          "fingerprint stable under <1e-4 float noise");

    // Every grade-affecting param must move the fingerprint.
    CHECK(previewParamFingerprint(2, 1, 1, 0.80, 0.75, 1.00) != base, "footage changes fp");
    CHECK(previewParamFingerprint(1, 2, 1, 0.80, 0.75, 1.00) != base, "theme changes fp");
    CHECK(previewParamFingerprint(1, 1, 2, 0.80, 0.75, 1.00) != base, "lut source changes fp");
    CHECK(previewParamFingerprint(1, 1, 1, 0.81, 0.75, 1.00) != base, "strength changes fp");
    CHECK(previewParamFingerprint(1, 1, 1, 0.80, 0.76, 1.00) != base, "skin changes fp");
    CHECK(previewParamFingerprint(1, 1, 1, 0.80, 0.75, 1.50) != base, "chroma changes fp");

    // A real 1e-4 slider step is distinguishable (the quantum boundary resolves).
    CHECK(previewParamFingerprint(1, 1, 1, 0.8010, 0.75, 1.00) !=
              previewParamFingerprint(1, 1, 1, 0.8000, 0.75, 1.00),
          "1e-3 strength step distinguished");
}

static void test_key_equality() {
    PreviewKey a{100, 30, previewParamFingerprint(1, 1, 1, 0.8, 0.75, 1.0)};
    PreviewKey b{100, 30, previewParamFingerprint(1, 1, 1, 0.8, 0.75, 1.0)};
    PreviewKey c{101, 30, b.paramFingerprint};  // different time
    PreviewKey d{100, 30, previewParamFingerprint(1, 2, 1, 0.8, 0.75, 1.0)};  // diff params
    CHECK(a == b, "same time+params equal");
    CHECK(a != c, "different time not equal");
    CHECK(a != d, "different params not equal");
}

// --- LRU cache: promotion, bound, eviction ----------------------------------
static void test_cache_lru() {
    PreviewCache cache(2);
    PreviewKey k1{1, 30, 0xA1};
    PreviewKey k2{2, 30, 0xA2};
    PreviewKey k3{3, 30, 0xA3};

    cache.put(k1, makeFrame(4, 4, 1));
    cache.put(k2, makeFrame(4, 4, 2));
    CHECK(cache.size() == 2, "cache holds 2");
    CHECK(cache.contains(k1) && cache.contains(k2), "both present");

    // Touch k1 so it becomes MRU; k2 is now the LRU.
    auto g1 = cache.get(k1);
    CHECK(g1 && g1->rgba[0] == 1, "get k1 returns its frame");

    // Inserting k3 (over cap) must evict the LRU (k2), keep k1 + k3.
    cache.put(k3, makeFrame(4, 4, 3));
    CHECK(cache.size() == 2, "still bounded to 2");
    CHECK(cache.contains(k1), "k1 (recently used) survived");
    CHECK(!cache.contains(k2), "k2 (LRU) evicted");
    CHECK(cache.contains(k3), "k3 inserted");

    // A miss returns null.
    CHECK(cache.get(k2) == nullptr, "evicted key misses");

    // Re-put an existing key updates value without growing.
    cache.put(k1, makeFrame(4, 4, 9));
    CHECK(cache.size() == 2, "re-put existing key doesn't grow");
    CHECK(cache.get(k1)->rgba[0] == 9, "value replaced");
}

static void test_cache_capacity_floor() {
    PreviewCache zero(0);  // must clamp to >=1, never divide-by-zero / accept everything
    CHECK(zero.capacity() == 1, "zero capacity clamps to 1");
    zero.put({1, 30, 1}, makeFrame(2, 2, 1));
    zero.put({2, 30, 2}, makeFrame(2, 2, 2));
    CHECK(zero.size() == 1, "capacity-1 cache holds only newest");
    CHECK(zero.contains(PreviewKey{2, 30, 2}) && !zero.contains(PreviewKey{1, 30, 1}),
          "capacity-1 keeps MRU");
}

// --- scrub / param-change decision ------------------------------------------
static void test_decide_action() {
    PreviewKey a{10, 30, 0xF1};
    PreviewKey b{11, 30, 0xF1};  // scrub one frame forward
    PreviewKey a2{10, 30, 0xF2};  // same time, knob nudged

    // No last publish yet -> must render.
    CHECK(decidePreviewAction(false, PreviewKey{}, a, false) == PreviewAction::Render,
          "first frame renders");
    // Same key already shown -> nothing to do.
    CHECK(decidePreviewAction(true, a, a, true) == PreviewAction::UpToDate,
          "unchanged key is up to date");
    // Scrub to a new time not cached -> render.
    CHECK(decidePreviewAction(true, a, b, false) == PreviewAction::Render,
          "scrub to uncached time renders");
    // Scrub back to a cached time -> serve cached (the interactive path).
    CHECK(decidePreviewAction(true, b, a, true) == PreviewAction::ServeCached,
          "scrub back to cached serves cache");
    // Knob nudge at same time, uncached fingerprint -> render.
    CHECK(decidePreviewAction(true, a, a2, false) == PreviewAction::Render,
          "param change renders");
}

// --- letterbox fit ----------------------------------------------------------
static void test_letterbox() {
    // Wide frame (2:1) into a square box -> fit width, letterbox top/bottom.
    FitRect r = letterboxFit(100, 100, 200, 100);
    CHECK(approx(r.w, 100) && approx(r.h, 50), "wide frame fits width");
    CHECK(approx(r.x, 0) && approx(r.y, 25), "wide frame centered vertically");

    // Tall frame (1:2) into a square box -> fit height, pillarbox left/right.
    r = letterboxFit(100, 100, 100, 200);
    CHECK(approx(r.w, 50) && approx(r.h, 100), "tall frame fits height");
    CHECK(approx(r.x, 25) && approx(r.y, 0), "tall frame centered horizontally");

    // Matching aspect fills exactly, no offset.
    r = letterboxFit(160, 90, 1920, 1080);
    CHECK(approx(r.w, 160) && approx(r.h, 90) && approx(r.x, 0) && approx(r.y, 0),
          "matching aspect fills box");

    // Aspect is preserved (w/h of dst == w/h of frame) for an odd size.
    r = letterboxFit(300, 200, 1920, 817);
    CHECK(approx(r.w / r.h, 1920.0 / 817.0, 1e-3), "aspect preserved");
    CHECK(r.w <= 300.0f + 1e-3f && r.h <= 200.0f + 1e-3f, "dst fits inside box");

    // Degenerate inputs -> empty rect, no NaN/inf.
    r = letterboxFit(0, 100, 100, 100);
    CHECK(approx(r.w, 0) && approx(r.h, 0), "zero avail -> empty");
    r = letterboxFit(100, 100, 0, 100);
    CHECK(approx(r.w, 0) && approx(r.h, 0), "zero frame -> empty");
}

// --- ScopedCheckin: exactly once, even on exception -------------------------
static void test_scoped_checkin() {
    int checkins = 0;
    {
        auto guard = makeScopedCheckin([&] { ++checkins; });
        (void)guard;
    }
    CHECK(checkins == 1, "checkin fires once on normal scope exit");

    // Fires during stack unwind when the copy throws (the leak-prevention case).
    checkins = 0;
    try {
        auto guard = makeScopedCheckin([&] { ++checkins; });
        (void)guard;
        throw std::runtime_error("copy failed");
    } catch (...) {
        // swallowed
    }
    CHECK(checkins == 1, "checkin fires exactly once during exception unwind");

    // disarm suppresses it (ownership handed elsewhere) - not used by the preview path,
    // but part of the contract.
    checkins = 0;
    {
        auto guard = makeScopedCheckin([&] { ++checkins; });
        guard.disarm();
    }
    CHECK(checkins == 0, "disarm suppresses checkin");
}

int main() {
    test_fingerprint();
    test_key_equality();
    test_cache_lru();
    test_cache_capacity_floor();
    test_decide_action();
    test_letterbox();
    test_scoped_checkin();

    if (g_failures == 0) {
        std::printf("preview-test: PASS (all live-preview core checks)\n");
        return 0;
    }
    std::printf("preview-test: FAIL (%d checks)\n", g_failures);
    return 1;
}
