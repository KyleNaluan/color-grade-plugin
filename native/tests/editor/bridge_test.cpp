/*
 * bridge_test.cpp - headless unit test for the pure editor<->effect bridge seam
 * (editor/EditorBridge.h). Compiled and run by `npm run native:editor-test`
 * (native/scripts/editor-bridge-test.ts) with g++/clang, OUTSIDE After Effects, so
 * the window->effect edit plumbing is proven without a running host. Mirrors the
 * Phase 1/2 parity-harness convention (local compiler, deliberately not in CI).
 *
 * What it covers:
 *   - EditQueue FIFO across distinct fields + latest-value coalescing per field,
 *   - concurrent push (UI thread) / drain (AE main thread) never loses or tears an
 *     edit (thread-safety of the cross-thread channel),
 *   - percent<->fraction mapping round-trips and the clamps hold their bands,
 *   - applyEdit models the param write: a drained edit lands back in the snapshot
 *     the window displays (the "edit reflects in Effect Controls" invariant, pure).
 *
 * Self-asserting: returns non-zero and prints the first failure on any mismatch.
 */
#include "../../ColorGradeFX/editor/EditorBridge.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace cg::editor;

static int g_failures = 0;

#define CHECK(cond, msg)                                             \
    do {                                                             \
        if (!(cond)) {                                               \
            std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            ++g_failures;                                            \
        }                                                            \
    } while (0)

static bool approx(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// --- EditQueue: FIFO across fields + coalescing per field -------------------
static void test_queue_basic() {
    EditQueue q;
    CHECK(q.empty(), "new queue empty");

    q.push({EditField::Strength, 0.5});
    q.push({EditField::SkinProtection, 0.2});
    q.push({EditField::Strength, 0.9});  // coalesce onto the existing Strength edit

    CHECK(q.size() == 2, "coalesced to 2 distinct fields");
    auto drained = q.drain();
    CHECK(drained.size() == 2, "drain returns 2");
    // Order preserved: Strength first (its slot), then SkinProtection.
    CHECK(drained[0].field == EditField::Strength && approx(drained[0].value, 0.9),
          "Strength coalesced to latest 0.9, kept first slot");
    CHECK(drained[1].field == EditField::SkinProtection && approx(drained[1].value, 0.2),
          "SkinProtection second");
    CHECK(q.empty(), "queue empty after drain");
}

// --- EditQueue: concurrent push/drain loses nothing -------------------------
static void test_queue_threaded() {
    EditQueue q;
    const int kIters = 20000;
    std::atomic<bool> producer_done{false};

    // Producer floods one field (ChromaGain) with monotonically rising values on a
    // "UI thread"; consumer drains on the "AE main thread". Because ChromaGain
    // coalesces, the consumer must observe a non-decreasing sequence of values and,
    // critically, the FINAL drained value must equal the last produced value - no
    // torn double, no lost final edit.
    double last_seen = -1.0;
    bool   monotonic = true;

    std::thread producer([&] {
        for (int i = 0; i < kIters; ++i) {
            q.push({EditField::ChromaGain, static_cast<double>(i)});
        }
        producer_done.store(true);
    });

    while (!producer_done.load() || !q.empty()) {
        for (const auto& e : q.drain()) {
            if (e.value + 1e-9 < last_seen) monotonic = false;
            last_seen = e.value;
        }
    }
    producer.join();
    // Final flush in case the last push raced the empty() check above.
    for (const auto& e : q.drain()) last_seen = e.value;

    CHECK(monotonic, "coalesced values observed monotonically (no torn/reordered edit)");
    CHECK(approx(last_seen, static_cast<double>(kIters - 1)),
          "final drained value == last produced value (no lost final edit)");
}

// --- percent<->fraction + clamps --------------------------------------------
static void test_mapping() {
    CHECK(approx(percentToFraction(80.0), 0.8), "80% -> 0.8");
    CHECK(approx(fractionToPercent(0.8), 80.0), "0.8 -> 80%");
    CHECK(approx(fractionToPercent(percentToFraction(37.5)), 37.5), "percent round-trip");

    CHECK(approx(clamp01(-0.3), 0.0) && approx(clamp01(1.4), 1.0) && approx(clamp01(0.5), 0.5),
          "clamp01 band");
    CHECK(approx(clampChromaFraction(-1.0), 0.0) && approx(clampChromaFraction(9.0), 3.0),
          "chroma clamped to [0,3]");
    CHECK(approx(clampRange(5.0, 1.0, 3.0), 3.0) && approx(clampRange(0.0, 1.0, 3.0), 1.0),
          "clampRange band");
}

// --- applyEdit: a drained edit lands back in the visible snapshot ------------
static void test_apply_roundtrip() {
    ParamSnapshot s;  // defaults: footage=1, theme=1, strength=0.8, skin=0.75, chroma=1.0, lut=1
    EditQueue q;
    q.push({EditField::FootageProfile, 2});
    q.push({EditField::Theme, 2});
    q.push({EditField::Strength, 0.42});
    q.push({EditField::SkinProtection, 0.10});
    q.push({EditField::ChromaGain, 1.75});
    q.push({EditField::LutSource, 3});

    for (const auto& e : q.drain()) applyEdit(s, e);

    CHECK(s.footageProfile == 2, "footage profile applied");
    CHECK(s.theme == 2, "theme applied");
    CHECK(approx(s.strength, 0.42), "strength applied");
    CHECK(approx(s.skinProtection, 0.10), "skin applied");
    CHECK(approx(s.chromaGain, 1.75), "chroma applied");
    CHECK(s.lutSource == 3, "lut source applied");

    // Out-of-band values are clamped by applyEdit (defensive against a bad UI value).
    ParamSnapshot s2;
    applyEdit(s2, {EditField::Strength, 3.0});
    applyEdit(s2, {EditField::ChromaGain, -2.0});
    CHECK(approx(s2.strength, 1.0), "over-range strength clamped");
    CHECK(approx(s2.chromaGain, 0.0), "under-range chroma clamped");

    // Popup rounding: a double that is nominally an index snaps to the nearest int.
    ParamSnapshot s3;
    applyEdit(s3, {EditField::Theme, 2.999999});
    CHECK(s3.theme == 3, "popup index rounds to nearest");
}

int main() {
    test_queue_basic();
    test_queue_threaded();
    test_mapping();
    test_apply_roundtrip();

    if (g_failures == 0) {
        std::printf("editor-bridge-test: PASS (all bridge-logic checks)\n");
        return 0;
    }
    std::printf("editor-bridge-test: FAIL (%d checks)\n", g_failures);
    return 1;
}
