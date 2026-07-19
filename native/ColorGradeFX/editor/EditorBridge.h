/*
 * EditorBridge.h - the pure, host-agnostic seam between the native editor window
 * and the effect instance. NO AE SDK, NO Win32, NO ImGui here on purpose: this is
 * the part of the bridge that is exercised headlessly by native:editor-test
 * (tests/editor/bridge_test.cpp), so its logic is proven without a running AE.
 *
 * The bridge is a one-instance-per-effect channel with two directions:
 *   - effect  -> window: a ParamSnapshot (the effect's current param values),
 *     published whenever the effect renders / a param changes, so the window's
 *     controls always reflect Effect Controls.
 *   - window -> effect: a thread-safe EditQueue of ParamEdits, pushed on the UI
 *     thread when the user drags a control, drained on AE's main thread (by the
 *     companion-AEGP idle hook, see adr-editor-ui.md) and written back into the
 *     effect's params/arb-data via the AEGP StreamSuite inside one undo group.
 *
 * Values are carried as normalized fractions (0..1) for the sliders and as 1-based
 * popup indices for Theme / LUT-Source, matching the effect's PF_ params. The UI
 * displays percents; the percent<->fraction mapping lives here so both the window
 * and the (headless) test agree on it.
 */
#pragma once
#ifndef CG_EDITOR_BRIDGE_H
#define CG_EDITOR_BRIDGE_H

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace cg {
namespace editor {

// Manual primary correction (Phase 6a) - the recipe-backed editor state. Mirrors
// the 9 recipe-only manual fields (RecipeData.manual*); exposure, temperature, and
// lookMix are separate keyframeable PF params (see EditField below), not carried here.
// Values are in engine units (the same the ManualGrade engine struct uses), so the
// bridge stays a straight pass-through and this header needs no percent mapping.
struct ManualState {
    double contrast = 0.0;      // -100..100
    double pivot = 0.435;       // gamma-709
    double highlights = 0.0;    // -100..100
    double shadows = 0.0;       // -100..100
    double whites = 0.0;        // -100..100
    double blacks = 0.0;        // -100..100
    double tint = 0.0;          // -100..100
    double saturation = 1.0;    // 0..2
    double vibrance = 0.0;      // -100..100

    bool operator==(const ManualState& o) const {
        return contrast == o.contrast && pivot == o.pivot && highlights == o.highlights &&
               shadows == o.shadows && whites == o.whites && blacks == o.blacks &&
               tint == o.tint && saturation == o.saturation && vibrance == o.vibrance;
    }
    bool operator!=(const ManualState& o) const { return !(*this == o); }
};

// Curves editor state (Phase 6b). Four independent monotone curves (master + R/G/B),
// each up to CG_EDIT_MAX_CURVE_POINTS control points in x-ascending order; count 0 =
// absent (identity). Points are in gamma-Rec.709 [0,1]x[0,1], matching the recipe's
// CurveData and the engine's authored curves - a straight pass-through, no mapping.
constexpr int CG_EDIT_MAX_CURVE_POINTS = 16;

struct CurveState {
    int    count = 0;
    double x[CG_EDIT_MAX_CURVE_POINTS] = {};
    double y[CG_EDIT_MAX_CURVE_POINTS] = {};

    bool operator==(const CurveState& o) const {
        if (count != o.count) return false;
        for (int i = 0; i < count; ++i)
            if (x[i] != o.x[i] || y[i] != o.y[i]) return false;
        return true;
    }
    bool operator!=(const CurveState& o) const { return !(*this == o); }
};

struct CurvesState {
    CurveState master, r, g, b;

    bool operator==(const CurvesState& o) const {
        return master == o.master && r == o.r && g == o.g && b == o.b;
    }
    bool operator!=(const CurvesState& o) const { return !(*this == o); }
};

// Wheels editor state (Phase 6c). The DaVinci Lift/Gamma/Gain triples are the primary
// face; the Adobe 3-way secondary mode reuses the same lift/gamma/gain masters for its
// per-band luminance and the recipe's shadow/mid/highlight LAB tint fields for its color
// discs (so it needs no new engine math). Values are in engine units (straight pass-through).
struct WheelsState {
    // DaVinci Lift/Gamma/Gain (per-channel). Neutral: lift 0, gamma 1, gain 1.
    double lift[3]  = {0.0, 0.0, 0.0};
    double gamma[3] = {1.0, 1.0, 1.0};
    double gain[3]  = {1.0, 1.0, 1.0};
    // Adobe 3-way band color discs -> LAB [a,b] tints. hasX mirrors the recipe's
    // presence flags so an unset band round-trips as "no tint".
    bool hasShadowTint = false;    double shadowTint[2] = {0.0, 0.0};
    bool hasMidTint    = false;    double midTint[2]    = {0.0, 0.0};
    bool hasHighTint   = false;    double highTint[2]   = {0.0, 0.0};
    // Which face the editor last showed: 0 = Lift/Gamma/Gain, 1 = Adobe 3-way. UI only.
    int  mode = 0;

    bool operator==(const WheelsState& o) const {
        for (int c = 0; c < 3; ++c)
            if (lift[c] != o.lift[c] || gamma[c] != o.gamma[c] || gain[c] != o.gain[c]) return false;
        return hasShadowTint == o.hasShadowTint && shadowTint[0] == o.shadowTint[0] &&
               shadowTint[1] == o.shadowTint[1] && hasMidTint == o.hasMidTint &&
               midTint[0] == o.midTint[0] && midTint[1] == o.midTint[1] &&
               hasHighTint == o.hasHighTint && highTint[0] == o.highTint[0] &&
               highTint[1] == o.highTint[1] && mode == o.mode;
    }
    bool operator!=(const WheelsState& o) const { return !(*this == o); }
};

// Which effect param an edit targets. Kept independent of the effect's CG_* enum
// so this header stays free of ColorGrade.h (host coupling lives in EditorWindow).
enum class EditField : int {
    FootageProfile = 0,  // Correct: 1-based popup index (CG_FOOT_*)
    Theme,               // 1-based popup index (CG_THEME_*)
    Strength,            // fraction 0..1
    SkinProtection,      // fraction 0..1
    ChromaGain,          // fraction; 1.0 == 100% (theme's authored gain)
    LutSource,           // 1-based popup index (CG_SRC_*)
    // Phase 6a. Exposure/LookMix/Temperature are keyframeable PF scalar params;
    // Manual carries the whole recipe-backed ManualState (written to the arb recipe).
    Exposure,            // stops (engine units), typically -5..5
    LookMix,             // fraction 0..1
    Temperature,         // -100..100 (engine units)
    Manual,              // payload = ParamEdit::manual (the 9 recipe-backed controls)
    // Phase 6b/6c. Curves and Wheels are recipe-backed like Manual: the whole state
    // is carried as one payload and written to the CG_RECIPE arb (idle-hook rmw).
    Curves,              // payload = ParamEdit::curves (master + R/G/B curves)
    Wheels,              // payload = ParamEdit::wheels (LGG triples + 3-way tints)
};

// True for fields whose value lives in the CG_RECIPE arb blob (Manual/Curves/Wheels),
// as opposed to a scalar/popup PF stream. The effect drains these through the arb
// read-modify-write path instead of StreamForEdit.
inline bool isRecipeBackedField(EditField f) {
    return f == EditField::Manual || f == EditField::Curves || f == EditField::Wheels;
}

// A single control edit produced by the window, consumed by the effect side.
struct ParamEdit {
    EditField   field;
    double      value = 0.0;   // scalar edits: slider value / popup index (as a double)
    ManualState manual;        // used only when field == Manual
    CurvesState curves;        // used only when field == Curves
    WheelsState wheels;        // used only when field == Wheels
};

// The effect's current param values, published to the window for display.
struct ParamSnapshot {
    int    footageProfile = 1;  // Correct: 1-based popup index (1 = Rec.709)
    int    theme = 1;           // 1-based popup index
    double strength = 0.8;      // fraction 0..1
    double skinProtection = 0.75;
    double chromaGain = 1.0;    // fraction; 1.0 == 100%
    int    lutSource = 1;       // 1-based popup index
    // Phase 6a: the three keyframeable PF scalar params + the recipe-backed manual
    // block. `recipeHash` is a cheap fingerprint of the whole recipe blob (folded
    // into the preview cache key so a manual edit never serves a stale frame).
    double exposure = 0.0;      // stops
    double lookMix = 1.0;       // fraction 0..1
    double temperature = 0.0;   // -100..100
    ManualState manual;         // the 9 recipe-backed manual controls
    CurvesState curves;         // Phase 6b: master + R/G/B curves (recipe-backed)
    WheelsState wheels;         // Phase 6c: LGG triples + 3-way tints (recipe-backed)
    uint64_t recipeHash = 0;
    // A monotonically increasing stamp so the window can tell a genuinely newer
    // snapshot from a stale re-publish and avoid clobbering an in-flight drag.
    uint64_t revision = 0;
};

// Effect -> window: the state of the in-effect multi-frame analysis (Phase 5), so the
// window can show an "Analyzing 3/8..." indicator and an analyzed/stale badge. Published
// by the idle-hook analysis driver; purely informational (no control flow depends on it).
struct AnalysisStatus {
    enum class State : int { Idle = 0, Sampling = 1, Analyzed = 2 };
    State state = State::Idle;
    int   sampled = 0;   // frames checked out so far (Sampling)
    int   total = 0;     // frames in the current schedule
    bool  fromCache = false;  // the analyzed stats were served from the results cache
};

// --- percent <-> fraction (the UI shows percents; params are fractions) -----

inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

inline double clampRange(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline double fractionToPercent(double f) { return f * 100.0; }
inline double percentToFraction(double p) { return p / 100.0; }

// Chroma gain slider runs 0..300% about a 100% (=1.0) default; keep it in that band.
inline double clampChromaFraction(double f) { return clampRange(f, 0.0, 3.0); }

// --- window -> effect edit queue (thread-safe) ------------------------------
//
// Pushed from the window's UI thread, drained on AE's main thread. Coalescing keeps
// only the latest value per field so a fast drag doesn't flood the drain with an
// undo step per pixel of travel - the drain applies one write per changed field.

class EditQueue {
public:
    void push(const ParamEdit& e) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& existing : pending_) {
            if (existing.field == e.field) {
                existing = e;  // coalesce: latest edit wins (incl. the Manual payload)
                return;
            }
        }
        pending_.push_back(e);
    }

    // Move all pending edits out for application. FIFO across distinct fields.
    std::vector<ParamEdit> drain() {
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<ParamEdit> out(pending_.begin(), pending_.end());
        pending_.clear();
        return out;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return pending_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return pending_.size();
    }

private:
    mutable std::mutex     mutex_;
    std::deque<ParamEdit>  pending_;
};

// Apply one drained edit to a snapshot in memory. This is the pure model of what
// the AEGP StreamSuite write does to the effect's params, so the headless test can
// assert "an edit round-trips back into the visible param state" without AE.
inline void applyEdit(ParamSnapshot& s, const ParamEdit& e) {
    switch (e.field) {
        case EditField::FootageProfile: s.footageProfile = static_cast<int>(e.value + 0.5); break;
        case EditField::Theme:          s.theme = static_cast<int>(e.value + 0.5); break;
        case EditField::Strength:       s.strength = clamp01(e.value); break;
        case EditField::SkinProtection: s.skinProtection = clamp01(e.value); break;
        case EditField::ChromaGain:     s.chromaGain = clampChromaFraction(e.value); break;
        case EditField::LutSource:      s.lutSource = static_cast<int>(e.value + 0.5); break;
        case EditField::Exposure:       s.exposure = e.value; break;
        case EditField::LookMix:        s.lookMix = clamp01(e.value); break;
        case EditField::Temperature:    s.temperature = e.value; break;
        case EditField::Manual:         s.manual = e.manual; break;
        case EditField::Curves:         s.curves = e.curves; break;
        case EditField::Wheels:         s.wheels = e.wheels; break;
    }
}

}  // namespace editor
}  // namespace cg

#endif  // CG_EDITOR_BRIDGE_H
