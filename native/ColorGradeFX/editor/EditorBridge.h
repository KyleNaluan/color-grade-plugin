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

// Which effect param an edit targets. Kept independent of the effect's CG_* enum
// so this header stays free of ColorGrade.h (host coupling lives in EditorWindow).
enum class EditField : int {
    FootageProfile = 0,  // Correct: 1-based popup index (CG_FOOT_*)
    Theme,               // 1-based popup index (CG_THEME_*)
    Strength,            // fraction 0..1
    SkinProtection,      // fraction 0..1
    ChromaGain,          // fraction; 1.0 == 100% (theme's authored gain)
    LutSource,           // 1-based popup index (CG_SRC_*)
};

// A single control edit produced by the window, consumed by the effect side.
struct ParamEdit {
    EditField field;
    double    value;  // fraction for sliders; the popup index (as a double) for popups
};

// The effect's current param values, published to the window for display.
struct ParamSnapshot {
    int    footageProfile = 1;  // Correct: 1-based popup index (1 = Rec.709)
    int    theme = 1;           // 1-based popup index
    double strength = 0.8;      // fraction 0..1
    double skinProtection = 0.75;
    double chromaGain = 1.0;    // fraction; 1.0 == 100%
    int    lutSource = 1;       // 1-based popup index
    // A monotonically increasing stamp so the window can tell a genuinely newer
    // snapshot from a stale re-publish and avoid clobbering an in-flight drag.
    uint64_t revision = 0;
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
                existing.value = e.value;  // coalesce: latest value wins
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
    }
}

}  // namespace editor
}  // namespace cg

#endif  // CG_EDITOR_BRIDGE_H
