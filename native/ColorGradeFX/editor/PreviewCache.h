/*
 * PreviewCache.h - the pure, host-agnostic core of the Phase 4 live clip preview.
 * NO AE SDK, NO Win32, NO ImGui, NO D3D here on purpose: everything in this header is
 * exercised headlessly by native:preview-test (native/tests/editor/preview_test.cpp),
 * so the cache keying/eviction, the scrub/param-change decision, the letterbox fit
 * math, and the checkout/checkin "always check in" guarantee are all proven WITHOUT a
 * running After Effects.
 *
 * The AE-facing parts (rendering the layer frame via AEGP_RenderAndCheckoutLayerFrame,
 * uploading the pixels to a D3D11 texture) live in ColorGrade.cpp / EditorWindow.cpp
 * and are captain-verified; they lean on the primitives here so the risky logic is the
 * tested logic.
 *
 * Flow: the effect's AEGP idle hook (main thread) computes the current PreviewKey
 * (frame time + a fingerprint of the grade-affecting params). decidePreviewAction()
 * says whether the window is already up to date, whether a cached CPU frame can be
 * served cheaply (the interactive-scrub path), or whether AE must render a fresh frame.
 * A fresh render's pixels are copied into a PreviewFrame, the AE frame receipt is
 * checked back in (ScopedCheckin, unconditionally), the frame is stored in the bounded
 * LRU cache and published to the window, which uploads it to a texture and draws it
 * letterboxed in the center.
 */
#pragma once
#ifndef CG_EDITOR_PREVIEW_CACHE_H
#define CG_EDITOR_PREVIEW_CACHE_H

#include <cstdint>
#include <list>
#include <memory>
#include <utility>
#include <vector>

namespace cg {
namespace editor {

// A decoded preview frame: 8-bit RGBA, tightly packed (rowStride == width*4), the
// output of the effect's own decode+grade (we render the layer downstream of the
// effect, so V-Log is already decoded - the pipeline invariant holds by construction).
struct PreviewFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // width*height*4, byte order R,G,B,A

    bool valid() const {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    }
};

// Identifies a unique preview render: the frame time plus a fingerprint of every param
// that changes the graded pixels. Two requests with the same key produce identical
// pixels, so a cache hit is safe to serve. Time is carried in AE's rational form
// (A_Time value/scale) so equality is exact - no float time comparison.
struct PreviewKey {
    int64_t  timeValue = 0;   // A_Time.value
    uint32_t timeScale = 1;   // A_Time.scale (never 0 for a real key)
    uint64_t paramFingerprint = 0;

    bool operator==(const PreviewKey& o) const {
        return timeValue == o.timeValue && timeScale == o.timeScale &&
               paramFingerprint == o.paramFingerprint;
    }
    bool operator!=(const PreviewKey& o) const { return !(*this == o); }
};

// Fold the grade-affecting params into a stable 64-bit fingerprint (FNV-1a). Any change
// that changes the rendered pixels must change this: the popups (footage/theme/lut
// source) go in as ints; the three sliders are quantized to 1e-4 before mixing so float
// noise never churns the cache but a real drag always moves the fingerprint.
inline uint64_t fnv1a(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v & 0xff);
        h *= 1099511628211ull;
        v >>= 8;
    }
    return h;
}

inline int64_t quantize(double v, double step = 1e-4) {
    // Round-half-away-from-zero so +/- of the same magnitude map symmetrically.
    double s = v / step;
    return static_cast<int64_t>(s < 0 ? s - 0.5 : s + 0.5);
}

// Phase 6a: the manual grade lives in the recipe (folded in as `recipeHash`, a hash
// of the whole recipe blob) plus the three keyframeable scalar params (exposure /
// lookMix / temperature). Any manual edit changes the recipe hash or a scalar, so the
// preview cache never serves a stale frame after a manual change.
inline uint64_t previewParamFingerprint(int footage, int theme, int lutSource,
                                        double strength, double skin, double chroma,
                                        double exposure = 0.0, double lookMix = 1.0,
                                        double temperature = 0.0, uint64_t recipeHash = 0) {
    uint64_t h = 1469598103934665603ull;  // FNV offset basis
    h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(footage)));
    h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(theme)));
    h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(lutSource)));
    h = fnv1a(h, static_cast<uint64_t>(quantize(strength)));
    h = fnv1a(h, static_cast<uint64_t>(quantize(skin)));
    h = fnv1a(h, static_cast<uint64_t>(quantize(chroma)));
    h = fnv1a(h, static_cast<uint64_t>(quantize(exposure)));
    h = fnv1a(h, static_cast<uint64_t>(quantize(lookMix)));
    h = fnv1a(h, static_cast<uint64_t>(quantize(temperature)));
    h = fnv1a(h, recipeHash);
    return h;
}

// --- LRU cache of CPU preview frames ----------------------------------------
//
// Bounded so a long scrub can't grow memory without limit; the most-recently-served
// key is kept, the least-recently-used evicted. Capacities are small (a handful of
// frames), so a linear list scan is cheaper and simpler than a hash + intrusive list.
// Values are shared_ptr<const PreviewFrame> so the window can hold a frame for display
// while the cache independently evicts it.
class PreviewCache {
public:
    using ValuePtr = std::shared_ptr<const PreviewFrame>;

    explicit PreviewCache(size_t capacity) : cap_(capacity ? capacity : 1) {}

    // Return the frame for `key` if present, moving it to most-recently-used. Null if
    // absent. A hit is the interactive-scrub fast path (no AE render).
    ValuePtr get(const PreviewKey& key) {
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->first == key) {
                items_.splice(items_.begin(), items_, it);  // move to front (MRU)
                return items_.front().second;
            }
        }
        return nullptr;
    }

    bool contains(const PreviewKey& key) const {
        for (const auto& kv : items_)
            if (kv.first == key) return true;
        return false;
    }

    // Insert/replace `key` as most-recently-used, evicting the LRU entry if over cap.
    void put(const PreviewKey& key, ValuePtr value) {
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->first == key) {
                it->second = std::move(value);
                items_.splice(items_.begin(), items_, it);
                return;
            }
        }
        items_.emplace_front(key, std::move(value));
        while (items_.size() > cap_) items_.pop_back();  // drop LRU
    }

    void clear() { items_.clear(); }
    size_t size() const { return items_.size(); }
    size_t capacity() const { return cap_; }

private:
    size_t cap_;
    // front == most-recently-used, back == least-recently-used.
    std::list<std::pair<PreviewKey, ValuePtr>> items_;
};

// --- the scrub/param-change decision (pure) ---------------------------------

enum class PreviewAction {
    UpToDate,     // the window already shows `desired`; render nothing
    ServeCached,  // a cached CPU frame matches `desired`; publish it (no AE render)
    Render,       // no match anywhere; AE must render a fresh frame
};

// What should the driver do to bring the window to `desired`? Pure so the interactive
// path (scrub back onto a visited time -> ServeCached; nudge a knob -> Render) is unit
// tested without AE. `hasLast`/`lastKey` = the key last published to this window.
inline PreviewAction decidePreviewAction(bool hasLast, const PreviewKey& lastKey,
                                         const PreviewKey& desired, bool cacheHasDesired) {
    if (hasLast && lastKey == desired) return PreviewAction::UpToDate;
    if (cacheHasDesired) return PreviewAction::ServeCached;
    return PreviewAction::Render;
}

// --- letterbox fit (pure) ---------------------------------------------------

struct FitRect {
    float x = 0, y = 0, w = 0, h = 0;
};

// Center a frameW x frameH image inside an availW x availH box, preserving aspect
// (letterbox / pillarbox). Returns the destination rect (offset + size) within the box.
// Degenerate inputs (non-positive avail or frame) yield an empty rect at the origin.
inline FitRect letterboxFit(float availW, float availH, int frameW, int frameH) {
    FitRect r;
    if (availW <= 0.0f || availH <= 0.0f || frameW <= 0 || frameH <= 0) return r;
    const float frameAspect = static_cast<float>(frameW) / static_cast<float>(frameH);
    const float availAspect = availW / availH;
    if (frameAspect >= availAspect) {
        // Frame is wider than the box: fit width, letterbox top/bottom.
        r.w = availW;
        r.h = availW / frameAspect;
    } else {
        // Frame is taller: fit height, pillarbox left/right.
        r.h = availH;
        r.w = availH * frameAspect;
    }
    r.x = (availW - r.w) * 0.5f;
    r.y = (availH - r.h) * 0.5f;
    return r;
}

// --- before/after compare view (pure geometry) ------------------------------
//
// The editor can compare the ORIGINAL (decoded footage - never raw log, the "before")
// against the GRADED output (the "after"). Both frames share the clip's dimensions, so
// they letterbox to the same dst rect; the mode decides what is shown where.

enum class CompareMode {
    AfterOnly = 0,  // just the graded preview (default)
    BeforeOnly,     // just the decoded original
    Split,          // before on the left of the divider, after on the right
};

// Where to draw the frame(s) inside an availW x availH box, plus the split divider's x.
// Both before and after use `dst` (identical size -> identical letterbox); in Split mode
// the caller clips before to [dst.x, splitX] and after to [splitX, dst.x+dst.w].
struct SplitGeometry {
    FitRect dst;             // letterboxed frame rect within the box
    float   splitX = 0.0f;   // absolute x of the divider (box coords), within [dst.x, dst.x+dst.w]
};

// Compute the compare-view geometry. `splitFraction` (0..1) positions the divider across
// the letterboxed frame width; it is ignored except in Split mode but always clamped so a
// caller can pass it unconditionally. Degenerate inputs yield an empty dst at the origin.
inline SplitGeometry splitViewGeometry(float availW, float availH, int frameW, int frameH,
                                       float splitFraction) {
    SplitGeometry g;
    g.dst = letterboxFit(availW, availH, frameW, frameH);
    float f = splitFraction < 0.0f ? 0.0f : (splitFraction > 1.0f ? 1.0f : splitFraction);
    g.splitX = g.dst.x + g.dst.w * f;
    return g;
}

// --- checkout/checkin state machine (pure guarantee) ------------------------
//
// A frame checked out from AE (AEGP_RenderAndCheckoutLayerFrame) MUST be checked back
// in (AEGP_CheckinFrame) exactly once, or AE's frame cache leaks and destabilizes. This
// RAII guard makes "check in" unconditional: it fires on every scope exit, including an
// exception thrown while copying pixels out of the receipt world. Modeled here (pure,
// templated on the checkin callable) so native:preview-test proves the once-and-always
// guarantee cross-platform; the AE code instantiates it with the real AEGP_CheckinFrame.
template <typename CheckinFn>
class ScopedCheckin {
public:
    explicit ScopedCheckin(CheckinFn fn) : fn_(std::move(fn)) {}
    ~ScopedCheckin() {
        if (armed_) fn_();
    }
    // Movable (so makeScopedCheckin can return by value) but not copyable: the moved-from
    // guard is disarmed so the check-in fires exactly once.
    ScopedCheckin(ScopedCheckin&& o) noexcept : fn_(std::move(o.fn_)), armed_(o.armed_) {
        o.armed_ = false;
    }
    // Only if ownership of the check-in is deliberately handed elsewhere. The preview
    // path never disarms - it always checks the frame straight back in.
    void disarm() { armed_ = false; }

    ScopedCheckin(const ScopedCheckin&) = delete;
    ScopedCheckin& operator=(const ScopedCheckin&) = delete;
    ScopedCheckin& operator=(ScopedCheckin&&) = delete;

private:
    CheckinFn fn_;
    bool armed_ = true;
};

template <typename CheckinFn>
ScopedCheckin<CheckinFn> makeScopedCheckin(CheckinFn fn) {
    return ScopedCheckin<CheckinFn>(std::move(fn));
}

}  // namespace editor
}  // namespace cg

#endif  // CG_EDITOR_PREVIEW_CACHE_H
