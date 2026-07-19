/*
 * Analysis.h - the pure, host-agnostic core of the Phase 5 in-effect clip analysis.
 * NO AE SDK, NO Win32, NO ImGui, NO core-engine types here on purpose: everything in
 * this header is exercised headlessly by native:analysis-test
 * (native/tests/editor/analysis_test.cpp), so the risky logic - the multi-frame sample
 * schedule, the incremental job state machine, the analysis fingerprint, and the
 * slider-drag debounce - is proven WITHOUT a running After Effects.
 *
 * The AE-facing parts (checking out the layer frame UPSTREAM of this effect at each
 * scheduled time via AEGP_NewFromUpstreamOfEffect + AEGP_SetTime, decoding it to
 * Rec.709, and running cg::core::computeStats over the aggregate) live in
 * ColorGrade.cpp and are captain-verified; they lean on the primitives here so the
 * risky logic is the tested logic.
 *
 * Why multi-frame: a single frame under-samples a clip's tonal/chroma range. Phase 5
 * samples N frames evenly across the layer's duration and analyses the union, so the
 * grade adapts to the whole shot, not one instant. Why incremental: AEGP checkout is
 * main-thread only (the idle hook), so doing all N renders in one tick would stall AE.
 * The job renders ONE frame per idle tick, accumulating, so AE stays responsive.
 *
 * Decode invariant: the frames fed to addFrame() are already DECODED to Rec.709 by the
 * caller (V-Log decoded via the footage profile, Rec.709 passed through), exactly the
 * same decodePixelToRec709 the Auto bake composes - so the measured source stats and
 * the applied grade agree, and V-Log is never analysed as raw log.
 */
#pragma once
#ifndef CG_EDITOR_ANALYSIS_H
#define CG_EDITOR_ANALYSIS_H

#include <cstdint>
#include <vector>

namespace cg {
namespace editor {

// One render time, carried in AE's rational form (A_Time value/scale) so equality is
// exact and no float time arithmetic enters the schedule.
struct SampleTime {
    int64_t  value = 0;
    uint32_t scale = 1;

    bool operator==(const SampleTime& o) const { return value == o.value && scale == o.scale; }
};

// Evenly-spaced sample times across a layer's [inPoint, inPoint+duration) span, one per
// requested sample, each placed at the CENTRE of its sub-interval: t_i = inPoint +
// round((i+0.5)/N * duration). Centre placement avoids sampling exactly the in-point (a
// hold frame / slate) and the exclusive out-point. All arithmetic is integer on the
// shared time scale; `scale` is carried through unchanged. Degenerate inputs collapse
// gracefully: numSamples<=1 or duration<=0 yields a single sample at inPoint.
inline std::vector<SampleTime> frameSampleSchedule(int64_t inPointValue, int64_t durationValue,
                                                   uint32_t scale, int numSamples) {
    std::vector<SampleTime> out;
    if (scale == 0) scale = 1;
    if (numSamples < 1) numSamples = 1;
    if (numSamples == 1 || durationValue <= 0) {
        out.push_back(SampleTime{inPointValue, scale});
        return out;
    }
    out.reserve(static_cast<size_t>(numSamples));
    for (int i = 0; i < numSamples; ++i) {
        // offset = round(((2i+1) * duration) / (2N)), integer round-half-up (values >= 0).
        const int64_t num = (static_cast<int64_t>(2 * i + 1)) * durationValue;
        const int64_t den = static_cast<int64_t>(2 * numSamples);
        const int64_t offset = (num + den / 2) / den;
        out.push_back(SampleTime{inPointValue + offset, scale});
    }
    return out;
}

// Identifies WHAT is being analysed: the footage decode (footage-profile popup) plus the
// clip's time span. Two requests with the same fingerprint measure identical source
// stats, so a completed analysis can be cached and reused (re-selecting a previously
// analysed footage profile is instant). It deliberately does NOT include the grade knobs
// (theme / strength / skin / chroma): those change the LUT, not the measured source, so a
// knob drag must never trigger re-analysis.
struct AnalysisFingerprint {
    int32_t  footageProfile = 0;   // CG_FOOT_* (decode changes the measured stats)
    int64_t  inPointValue = 0;     // layer span, so a trim / different clip re-analyses
    int64_t  durationValue = 0;
    uint32_t scale = 1;

    bool operator==(const AnalysisFingerprint& o) const {
        return footageProfile == o.footageProfile && inPointValue == o.inPointValue &&
               durationValue == o.durationValue && scale == o.scale;
    }
    bool operator!=(const AnalysisFingerprint& o) const { return !(*this == o); }
    // A fingerprint is only usable once its span is known (layer timing resolved).
    bool valid() const { return footageProfile != 0; }
};

// --- slider-drag debounce ---------------------------------------------------
//
// An analysis-relevant change (footage profile) must not kick off a multi-frame checkout
// on every intermediate value while the user is mid-interaction. This tracks the last
// tick a NEW target fingerprint was observed and only greenlights a run once it has been
// stable for `debounceTicks`. `nowTick` is the idle-hook tick counter (monotonic).
//
// A never-analysed instance (no accepted fingerprint yet) still debounces, so the very
// first analysis waits one debounce window after the window opens - cheap, and it lets the
// layer timing settle before the first checkout.
class AnalysisDebounce {
public:
    explicit AnalysisDebounce(uint64_t debounceTicks) : debounce_(debounceTicks) {}

    // Observe the current desired fingerprint at `nowTick`. Returns true exactly on the
    // tick the target both differs from what was last analysed AND has been stable for the
    // debounce window - i.e. "start the analysis now". Call once per tick.
    bool observe(const AnalysisFingerprint& desired, uint64_t nowTick) {
        if (!desired.valid()) return false;
        if (hasAccepted_ && accepted_ == desired) return false;  // already analysed this
        if (!hasPending_ || pending_ != desired) {
            pending_ = desired;
            hasPending_ = true;
            sinceTick_ = nowTick;
            return false;  // just (re)started the timer
        }
        // Same pending target as before: has it been stable long enough?
        if (nowTick - sinceTick_ >= debounce_) {
            accepted_ = desired;
            hasAccepted_ = true;
            hasPending_ = false;
            return true;
        }
        return false;
    }

    // Mark `fp` as the accepted/analysed target without waiting (e.g. served from cache).
    void accept(const AnalysisFingerprint& fp) {
        accepted_ = fp;
        hasAccepted_ = true;
        hasPending_ = false;
    }

    bool hasAccepted() const { return hasAccepted_; }
    const AnalysisFingerprint& accepted() const { return accepted_; }

private:
    uint64_t          debounce_;
    AnalysisFingerprint pending_{};
    AnalysisFingerprint accepted_{};
    uint64_t          sinceTick_ = 0;
    bool              hasPending_ = false;
    bool              hasAccepted_ = false;
};

// --- incremental multi-frame job -------------------------------------------
//
// Renders the schedule ONE frame per idle tick, accumulating decoded RGB into a single
// buffer, then finalises with one computeStats over the union. Pure: the caller drives
// nextSample() -> (AE checkout+decode) -> addFrame(); this owns only the schedule cursor
// and the aggregate buffer, so the scheduling / aggregation / completion logic is tested
// without AE. Per-frame pixels are decimated by the caller to stay within a pixel budget;
// this header just concatenates what it is given.

enum class AnalysisState {
    Idle,      // no job
    Sampling,  // frames still outstanding
    Ready,     // all frames accumulated; caller runs computeStats then reset()
};

class AnalysisJob {
public:
    AnalysisJob() = default;

    // Begin a job over `schedule` for `fp`. Clears any prior aggregate.
    void begin(const AnalysisFingerprint& fp, std::vector<SampleTime> schedule) {
        fingerprint_ = fp;
        schedule_ = std::move(schedule);
        cursor_ = 0;
        pixels_.clear();
        state_ = schedule_.empty() ? AnalysisState::Idle : AnalysisState::Sampling;
    }

    AnalysisState state() const { return state_; }
    const AnalysisFingerprint& fingerprint() const { return fingerprint_; }

    // The next time to render, or false if none outstanding.
    bool nextSample(SampleTime& out) const {
        if (state_ != AnalysisState::Sampling || cursor_ >= schedule_.size()) return false;
        out = schedule_[cursor_];
        return true;
    }

    // Append one decoded frame's interleaved RGB floats (length a multiple of 3). Advances
    // the cursor; when the last scheduled frame is in, transitions to Ready. A failed
    // checkout should still call advanceSkip() so the job can't wedge.
    void addFrame(const float* rgb, size_t count) {
        if (state_ != AnalysisState::Sampling) return;
        pixels_.insert(pixels_.end(), rgb, rgb + count);
        advance();
    }

    // A scheduled frame could not be rendered: skip it without adding pixels. Keeps the
    // job progressing so one bad checkout never strands the analysis mid-run.
    void advanceSkip() {
        if (state_ != AnalysisState::Sampling) return;
        advance();
    }

    // The accumulated decoded RGB (valid when Ready). computeStats runs over this.
    const std::vector<float>& pixels() const { return pixels_; }
    size_t sampledFrames() const { return cursor_; }
    size_t totalFrames() const { return schedule_.size(); }

    void reset() {
        state_ = AnalysisState::Idle;
        schedule_.clear();
        pixels_.clear();
        cursor_ = 0;
    }

private:
    void advance() {
        ++cursor_;
        if (cursor_ >= schedule_.size()) {
            // Ready only if we actually gathered pixels; an all-skipped run yields nothing
            // to measure, so fall back to Idle (caller keeps the prior/placeholder stats).
            state_ = pixels_.empty() ? AnalysisState::Idle : AnalysisState::Ready;
        }
    }

    AnalysisState           state_ = AnalysisState::Idle;
    AnalysisFingerprint     fingerprint_{};
    std::vector<SampleTime> schedule_;
    size_t                  cursor_ = 0;
    std::vector<float>      pixels_;
};

// Per-frame pixel budget: total analysis pixels / number of sampled frames, floored at 1.
// The caller decimates each checked-out frame to at most this many pixels so the aggregate
// computeStats stays bounded regardless of clip resolution.
inline int perFramePixelBudget(int totalPixelBudget, int numSamples) {
    if (numSamples < 1) numSamples = 1;
    if (totalPixelBudget < numSamples) return 1;
    return totalPixelBudget / numSamples;
}

// Nearest-neighbour stride so a `w`x`h` frame decimates to <= `budget` pixels. Always >=1.
// The decimated count is ceil(w/step)*ceil(h/step); step grows until that is <= budget.
inline int decimationStride(int w, int h, int budget) {
    if (w <= 0 || h <= 0 || budget < 1) return 1;
    int step = 1;
    for (;;) {
        const int ow = (w + step - 1) / step;
        const int oh = (h + step - 1) / step;
        if (static_cast<int64_t>(ow) * oh <= budget) return step;
        ++step;
        if (step > w && step > h) return step;  // safety: can't exceed 1x1
    }
}

}  // namespace editor
}  // namespace cg

#endif  // CG_EDITOR_ANALYSIS_H
