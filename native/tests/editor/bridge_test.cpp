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

// --- Phase 6a: the manual grade + keyframeable scalars round-trip ------------
static void test_phase6a_manual() {
    ParamSnapshot s;  // defaults: exposure 0, lookMix 1, temperature 0, manual neutral

    // The three keyframeable scalar edits.
    applyEdit(s, {EditField::Exposure, 1.75});
    applyEdit(s, {EditField::Temperature, -40.0});
    applyEdit(s, {EditField::LookMix, 0.5});
    CHECK(approx(s.exposure, 1.75), "exposure applied");
    CHECK(approx(s.temperature, -40.0), "temperature applied");
    CHECK(approx(s.lookMix, 0.5), "look mix applied");
    applyEdit(s, {EditField::LookMix, 1.9});  // over-range fraction clamps to 1
    CHECK(approx(s.lookMix, 1.0), "look mix clamped to 1");

    // A Manual edit carries the whole recipe-backed ManualState.
    ParamEdit me;
    me.field = EditField::Manual;
    me.manual.contrast = 60;
    me.manual.shadows = 30;
    me.manual.saturation = 1.4;
    me.manual.vibrance = 25;
    applyEdit(s, me);
    CHECK(approx(s.manual.contrast, 60) && approx(s.manual.shadows, 30) &&
              approx(s.manual.saturation, 1.4) && approx(s.manual.vibrance, 25),
          "manual state applied");

    // Manual edits coalesce to one queue entry (latest ManualState wins).
    EditQueue q;
    ParamEdit a;
    a.field = EditField::Manual;
    a.manual.tint = 10;
    q.push(a);
    ParamEdit b;
    b.field = EditField::Manual;
    b.manual.tint = 55;
    q.push(b);
    CHECK(q.size() == 1, "manual edits coalesce to one field");
    auto drained = q.drain();
    CHECK(drained.size() == 1 && approx(drained[0].manual.tint, 55), "latest manual wins");

    // ManualState equality (used by SnapshotChanged on the effect side).
    ManualState m1, m2;
    CHECK(m1 == m2, "default ManualState equal");
    m2.blacks = -20;
    CHECK(m1 != m2, "differing ManualState unequal");
}

// --- Phase 6b/6c: curves + wheels round-trip through the recipe-backed edit path ----
static void test_phase6bc_curves_wheels() {
    // isRecipeBackedField classifies the three arb-backed fields, and only those.
    CHECK(isRecipeBackedField(EditField::Manual), "Manual is recipe-backed");
    CHECK(isRecipeBackedField(EditField::Curves), "Curves is recipe-backed");
    CHECK(isRecipeBackedField(EditField::Wheels), "Wheels is recipe-backed");
    CHECK(!isRecipeBackedField(EditField::Strength), "Strength is not recipe-backed");
    CHECK(!isRecipeBackedField(EditField::Exposure), "Exposure is not recipe-backed");

    // A Curves edit round-trips the whole four-curve state into the snapshot.
    ParamSnapshot s;
    ParamEdit ce;
    ce.field = EditField::Curves;
    ce.curves.master.count = 3;
    ce.curves.master.x[0] = 0.0; ce.curves.master.y[0] = 0.0;
    ce.curves.master.x[1] = 0.5; ce.curves.master.y[1] = 0.6;
    ce.curves.master.x[2] = 1.0; ce.curves.master.y[2] = 1.0;
    ce.curves.r.count = 2;
    ce.curves.r.x[0] = 0.0; ce.curves.r.y[0] = 0.1;
    ce.curves.r.x[1] = 1.0; ce.curves.r.y[1] = 0.9;
    applyEdit(s, ce);
    CHECK(s.curves.master.count == 3 && approx(s.curves.master.y[1], 0.6), "curve master applied");
    CHECK(s.curves.r.count == 2 && approx(s.curves.r.y[0], 0.1), "curve red applied");
    CHECK(s.curves == ce.curves, "curves state equal after apply");

    // A Wheels edit round-trips the LGG triples + 3-way band tints + mode.
    ParamEdit we;
    we.field = EditField::Wheels;
    we.wheels.lift[0] = 0.05; we.wheels.gamma[1] = 1.2; we.wheels.gain[2] = 0.9;
    we.wheels.hasShadowTint = true; we.wheels.shadowTint[0] = 6; we.wheels.shadowTint[1] = -8;
    we.wheels.mode = 1;
    applyEdit(s, we);
    CHECK(approx(s.wheels.lift[0], 0.05) && approx(s.wheels.gamma[1], 1.2) &&
              approx(s.wheels.gain[2], 0.9), "wheels LGG applied");
    CHECK(s.wheels.hasShadowTint && approx(s.wheels.shadowTint[0], 6), "wheels shadow tint applied");
    CHECK(s.wheels.mode == 1, "wheels mode applied");
    CHECK(s.wheels == we.wheels, "wheels state equal after apply");

    // Curves / Wheels coalesce per field (latest wins), like Manual.
    EditQueue q;
    ParamEdit a; a.field = EditField::Curves; a.curves.master.count = 2;
    ParamEdit b; b.field = EditField::Curves; b.curves.master.count = 5;
    q.push(a);
    q.push(b);
    CHECK(q.size() == 1, "curve edits coalesce to one field");
    auto drained = q.drain();
    CHECK(drained.size() == 1 && drained[0].curves.master.count == 5, "latest curve wins");

    // CurveState / WheelsState equality (used by SnapshotChanged on the effect side).
    CurveState c1, c2;
    CHECK(c1 == c2, "default CurveState equal");
    c2.count = 1;
    CHECK(c1 != c2, "differing CurveState unequal");
    WheelsState w1, w2;
    CHECK(w1 == w2, "default WheelsState equal (neutral: lift 0, gamma 1, gain 1)");
    CHECK(approx(w1.gamma[0], 1.0) && approx(w1.gain[2], 1.0) && approx(w1.lift[1], 0.0),
          "default WheelsState is neutral LGG");
    w2.gain[0] = 1.3;
    CHECK(w1 != w2, "differing WheelsState unequal");
}

// --- Phase 6b: curve point manipulation stays a monotone function under far drags -------
// Regression for the round-1 "dots detach from the line" bug: the widget must keep points
// x-ascending AND y-non-decreasing so the engine's forceMonotoneY PCHIP (drawn + baked)
// passes through every dot. curveClampPoint is what enforces it.
static void test_phase6b_curve_drag() {
    CurveState c;
    CHECK(c.count == 0, "curve starts absent");
    curveEnsureEndpoints(c);
    CHECK(c.count == 2 && curveIsMonotone(c), "endpoints seeded, monotone");

    int i = curveInsertPoint(c, 0.5, 0.5);
    CHECK(i == 1 && c.count == 3 && curveIsMonotone(c), "insert mid point, monotone");

    // Far drag DOWN past the shadow endpoint: y clamps to the previous point's y (0), never below.
    curveClampPoint(c, 1, 0.5, -5.0);
    CHECK(approx(c.y[1], 0.0) && curveIsMonotone(c), "far-down drag clamps to prev y, monotone");
    // Far drag UP past the highlight endpoint: y clamps to the next point's y (1).
    curveClampPoint(c, 1, 0.5, 5.0);
    CHECK(approx(c.y[1], 1.0) && curveIsMonotone(c), "far-up drag clamps to next y, monotone");
    // Far drag LEFT/RIGHT: interior x stays strictly between neighbors (never crosses an endpoint).
    curveClampPoint(c, 1, -5.0, 0.5);
    CHECK(c.x[1] > c.x[0] && c.x[1] < c.x[2] && curveIsMonotone(c), "far-left drag keeps x ordered");
    curveClampPoint(c, 1, 5.0, 0.5);
    CHECK(c.x[1] > c.x[0] && c.x[1] < c.x[2] && curveIsMonotone(c), "far-right drag keeps x ordered");

    // A pile of inserts + wild drags (mimicking Curves2.png) must never break monotonicity.
    CurveState d;
    curveEnsureEndpoints(d);
    const double xs[] = {0.2, 0.8, 0.35, 0.6, 0.15, 0.9, 0.5};
    for (double x : xs) curveInsertPoint(d, x, 0.5);
    CHECK(curveIsMonotone(d), "many inserts stay monotone");
    // Drag every interior point to extreme, alternating y; must stay a monotone function.
    for (int k = 1; k < d.count - 1; ++k) {
        curveClampPoint(d, k, d.x[k], (k % 2) ? 9.0 : -9.0);
        CHECK(curveIsMonotone(d), "extreme alternating drag stays monotone");
    }
    // In-range drag stores the value exactly (no clamp when between neighbors).
    CurveState e;
    curveEnsureEndpoints(e);
    curveInsertPoint(e, 0.4, 0.4);
    curveClampPoint(e, 1, 0.45, 0.6);
    CHECK(approx(e.x[1], 0.45) && approx(e.y[1], 0.6), "in-range drag stored exactly");

    // Removing the interior point returns to the two fixed endpoints (identity curve).
    curveRemovePoint(e, 1);
    CHECK(e.count == 2 && approx(e.y[0], 0.0) && approx(e.y[1], 1.0), "remove interior -> endpoints");
}

int main() {
    test_queue_basic();
    test_queue_threaded();
    test_mapping();
    test_apply_roundtrip();
    test_phase6a_manual();
    test_phase6bc_curves_wheels();
    test_phase6b_curve_drag();

    if (g_failures == 0) {
        std::printf("editor-bridge-test: PASS (all bridge-logic checks)\n");
        return 0;
    }
    std::printf("editor-bridge-test: FAIL (%d checks)\n", g_failures);
    return 1;
}
