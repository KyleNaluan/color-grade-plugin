/*
 * analysis_test.cpp - headless unit test for the pure Phase 5 in-effect analysis core
 * (editor/Analysis.h). Compiled and run by `npm run native:analysis-test`
 * (native/scripts/analysis-test.ts) with g++/clang, OUTSIDE After Effects, so the risky
 * logic - the multi-frame sample schedule, the incremental job state machine, the
 * analysis fingerprint, and the slider-drag debounce - is proven without a running host.
 * Same convention as native:preview-test (local compiler, NOT in CI).
 *
 * What it covers:
 *   - frameSampleSchedule: N centred samples across a span, ordered, in-bounds,
 *     degenerate (1 sample, zero duration) collapses to the in-point,
 *   - AnalysisFingerprint equality (footage + span) and validity gate,
 *   - AnalysisDebounce: fires only after a target is stable for the window, restarts on a
 *     changed target, never re-fires an already-accepted target, accept() short-circuits,
 *   - AnalysisJob: sample cursor walks the schedule, aggregation concatenates, Ready only
 *     after every frame is in (and only if pixels were gathered), skip keeps it moving,
 *   - perFramePixelBudget / decimationStride: bounded, always >=1, honour the budget.
 *
 * Self-asserting: returns non-zero and prints the first failure on any mismatch.
 */
#include "../../ColorGradeFX/editor/Analysis.h"

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

// --- schedule ---------------------------------------------------------------
static void test_schedule() {
    // 4 samples across [0, 100): centres at 12.5, 37.5, 62.5, 87.5 -> round-half-up.
    auto s = frameSampleSchedule(0, 100, 30, 4);
    CHECK(s.size() == 4, "4 samples requested -> 4 times");
    CHECK(s[0].value == 13 && s[1].value == 38 && s[2].value == 63 && s[3].value == 88,
          "centred samples at the sub-interval midpoints");
    for (auto& t : s) CHECK(t.scale == 30, "scale carried through unchanged");

    // Strictly increasing and inside [inPoint, inPoint+duration).
    for (size_t i = 1; i < s.size(); ++i) CHECK(s[i].value > s[i - 1].value, "monotonic times");
    CHECK(s.front().value >= 0 && s.back().value < 100, "all times within the span");

    // A non-zero in-point offsets every sample.
    auto off = frameSampleSchedule(1000, 100, 30, 4);
    CHECK(off[0].value == 1013 && off[3].value == 1088, "in-point offsets the schedule");

    // Degenerate: one sample, or zero/negative duration -> single sample at the in-point.
    CHECK(frameSampleSchedule(500, 100, 30, 1).size() == 1, "1 sample -> size 1");
    CHECK(frameSampleSchedule(500, 100, 30, 1)[0].value == 500, "1 sample at in-point");
    auto z = frameSampleSchedule(500, 0, 30, 8);
    CHECK(z.size() == 1 && z[0].value == 500, "zero duration collapses to in-point");
    auto neg = frameSampleSchedule(500, -10, 30, 8);
    CHECK(neg.size() == 1 && neg[0].value == 500, "negative duration collapses to in-point");

    // numSamples < 1 clamps to 1; scale 0 clamps to 1.
    CHECK(frameSampleSchedule(0, 100, 0, 0).size() == 1, "n<1 clamps to 1");
    CHECK(frameSampleSchedule(0, 100, 0, 4)[0].scale == 1, "scale 0 clamps to 1");
}

// --- fingerprint ------------------------------------------------------------
static void test_fingerprint() {
    AnalysisFingerprint a{1, 0, 100, 30};
    AnalysisFingerprint b{1, 0, 100, 30};
    AnalysisFingerprint diffFoot{2, 0, 100, 30};
    AnalysisFingerprint diffSpan{1, 0, 200, 30};
    CHECK(a == b, "identical fingerprints equal");
    CHECK(a != diffFoot, "footage profile distinguishes");
    CHECK(a != diffSpan, "clip span distinguishes");
    CHECK(a.valid(), "footage set -> valid");
    CHECK(!AnalysisFingerprint{}.valid(), "unset footage -> invalid");
}

// --- debounce ---------------------------------------------------------------
static void test_debounce() {
    AnalysisDebounce d(3);  // must be stable 3 ticks
    AnalysisFingerprint fpA{1, 0, 100, 30};
    AnalysisFingerprint fpB{2, 0, 100, 30};

    CHECK(!d.observe(fpA, 0), "tick 0: timer just started");
    CHECK(!d.observe(fpA, 1), "tick 1: not yet stable");
    CHECK(!d.observe(fpA, 2), "tick 2: not yet stable");
    CHECK(d.observe(fpA, 3), "tick 3: stable window elapsed -> fire");
    CHECK(d.hasAccepted() && d.accepted() == fpA, "accepted fingerprint recorded");

    // Same accepted target never re-fires.
    CHECK(!d.observe(fpA, 4), "already-analysed target does not re-fire");
    CHECK(!d.observe(fpA, 100), "still no re-fire much later");

    // A new target restarts the timer and fires after its own window.
    CHECK(!d.observe(fpB, 5), "new target restarts timer");
    CHECK(!d.observe(fpB, 7), "still settling");
    CHECK(d.observe(fpB, 8), "new target fires after its window");

    // A target that changes before settling never fires (drag scrubbing profiles).
    AnalysisDebounce d2(3);
    CHECK(!d2.observe(fpA, 0), "d2 start A");
    CHECK(!d2.observe(fpB, 1), "d2 switch to B resets");
    CHECK(!d2.observe(fpA, 2), "d2 switch back to A resets");
    CHECK(!d2.observe(fpA, 4), "d2 A only stable 2 ticks");
    CHECK(d2.observe(fpA, 5), "d2 A now stable 3 ticks -> fire");

    // accept() short-circuits the wait (cache hit path).
    AnalysisDebounce d3(3);
    d3.accept(fpB);
    CHECK(d3.hasAccepted() && d3.accepted() == fpB, "accept records without waiting");
    CHECK(!d3.observe(fpB, 0), "accepted target does not fire");

    // Invalid fingerprint never fires.
    CHECK(!d.observe(AnalysisFingerprint{}, 200), "invalid fingerprint never fires");
}

// --- job --------------------------------------------------------------------
static void test_job() {
    AnalysisJob job;
    AnalysisFingerprint fp{1, 0, 120, 30};
    job.begin(fp, frameSampleSchedule(0, 120, 30, 3));

    CHECK(job.state() == AnalysisState::Sampling, "job starts sampling");
    CHECK(job.totalFrames() == 3, "3 scheduled frames");
    CHECK(job.fingerprint() == fp, "job carries its fingerprint");

    // Schedule centres for [0,120) with 3 samples are 20, 60, 100.
    SampleTime t;
    CHECK(job.nextSample(t) && t.value == 20, "first sample is the first scheduled time");

    // Feed three 2-pixel (6 float) frames; each addFrame advances the cursor.
    float frame[6] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    CHECK(job.nextSample(t) && t.value == 20, "sample 0 pending");
    job.addFrame(frame, 6);
    CHECK(job.sampledFrames() == 1 && job.state() == AnalysisState::Sampling, "after 1 frame");
    CHECK(job.nextSample(t), "sample 1 pending");
    job.addFrame(frame, 6);
    CHECK(job.sampledFrames() == 2, "after 2 frames");
    job.addFrame(frame, 6);
    CHECK(job.state() == AnalysisState::Ready, "all frames in -> Ready");
    CHECK(job.pixels().size() == 18, "aggregate concatenates all frames");
    CHECK(!job.nextSample(t), "no samples once Ready");

    job.reset();
    CHECK(job.state() == AnalysisState::Idle && job.pixels().empty(), "reset clears the job");

    // Skips keep the job progressing; an all-skipped run yields Idle (nothing to measure).
    AnalysisJob job2;
    job2.begin(fp, frameSampleSchedule(0, 120, 30, 2));
    job2.advanceSkip();
    job2.advanceSkip();
    CHECK(job2.state() == AnalysisState::Idle, "all-skipped run falls back to Idle");
    CHECK(job2.pixels().empty(), "no pixels gathered when all skipped");

    // A mixed run (one good, one skipped) is Ready with just the good frame's pixels.
    AnalysisJob job3;
    job3.begin(fp, frameSampleSchedule(0, 120, 30, 2));
    job3.addFrame(frame, 6);
    job3.advanceSkip();
    CHECK(job3.state() == AnalysisState::Ready && job3.pixels().size() == 6,
          "mixed run Ready with the gathered frame");
}

// --- budget / decimation ----------------------------------------------------
static void test_budget() {
    CHECK(perFramePixelBudget(120000, 4) == 30000, "budget splits across frames");
    CHECK(perFramePixelBudget(120000, 0) == 120000, "n<1 clamps to 1 frame");
    CHECK(perFramePixelBudget(2, 8) == 1, "tiny budget floors at 1");

    // Stride decimates within budget; decimated count <= budget.
    int step = decimationStride(1920, 1080, 30000);
    int ow = (1920 + step - 1) / step, oh = (1080 + step - 1) / step;
    CHECK(step >= 1, "stride at least 1");
    CHECK((int64_t)ow * oh <= 30000, "decimated frame within budget");

    // A frame already under budget needs no decimation.
    CHECK(decimationStride(100, 100, 30000) == 1, "small frame -> stride 1");
    // Degenerate inputs are safe.
    CHECK(decimationStride(0, 100, 30000) == 1, "zero width -> stride 1");
    CHECK(decimationStride(100, 100, 0) == 1, "zero budget -> stride 1");
}

int main() {
    test_schedule();
    test_fingerprint();
    test_debounce();
    test_job();
    test_budget();

    if (g_failures == 0) {
        std::printf("analysis-test: PASS (all in-effect analysis core checks)\n");
        return 0;
    }
    std::printf("analysis-test: FAIL (%d checks)\n", g_failures);
    return 1;
}
