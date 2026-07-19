/*
 * ColorGrade.cpp - native Color Grade effect: a SmartFX LUT-apply effect.
 *
 * Registers in AE 2025, drags onto a layer, and applies a baked 3D LUT to pixels
 * through the ported trilinear sampleLut (lut/CubeLut.h). CPU Smart Render is the
 * mandatory path; the DirectX GPU path (HAS_HLSL) accelerates it - see ColorGradeGPU.inc.
 *
 * LUT source (param):
 *   - "Auto (Theme + Analysis)" bakes the grade LUT in-effect from the ported
 *     engine: buildTransform(recipe source stats, selected theme + chroma-gain
 *     override, {strength, skinProtection sliders}) baked at CG_GRADE_LUT_SIZE.
 *     The grade recipe (measured stats + the full typed knob space) is persisted
 *     as an arb-data param (see the PF_Cmd_ARBITRARY_CALLBACK handler); AE stores
 *     it in the project. Analysis that populates real footage stats is a later
 *     phase - the default recipe seeds neutral placeholder stats.
 *   - "Embedded (Teal-Orange)" uses the compiled-in default LUT.
 *   - "External .cube file" loads from env CG_LUT_PATH, else <pluginDir>/ColorGrade_LUT.cube,
 *     falling back to the embedded LUT on any failure (so render never fails for a bad path).
 * The Auto path bakes strength into the LUT (post-LUT blend = 1.0); the embedded/
 * external paths blend by the strength slider, preserving Phase 1 behaviour.
 */

// cuda_runtime.h must precede ColorGrade.h: it defines MAJOR_VERSION/MINOR_VERSION macros
// that would clash with ours, so undef them here (mirrors the SDK's SDK_Invert_ProcAmp).
// HAS_CUDA is a project define (CG_GPU=true); undefined -> this #if is 0 and skips.
#if HAS_CUDA
#include <cuda_runtime.h>
#undef MAJOR_VERSION
#undef MINOR_VERSION
#endif

#include "ColorGrade.h"
#include "editor/Analysis.h"  // Phase 5: pure multi-frame analysis schedule/job/debounce

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <cstring>
#include <cstddef>
#include <new>
#include <vector>

// Debug-only path tracer: prints to the debugger / DebugView so the active render
// path (CPU vs DirectX GPU) can be confirmed WITHOUT attaching a debugger/breakpoint.
// See native/BUILDING.md "Verifying the GPU path". No-op in Release.
#if defined(_DEBUG) && defined(AE_OS_WIN)
#define CG_DBG(msg) ::OutputDebugStringA("[ColorGradeFX] " msg "\n")
#else
#define CG_DBG(msg) ((void)0)
#endif

/* ================= AEGP bridge globals + stable instance key ============== */
//
// The editor window <-> effect bridge (Phase 3). Two things need AEGP:
//   1. A STABLE per-instance key. in_data->effect_ref is NOT stable across command
//      types, so keying the window registry on it made the effect->window publish
//      (and the window->effect drain) target the wrong/no window - the live-AE
//      round-trip was dead. The key is now a uid stored in the effect's sequence
//      data, consistent across every command for one instance.
//   2. A main-thread DRIVER for window-originated writes. An effect gets no periodic
//      callback on its own, so a window edit had nothing to apply it. We register a
//      global AEGP idle hook (via PICA - no separate AEGP plugin) that, on AE's main
//      thread, drains each open window's edits and writes them onto the effect's
//      param streams (AEGP_SetStreamValue, inside one undo group) so AE re-renders and
//      Effect Controls updates. The hook cheap-noops when nothing is pending, so it
//      never burdens AE.
//
// All AEGP use here is captain-verified in AE (unautomatable from WSL); every call is
// guarded and failures are swallowed so a bridge hiccup never destabilizes rendering.

// AEGP_SuiteHandler requires the client to define this (it must throw, never return).
// All our AEGP use is wrapped in catch(...), so a missing suite degrades gracefully to
// the on-param-change drain instead of crashing.
void AEGP_SuiteHandler::MissingSuiteError() const {
    A_THROW(A_Err_MISSING_SUITE);
}

static SPBasicSuite*    g_spbasic = nullptr;   // cached PICA basic suite (from in_data)
static AEGP_PluginID    g_aegpId = 0;          // our AEGP id (for stream/undo calls)
static bool             g_idleHookRegistered = false;

// Per-instance effect CONTEXT, captured on the main thread (button-open): the parent
// layer + our installed-effect key. The idle hook does NOT keep a persistent
// AEGP_EffectRefH - a captured ref dangles the moment the user deletes the effect, and
// ANY stream call on that stale ref (even indexing into its stream group) makes AE raise
// an uncatchable modal + crash. Instead the hook re-enumerates the *live* layer's effects
// each tick (AEGP_GetLayerEffectByIndex on a live layer is always safe) and matches ours
// by installed key, so every stream/render call runs on a fresh, valid handle. When no
// matching effect remains on the layer, the effect was deleted -> close the window.
// AEGP_LayerH from AEGP_GetEffectLayer is not owned (no dispose); it stays valid while the
// layer exists (which outlives an effect deletion).
static std::mutex g_effMutex;
// Only STABLE identities are cached - NO AEGP handles. A deleted comp OR layer zombies in
// AE's undo buffer, where a cached AEGP_CompH/AEGP_LayerH keeps resolving; identities do
// not. Each idle tick re-resolves the current comp (by item id, via the live project item
// list) and the current layer (by layer id, within that comp), so liveness is genuine
// membership and every handle used is freshly derived (robust across delete+undo of the
// effect, layer, or comp).
struct EffectContext {
    A_long                  compItemID = 0;  // comp's project-item id (0 = unset)
    AEGP_LayerIDVal         layerID = AEGP_LayerIDVal_NONE;  // stable layer identity
    AEGP_InstalledEffectKey installedKey = AEGP_InstalledEffectKey_NONE;
};
static std::map<cg::editor::InstanceKey, EffectContext> g_effectCtx;

// Consecutive "cannot verify" idle ticks per instance (main thread only). A single
// could-not-verify tick is treated as transient and never force-closes the window
// (review finding: a stale/failed capture must not kill a live window). But a PERSISTENT
// failure - the parent layer or comp was deleted, so the cached layer handle no longer
// enumerates - closes the lingering window once it reaches CG_VERIFY_FAIL_LIMIT
// (captain: layer/comp deletion is crash-safe but leaves the window open; close it).
static std::map<cg::editor::InstanceKey, int> g_verifyFailures;
#define CG_VERIFY_FAIL_LIMIT 6  // ~0.8s at the windowed idle cadence (<=133ms/tick)

// Last param values the idle hook published to each window (touched only from the
// idle hook, which runs serially on the main thread - so no mutex). Lets the EC->window
// poll publish only when something actually changed, avoiding revision churn.
static std::map<cg::editor::InstanceKey, cg::editor::ParamSnapshot> g_lastPolled;

// --- Phase 4 live-preview driver state (idle hook / main thread only) --------
//
// The idle hook checks out the layer frame DOWNSTREAM of this effect (so decode + grade
// is already applied - the pipeline invariant holds by construction), copies it to a
// PreviewFrame, and publishes it to the window. A bounded per-instance LRU cache keyed
// by (frame time + grade-param fingerprint) keeps scrubbing interactive: revisiting a
// visited time/param state serves the cached CPU frame instead of re-rendering. All of
// this runs serially on AE's main thread, so no mutex is needed here (the cache/keying
// LOGIC lives in the pure, unit-tested editor/PreviewCache.h).
#define CG_PREVIEW_CACHE_CAP  16    // bounded LRU: keep ~16 decoded frames for scrub-back
#define CG_PREVIEW_MAX_DIM    960   // decimate the checked-out frame to <= this on the long side
#define CG_PREVIEW_DOWNSAMPLE 2     // render at 50% (preview is small; keeps the sync render cheap)

struct PreviewDriver {
    cg::editor::PreviewCache cache{CG_PREVIEW_CACHE_CAP};
    cg::editor::PreviewKey   lastKey;      // key of the frame currently shown by the window
    bool                     hasLast = false;
};
static std::map<cg::editor::InstanceKey, PreviewDriver> g_previewDrivers;

// --- Phase 5 in-effect analysis + before/after driver state ------------------
//
// The idle hook analyses the clip by sampling several frames UPSTREAM of this effect
// (AEGP_NewFromUpstreamOfEffect - the RAW footage), decoding each to Rec.709 via the
// footage profile (the SAME decode the Auto bake composes, so measured stats and the
// applied grade agree; V-Log is never analysed as raw log), and running the ported
// cg::core::computeStats over the union. Work is spread ONE frame per idle tick so a
// multi-frame job never stalls AE; a footage-profile change is debounced so a rapid
// toggle doesn't thrash checkouts. The measured stats are injected over the recipe's
// sourceStats at render (g_analyzedStats), so the Auto grade adapts to the real clip.
#define CG_ANALYSIS_FRAMES        8       // frames sampled evenly across the clip's span
#define CG_ANALYSIS_PIXEL_BUDGET  240000  // total decoded pixels fed to computeStats
#define CG_ANALYSIS_DEBOUNCE      5       // idle ticks a footage change must be stable
#define CG_ANALYSIS_DOWNSAMPLE    2       // AE-side downsample for the analysis checkout
#define CG_ANALYSIS_CACHE_CAP     6       // remembered (fingerprint -> stats) results
#define CG_BEFORE_CACHE_CAP       8       // before-frame LRU (decoded originals for split view)

// Analyzed source stats for an instance (written by the idle-hook analysis on the main
// thread, read at render on render threads - hence the mutex). Injected over the recipe's
// sourceStats when the footage profile still matches what was analysed.
struct AnalyzedStats {
    bool                            valid = false;
    cg::editor::AnalysisFingerprint fp;
    cg::core::StatsData             stats;
};
static std::mutex g_analyzedMutex;
static std::map<cg::editor::InstanceKey, AnalyzedStats> g_analyzedStats;

// Per-instance analysis runtime (idle hook / main thread only, so no mutex): the debounce
// gate, the incremental job, and a small results cache so re-selecting a previously
// analysed footage profile is instant (no re-run).
struct AnalysisRuntime {
    cg::editor::AnalysisDebounce debounce{CG_ANALYSIS_DEBOUNCE};
    cg::editor::AnalysisJob      job;
    // Tiny fingerprint->stats cache (linear; cap is small). front = most-recently-used.
    std::vector<std::pair<cg::editor::AnalysisFingerprint, cg::core::StatsData>> cache;
    // Before/after: LRU of decoded original frames keyed by (time + footage fingerprint).
    cg::editor::PreviewCache beforeCache{CG_BEFORE_CACHE_CAP};
    cg::editor::PreviewKey   beforeLastKey;
    bool                     beforeHasLast = false;

    const cg::core::StatsData* cacheFind(const cg::editor::AnalysisFingerprint& fp) {
        for (auto& kv : cache)
            if (kv.first == fp) return &kv.second;
        return nullptr;
    }
    void cachePut(const cg::editor::AnalysisFingerprint& fp, const cg::core::StatsData& s) {
        for (auto it = cache.begin(); it != cache.end(); ++it) {
            if (it->first == fp) { it->second = s; return; }
        }
        cache.insert(cache.begin(), {fp, s});
        if (cache.size() > CG_ANALYSIS_CACHE_CAP) cache.pop_back();
    }
};
static std::map<cg::editor::InstanceKey, AnalysisRuntime> g_analysisRuntime;
static std::atomic<uint64_t> g_idleTick{0};  // monotonic idle-hook tick for the debounce

// Flat POD sequence data: just a stable per-instance uid for the window registry.
#define CG_SEQ_MAGIC   0x43475351u  // 'CGSQ'
#define CG_SEQ_VERSION 1u
struct CG_SequenceData {
    A_u_long magic;
    A_u_long version;
    A_u_long uidHi;
    A_u_long uidLo;
};

// Monotonic-per-session uid, seeded from a clock so it is unlikely to collide with a
// uid persisted by a previous session. Regenerated on SETUP/RESETUP (so a duplicated
// or reloaded effect never shares a key). Windows never persist, so this is purely a
// runtime association token.
static uint64_t GenUid() {
    static std::atomic<uint64_t> counter{
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) << 16};
    return counter.fetch_add(1) + 1;
}

static void PackUid(CG_SequenceData* sd, uint64_t uid) {
    sd->uidHi = static_cast<A_u_long>(uid >> 32);
    sd->uidLo = static_cast<A_u_long>(uid & 0xffffffffu);
}
static uint64_t UnpackUid(const CG_SequenceData* sd) {
    return (static_cast<uint64_t>(sd->uidHi) << 32) | static_cast<uint64_t>(sd->uidLo);
}

/* =========================== LUT resolution =============================== */

#ifdef AE_OS_WIN
// Directory containing this plugin binary (so "External" can look next to the .aex).
static std::wstring ModuleDirW() {
    HMODULE hm = NULL;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleDirW), &hm)) {
        return std::wstring();
    }
    wchar_t path[MAX_PATH * 4];
    DWORD n = ::GetModuleFileNameW(hm, path, sizeof(path) / sizeof(path[0]));
    if (n == 0) return std::wstring();
    std::wstring full(path, n);
    size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash + 1);
}

static bool ReadFileTextW(const std::wstring& path, std::string& out) {
    if (path.empty()) return false;
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return !out.empty();
}

// Resolve the external .cube path: env CG_LUT_PATH first, then <pluginDir>/ColorGrade_LUT.cube.
static bool LoadExternalCube(cg::Lut3D& outLut) {
    std::string text;
    wchar_t envBuf[MAX_PATH * 4];
    DWORD envLen = ::GetEnvironmentVariableW(L"CG_LUT_PATH", envBuf, sizeof(envBuf) / sizeof(envBuf[0]));
    if (envLen > 0 && envLen < sizeof(envBuf) / sizeof(envBuf[0])) {
        if (ReadFileTextW(std::wstring(envBuf, envLen), text)) {
            try { outLut = cg::parseCube(text); return true; } catch (...) { /* fall through */ }
        }
    }
    std::wstring sidecar = ModuleDirW() + L"ColorGrade_LUT.cube";
    if (ReadFileTextW(sidecar, text)) {
        try { outLut = cg::parseCube(text); return true; } catch (...) { /* fall through */ }
    }
    return false;
}
#else
static bool LoadExternalCube(cg::Lut3D&) { return false; }
#endif

// Fill dst with the LUT chosen by the source popup, always leaving a valid LUT.
static void ResolveLut(A_long source, cg::Lut3D& dst) {
    if (source == CG_SRC_EXTERNAL && LoadExternalCube(dst)) return;
    dst = cg::EmbeddedLut();
}

/* ===================== Auto bake (theme + engine) ======================== */

// Map the theme popup (1-based) to a ported built-in theme.
static cg::core::Theme ThemeFromPopup(A_long themePopup) {
    switch (themePopup) {
        case CG_THEME_WARM: return cg::core::warmFilmTheme();
        case CG_THEME_COOL: return cg::core::coolNoirTheme();
        case CG_THEME_NONE: return cg::core::noneManualTheme();
        case CG_THEME_TEAL:
        default: return cg::core::tealOrangeTheme();
    }
}

// Map the footage-profile popup (1-based) to a ported log profile.
static const cg::core::LogProfile& ProfileFromFootagePopup(A_long footagePopup) {
    return footagePopup == CG_FOOT_VLOG ? cg::core::VLOG_PROFILE() : cg::core::REC709_PROFILE();
}

// Bake the Auto-mode grade LUT natively: the popup theme supplies the look, the
// arb-data recipe supplies the measured source stats, and the sliders drive the
// engine knobs (strength / skinProtection as EngineOptions, chroma-gain as a
// relative multiplier on the theme's authored chromaGain). This is the in-effect
// counterpart to the TS bakeGradeLut; the cross-engine golden harness proves the
// two agree. The chromaGain arg is the slider fraction (slider/100), so 100% (=1.0)
// preserves each theme's authored chromaGain exactly and the slider scales from there.
static void BakeAutoLut(A_long themePopup, A_long footagePopup, double strength01, double skin01,
                        double chromaGain, double exposure, double lookMix, double temperature,
                        const cg::core::RecipeData& recipe, cg::Lut3D& dst) {
    cg::core::Theme theme = ThemeFromPopup(themePopup);
    cg::core::ThemeOverrides ov = theme.overrides.value_or(cg::core::ThemeOverrides{});
    const double authored = ov.chromaGain.value_or(1.0);
    ov.chromaGain = authored * chromaGain;  // slider scales the theme's authored gain

    // Phase 6b/6c: COMPOSE the editor-owned layers (curves / 3-way tints) onto the popup
    // theme's authored look via the SAME core helper the pure themeFromRecipe path uses. The
    // theme LOOK stays popup-driven; only these user edits come from the recipe, so switching
    // themes never applies a stale editor override and an old grade with no edits bakes exactly
    // the theme. User curves REPLACE the theme's authored curve per slot; user 3-way tints ADD.
    cg::core::applyEditorOverrides(ov, recipe);
    theme.overrides = ov;

    cg::core::FootageStats src = cg::core::statsFromData(recipe.sourceStats);
    cg::core::EngineOptions opts;
    opts.strength = strength01;
    opts.skinProtection = skin01;
    // Phase 6a: fold the recipe's manual primary correction into the bake, with the
    // live keyframeable PF params (Exposure/Temperature/Look Mix) overriding the
    // recipe's stored values (decision D1). Neutral values contribute exact identity.
    cg::core::ManualGrade manual = cg::core::manualFromRecipe(recipe);
    manual.exposure = exposure;
    manual.temperature = temperature;
    opts.manual = manual;
    // Phase 6c: the Lift/Gamma/Gain wheels are editor state carried in the recipe.
    opts.lgg = cg::core::lggFromRecipe(recipe);
    opts.lookMix = lookMix;

    // Correct + Grade in one baked LUT. When the clip is a log profile (V-Log), the
    // Correct stage decodes each pixel to Rec.709 *before* the grade; baking the
    // composition into a single LUT keeps the CPU and GPU apply paths identical (no
    // per-pixel decode in the kernel). Rec.709 footage decodes to itself, so the
    // standard profile bakes the plain grade unchanged. (The decode stage applies to
    // the Auto path; the Embedded/External raw-LUT modes are unaffected by design.)
    auto grade = cg::core::buildTransform(src, theme, opts);
    if (footagePopup == CG_FOOT_VLOG) {
        const cg::core::LogProfile& profile = ProfileFromFootagePopup(footagePopup);
        dst = cg::core::bakeLut(
            [&grade, &profile](const cg::core::Vec3d& x) {
                return grade(cg::core::decodePixelToRec709(x, profile));
            },
            CG_GRADE_LUT_SIZE, theme.name + " correct+grade");
    } else {
        dst = cg::core::bakeLut(grade, CG_GRADE_LUT_SIZE, theme.name + " grade");
    }
}

// Resample a baked LUT so the Footage/Correct decode applies *before* it and the
// Strength blend happens in DECODED space:
//   newLut(x) = lerp(decode(x), rawLut(decode(x)), strength01)
//             = decode(x)*(1-s) + rawLut(decode(x))*s.
// This makes the decode stage apply in the Embedded/External raw-LUT modes too
// (captain directive: never leave V-Log footage undecoded under any LUT Source) AND
// bakes Strength into the composed LUT so at s<100% the blend is decoded-vs-graded,
// never a raw-log term. Callers apply this LUT at applyStrength=1.0. The per-pixel
// apply stays a single trilinear sample (CPU/GPU identical). Rec.709 decodes to
// itself, so it is a no-op and the caller keeps the raw LUT + slider strength.
// (The Auto path composes the decode into the *continuous* grade in BakeAutoLut, which
// avoids this resample's extra interpolation; here we only have a baked LUT to compose.)
static cg::Lut3D ComposeDecodeIntoLut(const cg::Lut3D& lut, A_long footagePopup, double strength01) {
    if (footagePopup != CG_FOOT_VLOG) return lut;
    const cg::core::LogProfile& profile = ProfileFromFootagePopup(footagePopup);
    return cg::core::bakeLut(
        [&lut, &profile, strength01](const cg::core::Vec3d& x) -> cg::core::Vec3d {
            const cg::core::Vec3d dec = cg::core::decodePixelToRec709(x, profile);
            const cg::Vec3 s = cg::sampleLut(
                lut, cg::Vec3{static_cast<float>(dec[0]), static_cast<float>(dec[1]),
                              static_cast<float>(dec[2])});
            return cg::core::Vec3d{
                dec[0] * (1.0 - strength01) + s[0] * strength01,
                dec[1] * (1.0 - strength01) + s[1] * strength01,
                dec[2] * (1.0 - strength01) + s[2] * strength01,
            };
        },
        lut.size);
}

/* ========================== Arb-data (recipe) ============================ */
//
// The grade recipe is a flat POD (cg::core::RecipeData). AE persists it in the
// project and hands it back at render time. All the arb callbacks reduce to a
// byte copy because the struct has no pointers; the refcon guard rejects a blob
// meant for a different param, and NEW/UNFLATTEN seed/validate the magic+version.
// Pattern mirrors the SDK ColorGrid sample's arb handler.

using cg::core::RecipeData;

// Allocate a default recipe handle: the teal-orange theme over neutral placeholder
// source stats (its own target stats), until in-effect analysis populates real ones.
static PF_Err CreateDefaultRecipe(PF_InData* in_data, PF_ArbitraryH* arbPH) {
    PF_Err err = PF_Err_NONE;
    PF_Handle arbH = PF_NEW_HANDLE(sizeof(RecipeData));
    if (!arbH) return PF_Err_OUT_OF_MEMORY;
    RecipeData* p = reinterpret_cast<RecipeData*>(PF_LOCK_HANDLE(arbH));
    if (!p) {
        err = PF_Err_OUT_OF_MEMORY;
    } else {
        cg::core::Theme th = cg::core::tealOrangeTheme();
        RecipeData def = cg::core::recipeFromTheme(th, th.targetStats);
        std::memcpy(p, &def, sizeof(RecipeData));
        *arbPH = arbH;
        PF_UNLOCK_HANDLE(arbH);
    }
    return err;
}

// The default recipe used as the migration fallback (foreign/corrupt blob): the
// teal-orange theme over neutral placeholder stats, matching CreateDefaultRecipe.
static RecipeData DefaultRecipe() {
    cg::core::Theme th = cg::core::tealOrangeTheme();
    return cg::core::recipeFromTheme(th, th.targetStats);
}

static PF_Err HandleArbitrary(PF_InData* in_data, PF_OutData* out_data, PF_ArbParamsExtra* extra) {
    PF_Err err = PF_Err_NONE;
    switch (extra->which_function) {
        case PF_Arbitrary_NEW_FUNC:
            if (extra->u.new_func_params.refconPV != CG_ARB_REFCON) return PF_Err_INTERNAL_STRUCT_DAMAGED;
            err = CreateDefaultRecipe(in_data, extra->u.new_func_params.arbPH);
            break;
        case PF_Arbitrary_DISPOSE_FUNC:
            if (extra->u.dispose_func_params.refconPV != CG_ARB_REFCON) return PF_Err_INTERNAL_STRUCT_DAMAGED;
            PF_DISPOSE_HANDLE(extra->u.dispose_func_params.arbH);
            break;
        case PF_Arbitrary_COPY_FUNC: {
            if (extra->u.copy_func_params.refconPV != CG_ARB_REFCON) return PF_Err_INTERNAL_STRUCT_DAMAGED;
            err = CreateDefaultRecipe(in_data, extra->u.copy_func_params.dst_arbPH);
            if (!err) {
                RecipeData* srcP = reinterpret_cast<RecipeData*>(PF_LOCK_HANDLE(extra->u.copy_func_params.src_arbH));
                RecipeData* dstP = reinterpret_cast<RecipeData*>(PF_LOCK_HANDLE(*extra->u.copy_func_params.dst_arbPH));
                if (srcP && dstP) std::memcpy(dstP, srcP, sizeof(RecipeData));
                PF_UNLOCK_HANDLE(extra->u.copy_func_params.src_arbH);
                PF_UNLOCK_HANDLE(*extra->u.copy_func_params.dst_arbPH);
            }
            break;
        }
        case PF_Arbitrary_FLAT_SIZE_FUNC:
            *(extra->u.flat_size_func_params.flat_data_sizePLu) = sizeof(RecipeData);
            break;
        case PF_Arbitrary_FLATTEN_FUNC:
            if (extra->u.flatten_func_params.buf_sizeLu >= sizeof(RecipeData)) {
                RecipeData* srcP = reinterpret_cast<RecipeData*>(PF_LOCK_HANDLE(extra->u.flatten_func_params.arbH));
                if (srcP) std::memcpy(extra->u.flatten_func_params.flat_dataPV, srcP, sizeof(RecipeData));
                PF_UNLOCK_HANDLE(extra->u.flatten_func_params.arbH);
            }
            break;
        case PF_Arbitrary_UNFLATTEN_FUNC: {
            PF_Handle handle = PF_NEW_HANDLE(sizeof(RecipeData));
            if (!handle) return PF_Err_OUT_OF_MEMORY;
            RecipeData* dstP = reinterpret_cast<RecipeData*>(PF_LOCK_HANDLE(handle));
            if (dstP) {
                // Migrate the persisted blob forward instead of reseeding on any
                // mismatch: a v2 grade (or the current version) survives; only a
                // foreign/corrupt blob falls back to the default. Never leave *arbPH
                // unset - AE must always receive a valid handle.
                cg::core::migrateRecipeInto(dstP, extra->u.unflatten_func_params.flat_dataPV,
                                            extra->u.unflatten_func_params.buf_sizeLu,
                                            DefaultRecipe());
            }
            *(extra->u.unflatten_func_params.arbPH) = handle;
            PF_UNLOCK_HANDLE(handle);
            break;
        }
        case PF_Arbitrary_INTERP_FUNC:
            // No meaningful tween for a grade recipe; snap to the left keyframe.
            err = CreateDefaultRecipe(in_data, extra->u.interp_func_params.interpPH);
            if (!err) {
                RecipeData* lP = reinterpret_cast<RecipeData*>(PF_LOCK_HANDLE(extra->u.interp_func_params.left_arbH));
                RecipeData* iP = reinterpret_cast<RecipeData*>(PF_LOCK_HANDLE(*extra->u.interp_func_params.interpPH));
                if (lP && iP) std::memcpy(iP, lP, sizeof(RecipeData));
                PF_UNLOCK_HANDLE(extra->u.interp_func_params.left_arbH);
                PF_UNLOCK_HANDLE(*extra->u.interp_func_params.interpPH);
            }
            break;
        case PF_Arbitrary_COMPARE_FUNC: {
            RecipeData* aP = reinterpret_cast<RecipeData*>(PF_LOCK_HANDLE(extra->u.compare_func_params.a_arbH));
            RecipeData* bP = reinterpret_cast<RecipeData*>(PF_LOCK_HANDLE(extra->u.compare_func_params.b_arbH));
            *extra->u.compare_func_params.compareP =
                (aP && bP && std::memcmp(aP, bP, sizeof(RecipeData)) == 0) ? PF_ArbCompare_EQUAL
                                                                          : PF_ArbCompare_NOT_EQUAL;
            PF_UNLOCK_HANDLE(extra->u.compare_func_params.a_arbH);
            PF_UNLOCK_HANDLE(extra->u.compare_func_params.b_arbH);
            break;
        }
        case PF_Arbitrary_PRINT_SIZE_FUNC:
            *extra->u.print_size_func_params.print_sizePLu = 0;  // no text representation
            break;
        case PF_Arbitrary_PRINT_FUNC:
        case PF_Arbitrary_SCAN_FUNC:
            break;  // binary-only arb data: no text print/scan
    }
    return err;
}

/* ==================== Render-data resolution (shared) ==================== */

// Copy the grade recipe out of its arb handle (falling back to the default
// recipe if the handle is missing), so callers can checkin the param safely.
static RecipeData RecipeFromHandle(PF_InData* in_data, PF_ArbitraryH h) {
    if (h) {
        RecipeData* rp = reinterpret_cast<RecipeData*>(PF_LOCK_HANDLE(h));
        if (rp) {
            RecipeData copy = *rp;
            PF_UNLOCK_HANDLE(h);
            return copy;
        }
    }
    cg::core::Theme th = cg::core::tealOrangeTheme();
    return cg::core::recipeFromTheme(th, th.targetStats);
}

// Phase 5: override the recipe's placeholder sourceStats with the in-effect analysis for
// this instance, when it exists AND still matches the current footage decode. This is what
// makes the Auto grade adapt to the real clip: the idle-hook analysis measures the decoded
// footage and stores it in g_analyzedStats; render reads it here (mutex-guarded, since the
// idle hook writes on the main thread and render runs on render threads). A footage-profile
// change invalidates the match until the (debounced) re-analysis completes, so stale stats
// from the other decode are never applied.
static void InjectAnalyzedStats(cg::editor::InstanceKey key, A_long footage, RecipeData& recipe) {
    std::lock_guard<std::mutex> lk(g_analyzedMutex);
    auto it = g_analyzedStats.find(key);
    if (it != g_analyzedStats.end() && it->second.valid &&
        it->second.fp.footageProfile == static_cast<int>(footage)) {
        recipe.sourceStats = it->second.stats;
    }
}

// Resolve the LUT + post-blend for a frame from the resolved param values. Auto
// bakes natively (strength baked in); embedded/external keep the Phase 1 blend.
static void ResolveRenderData(PF_InData* in_data, A_long source, A_long themePopup, A_long footagePopup,
                              double strength01, double skin01, double chromaGain, double exposure,
                              double lookMix, double temperature, const RecipeData& recipe,
                              CG_RenderData& d) {
    if (source == CG_SRC_AUTO) {
        // Manual primary correction (Phase 6a) folds into the Auto grade bake. The
        // Embedded/External raw-LUT modes carry no buildTransform to fold it into, so
        // manual grading applies in Auto mode (the theme / None-Manual path).
        BakeAutoLut(themePopup, footagePopup, strength01, skin01, chromaGain, exposure, lookMix,
                    temperature, recipe, d.lut);
        d.applyStrength = 1.0f;
    } else {
        // Embedded/External raw LUT. For V-Log the Footage/Correct decode is composed in
        // and Strength is baked into the composed LUT (blend in decoded space, applied at
        // 1.0) so V-Log is never left undecoded even at partial Strength (captain
        // directive). Rec.709 decodes to itself: keep the raw LUT + slider-strength blend.
        cg::Lut3D raw;
        ResolveLut(source, raw);
        if (footagePopup == CG_FOOT_VLOG) {
            d.lut = ComposeDecodeIntoLut(raw, footagePopup, strength01);
            d.applyStrength = 1.0f;
        } else {
            d.lut = raw;
            d.applyStrength = static_cast<float>(strength01);
        }
    }
}

/* ===================== Editor window <-> effect bridge =================== */
//
// Phase 3 editor spike. The "Open Editor" button opens a native ImGui/D3D11 window
// (editor/EditorWindow.*); the window and this effect instance talk through the
// pure EditorBridge.h seam. Direction of flow:
//   - effect -> window: PublishEditorSnapshot() copies the current param values so
//     the window's controls mirror Effect Controls (called from PreRender; a locked
//     value copy, so it is safe from any render thread).
//   - window -> effect: the window pushes ParamEdits; ApplyEditorEdits() drains them
//     and writes them back onto the effect's params (with CHANGED_VALUE so AE
//     re-renders and Effect Controls updates). See adr-editor-ui.md for the
//     production driver (a companion-AEGP idle hook applying edits continuously via
//     the AEGP StreamSuite inside one undo group) - this spike drains on the next
//     param-change command and leaves the continuous driver + AEGP DOM writes to the
//     post-decision full build (captain-verified in AE).
//
// The per-instance key is derived from in_data->effect_ref (stable for an applied
// effect's lifetime within a session). Production hardening: store a generated uid
// in sequence data so the key survives an effect_ref reuse across delete/re-add.

// Stable per-instance key = the uid in this effect's sequence data (consistent across
// every command for one instance). Falls back to effect_ref only if sequence data is
// somehow unavailable.
static cg::editor::InstanceKey SeqKey(PF_InData* in_data) {
    if (in_data->sequence_data) {
        CG_SequenceData* sd = reinterpret_cast<CG_SequenceData*>(PF_LOCK_HANDLE(in_data->sequence_data));
        if (sd && sd->magic == CG_SEQ_MAGIC) {
            const uint64_t uid = UnpackUid(sd);
            PF_UNLOCK_HANDLE(in_data->sequence_data);
            return static_cast<cg::editor::InstanceKey>(uid);
        }
        if (sd) PF_UNLOCK_HANDLE(in_data->sequence_data);
    }
    return static_cast<cg::editor::InstanceKey>(reinterpret_cast<uintptr_t>(in_data->effect_ref));
}

// Monotonic snapshot revision so the window can tell a genuinely newer publish from
// a stale one and never stomp the control the user is mid-drag on.
static std::atomic<uint64_t> g_snapshotRevision{1};

// Cheap FNV-1a hash over the whole recipe blob (flat POD), folded into the preview
// fingerprint so any manual/recipe change busts the preview cache (Phase 6a).
static uint64_t RecipeHash(const RecipeData& r) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&r);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(RecipeData); ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
    return h;
}

// The recipe-backed 9 manual controls -> the bridge ManualState (editor display).
static cg::editor::ManualState ManualStateFromRecipe(const RecipeData& r) {
    cg::editor::ManualState m;
    m.contrast = r.manualContrast;
    m.pivot = r.manualPivot;
    m.highlights = r.manualHighlights;
    m.shadows = r.manualShadows;
    m.whites = r.manualWhites;
    m.blacks = r.manualBlacks;
    m.tint = r.manualTint;
    m.saturation = r.manualSaturation;
    m.vibrance = r.manualVibrance;
    return m;
}

// Write a bridge ManualState back into a recipe's 9 manual controls (window -> effect).
static void ApplyManualStateToRecipe(const cg::editor::ManualState& m, RecipeData& r) {
    r.manualContrast = m.contrast;
    r.manualPivot = m.pivot;
    r.manualHighlights = m.highlights;
    r.manualShadows = m.shadows;
    r.manualWhites = m.whites;
    r.manualBlacks = m.blacks;
    r.manualTint = m.tint;
    r.manualSaturation = m.saturation;
    r.manualVibrance = m.vibrance;
}

// --- Curves (Phase 6b): recipe CurveData <-> bridge CurveState ---------------
static cg::editor::CurveState CurveStateFromData(const cg::core::CurveData& c) {
    cg::editor::CurveState s;
    int n = c.count < 0 ? 0 : c.count;
    if (n > cg::editor::CG_EDIT_MAX_CURVE_POINTS) n = cg::editor::CG_EDIT_MAX_CURVE_POINTS;
    s.count = n;
    for (int i = 0; i < n; ++i) { s.x[i] = c.pts[i][0]; s.y[i] = c.pts[i][1]; }
    return s;
}
static void CurveStateToData(const cg::editor::CurveState& s, cg::core::CurveData& c) {
    int n = s.count < 0 ? 0 : s.count;
    if (n > cg::core::RECIPE_MAX_POINTS) n = cg::core::RECIPE_MAX_POINTS;
    c = cg::core::CurveData{};
    c.count = n;
    for (int i = 0; i < n; ++i) { c.pts[i][0] = s.x[i]; c.pts[i][1] = s.y[i]; }
}
// The Curves tab is EDITOR-OWNED (writes the recipe's userToneCurve/userChannelR/G/B),
// distinct from the theme-seeded toneCurve/channelR/G/B. For DISPLAY the tab shows the
// EFFECTIVE curve per slot: the user's curve if present, else the picked theme's authored
// curve (so the user edits from the theme's curve as a starting point). The bake mirrors
// this per-slot replace (BakeAutoLut). `themePopup` supplies the theme fallback.
static cg::editor::CurvesState CurvesStateForDisplay(const RecipeData& r, A_long themePopup) {
    cg::core::Theme theme = ThemeFromPopup(themePopup);
    cg::core::ThemeOverrides ov = theme.overrides.value_or(cg::core::ThemeOverrides{});
    cg::core::ChannelCurves cc = ov.channelCurves.value_or(cg::core::ChannelCurves{});
    auto pick = [](const cg::core::CurveData& user,
                   const std::optional<std::vector<cg::core::CurvePoint>>& authored) {
        if (user.count > 0) return CurveStateFromData(user);
        return CurveStateFromData(cg::core::curveToData(authored));
    };
    cg::editor::CurvesState s;
    s.master = pick(r.userToneCurve, ov.toneCurve);
    s.r = pick(r.userChannelR, cc.r);
    s.g = pick(r.userChannelG, cc.g);
    s.b = pick(r.userChannelB, cc.b);
    return s;
}
static void ApplyCurvesStateToRecipe(const cg::editor::CurvesState& s, RecipeData& r) {
    CurveStateToData(s.master, r.userToneCurve);
    CurveStateToData(s.r, r.userChannelR);
    CurveStateToData(s.g, r.userChannelG);
    CurveStateToData(s.b, r.userChannelB);
}

// --- Wheels (Phase 6c): recipe LGG triples + additive 3-way user tints <-> WheelsState.
// The LGG triples are the primary face; the Adobe 3-way secondary mode's color discs are
// the editor's ADDITIVE user band tints (userShadowTint/...), distinct from the theme's
// authored band tints, and its per-band luminance reuses the LGG masters. No new engine math.
static cg::editor::WheelsState WheelsStateFromRecipe(const RecipeData& r) {
    cg::editor::WheelsState s;
    for (int c = 0; c < 3; ++c) { s.lift[c] = r.lift[c]; s.gamma[c] = r.gamma[c]; s.gain[c] = r.gain[c]; }
    s.shadowTint[0] = r.userShadowTint[0]; s.shadowTint[1] = r.userShadowTint[1];
    s.hasShadowTint = r.userShadowTint[0] != 0.0 || r.userShadowTint[1] != 0.0;
    s.midTint[0] = r.userMidTint[0]; s.midTint[1] = r.userMidTint[1];
    s.hasMidTint = r.userMidTint[0] != 0.0 || r.userMidTint[1] != 0.0;
    s.highTint[0] = r.userHighTint[0]; s.highTint[1] = r.userHighTint[1];
    s.hasHighTint = r.userHighTint[0] != 0.0 || r.userHighTint[1] != 0.0;
    s.mode = static_cast<int>(r.wheelsMode);
    return s;
}
static void ApplyWheelsStateToRecipe(const cg::editor::WheelsState& s, RecipeData& r) {
    for (int c = 0; c < 3; ++c) { r.lift[c] = s.lift[c]; r.gamma[c] = s.gamma[c]; r.gain[c] = s.gain[c]; }
    r.userShadowTint[0] = s.shadowTint[0]; r.userShadowTint[1] = s.shadowTint[1];
    r.userMidTint[0] = s.midTint[0]; r.userMidTint[1] = s.midTint[1];
    r.userHighTint[0] = s.highTint[0]; r.userHighTint[1] = s.highTint[1];
    r.wheelsMode = static_cast<uint32_t>(s.mode);
}

// Dispatch one recipe-backed edit (Manual/Curves/Wheels) into the recipe blob.
static void ApplyRecipeEditToRecipe(const cg::editor::ParamEdit& e, RecipeData& r) {
    switch (e.field) {
        case cg::editor::EditField::Manual: ApplyManualStateToRecipe(e.manual, r); break;
        case cg::editor::EditField::Curves: ApplyCurvesStateToRecipe(e.curves, r); break;
        case cg::editor::EditField::Wheels: ApplyWheelsStateToRecipe(e.wheels, r); break;
        default: break;
    }
}

static cg::editor::ParamSnapshot MakeSnapshot(A_long footage, A_long theme, double strength01,
                                              double skin01, double chromaFrac, A_long source,
                                              double exposure, double lookMix, double temperature,
                                              const RecipeData& recipe) {
    cg::editor::ParamSnapshot s;
    s.footageProfile = static_cast<int>(footage);
    s.theme = static_cast<int>(theme);
    s.strength = strength01;
    s.skinProtection = skin01;
    s.chromaGain = chromaFrac;
    s.lutSource = static_cast<int>(source);
    s.exposure = exposure;
    s.lookMix = lookMix;
    s.temperature = temperature;
    s.manual = ManualStateFromRecipe(recipe);
    s.curves = CurvesStateForDisplay(recipe, theme);
    s.wheels = WheelsStateFromRecipe(recipe);
    s.recipeHash = RecipeHash(recipe);
    s.revision = g_snapshotRevision.fetch_add(1);
    return s;
}

// Push the effect's current params to the editor window (if open).
static void PublishEditorSnapshot(PF_InData* in_data, A_long footage, A_long theme, double strength01,
                                  double skin01, double chromaFrac, A_long source, double exposure,
                                  double lookMix, double temperature, const RecipeData& recipe) {
    cg::editor::EditorHost::instance().publishSnapshot(
        SeqKey(in_data), MakeSnapshot(footage, theme, strength01, skin01, chromaFrac, source,
                                      exposure, lookMix, temperature, recipe));
}

// Drain any edits the window produced and write them onto params[] (CHANGED_VALUE so
// AE re-renders). Valid only where params[] is writable (USER_CHANGED_PARAM context).
static void ApplyEditorEdits(PF_InData* in_data, PF_ParamDef* params[]) {
    auto edits = cg::editor::EditorHost::instance().drainEdits(SeqKey(in_data));
    for (const auto& e : edits) {
        switch (e.field) {
            case cg::editor::EditField::FootageProfile:
                params[CG_FOOTAGE_PROFILE]->u.pd.value = static_cast<A_long>(e.value + 0.5);
                params[CG_FOOTAGE_PROFILE]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
                break;
            case cg::editor::EditField::Theme:
                params[CG_THEME]->u.pd.value = static_cast<A_long>(e.value + 0.5);
                params[CG_THEME]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
                break;
            case cg::editor::EditField::Strength:
                params[CG_STRENGTH]->u.fs_d.value = cg::editor::clamp01(e.value) * 100.0;
                params[CG_STRENGTH]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
                break;
            case cg::editor::EditField::SkinProtection:
                params[CG_SKIN_PROTECTION]->u.fs_d.value = cg::editor::clamp01(e.value) * 100.0;
                params[CG_SKIN_PROTECTION]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
                break;
            case cg::editor::EditField::ChromaGain:
                params[CG_CHROMA_GAIN]->u.fs_d.value = cg::editor::clampChromaFraction(e.value) * 100.0;
                params[CG_CHROMA_GAIN]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
                break;
            case cg::editor::EditField::LutSource:
                params[CG_LUT_SOURCE]->u.pd.value = static_cast<A_long>(e.value + 0.5);
                params[CG_LUT_SOURCE]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
                break;
            case cg::editor::EditField::Exposure:
                params[CG_EXPOSURE]->u.fs_d.value =
                    cg::editor::clampRange(e.value, CG_EXPOSURE_MIN, CG_EXPOSURE_MAX);
                params[CG_EXPOSURE]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
                break;
            case cg::editor::EditField::LookMix:
                params[CG_LOOK_MIX]->u.fs_d.value = cg::editor::clamp01(e.value) * 100.0;
                params[CG_LOOK_MIX]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
                break;
            case cg::editor::EditField::Temperature:
                params[CG_TEMPERATURE]->u.fs_d.value =
                    cg::editor::clampRange(e.value, CG_TEMPERATURE_MIN, CG_TEMPERATURE_MAX);
                params[CG_TEMPERATURE]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
                break;
            case cg::editor::EditField::Manual:
            case cg::editor::EditField::Curves:
            case cg::editor::EditField::Wheels: {
                // Recipe-backed controls (manual / curves / wheels): mutate the CG_RECIPE
                // arb handle in place (the SDK-sanctioned ColorGrid pattern) + CHANGED_VALUE.
                PF_ArbitraryH arbH = params[CG_RECIPE]->u.arb_d.value;
                if (arbH) {
                    RecipeData* rp = reinterpret_cast<RecipeData*>(PF_LOCK_HANDLE(arbH));
                    if (rp) {
                        ApplyRecipeEditToRecipe(e, *rp);
                        PF_UNLOCK_HANDLE(arbH);
                        params[CG_RECIPE]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
                    }
                }
                break;
            }
        }
    }
}

/* ============= Window -> effect writes via AEGP (idle-hook driver) ======== */
//
// The idle hook (registered in GlobalSetup) is the main-thread driver that actually
// applies window-originated edits: it drains each open window's queue and writes the
// values onto the effect's param streams so AE re-renders and Effect Controls updates.
// This is what makes the window->effect direction work without the user also touching
// Effect Controls (the ApplyEditorEdits stand-in above still runs on any param change).

// Map an edit to its effect param stream index + the stream's one_d value (popups take
// the 1-based index; the percent sliders take 0..100).
static bool StreamForEdit(const cg::editor::ParamEdit& e, PF_ParamIndex& idx, double& one_d) {
    using F = cg::editor::EditField;
    switch (e.field) {
        case F::FootageProfile: idx = CG_FOOTAGE_PROFILE; one_d = static_cast<int>(e.value + 0.5); return true;
        case F::Theme:          idx = CG_THEME;           one_d = static_cast<int>(e.value + 0.5); return true;
        case F::Strength:       idx = CG_STRENGTH;        one_d = cg::editor::clamp01(e.value) * 100.0; return true;
        case F::SkinProtection: idx = CG_SKIN_PROTECTION; one_d = cg::editor::clamp01(e.value) * 100.0; return true;
        case F::ChromaGain:     idx = CG_CHROMA_GAIN;     one_d = cg::editor::clampChromaFraction(e.value) * 100.0; return true;
        case F::LutSource:      idx = CG_LUT_SOURCE;      one_d = static_cast<int>(e.value + 0.5); return true;
        case F::Exposure:       idx = CG_EXPOSURE;        one_d = cg::editor::clampRange(e.value, CG_EXPOSURE_MIN, CG_EXPOSURE_MAX); return true;
        case F::LookMix:        idx = CG_LOOK_MIX;        one_d = cg::editor::clamp01(e.value) * 100.0; return true;
        case F::Temperature:    idx = CG_TEMPERATURE;     one_d = cg::editor::clampRange(e.value, CG_TEMPERATURE_MIN, CG_TEMPERATURE_MAX); return true;
        case F::Manual:                        // recipe arb writes, handled out-of-band (idle hook)
        case F::Curves:
        case F::Wheels:         return false;
    }
    return false;
}

// Compute an effect instance's stable identity (comp project-item id + layer id + installed
// key) from a LIVE effect_ref (main-thread command context only). Returns false if any AEGP
// step fails or the identity is incomplete. Shared by capture and the Open-Editor
// duplicate-window reap so both agree on "which effect is this".
static bool ComputeEffectContext(AEGP_SuiteHandler& sh, PF_ProgPtr effectRef, EffectContext& out) {
    AEGP_LayerH layerH = nullptr;
    if (sh.PFInterfaceSuite1()->AEGP_GetEffectLayer(effectRef, &layerH) || !layerH) return false;
    AEGP_CompH compH = nullptr;
    if (sh.LayerSuite9()->AEGP_GetLayerParentComp(layerH, &compH) || !compH) return false;
    AEGP_ItemH itemH = nullptr;
    if (sh.CompSuite12()->AEGP_GetItemFromComp(compH, &itemH) || !itemH) return false;
    if (sh.ItemSuite9()->AEGP_GetItemID(itemH, &out.compItemID) || out.compItemID == 0) return false;
    if (sh.LayerSuite9()->AEGP_GetLayerID(layerH, &out.layerID) ||
        out.layerID == AEGP_LayerIDVal_NONE) {
        return false;
    }
    AEGP_EffectRefH tmpH = nullptr;
    if (!sh.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(g_aegpId, effectRef, &tmpH) && tmpH) {
        sh.EffectSuite5()->AEGP_GetInstalledKeyFromLayerEffect(tmpH, &out.installedKey);
        sh.EffectSuite5()->AEGP_DisposeEffect(tmpH);
    }
    return out.installedKey != AEGP_InstalledEffectKey_NONE;
}

// Capture this effect instance's stable identity at button-open (in_data->effect_ref valid).
static void CaptureEffectContextForKey(PF_InData* in_data, cg::editor::InstanceKey key) {
    if (!g_spbasic || !g_aegpId || !in_data->effect_ref) return;
    try {
        AEGP_SuiteHandler sh(g_spbasic);
        EffectContext ctx;
        if (!ComputeEffectContext(sh, in_data->effect_ref, ctx)) return;
        std::lock_guard<std::mutex> lk(g_effMutex);
        g_effectCtx[key] = ctx;
    } catch (...) { /* AEGP unavailable - the on-param-change stand-in still applies edits */ }
}

static void ForgetEffectContextForKey(cg::editor::InstanceKey key) {
    std::lock_guard<std::mutex> lk(g_effMutex);
    g_effectCtx.erase(key);  // nothing owned to dispose - only stable ids are stored
}

// Drop all Phase 5 per-instance state for a closed/deleted window (analysis runtime +
// the cross-thread analyzed stats). Main-thread only, alongside the preview-driver erase.
static void ForgetAnalysisStateForKey(cg::editor::InstanceKey key) {
    g_analysisRuntime.erase(key);
    std::lock_guard<std::mutex> lk(g_analyzedMutex);
    g_analyzedStats.erase(key);
}

// Re-resolve the CURRENT comp handle for a stored comp project-item id by walking the live
// project's item list (AEGP_GetFirstProjItem/AEGP_GetNextProjItem). A deleted comp is not in
// that list (it zombies in the undo buffer, off the live tree), so this returns null exactly
// when the comp is gone - and re-succeeds after an undo restores it. Null => comp not live.
static AEGP_CompH ResolveCompByItemID(AEGP_SuiteHandler& sh, A_long compItemID) {
    if (compItemID == 0) return nullptr;
    AEGP_ProjectH projH = nullptr;
    if (sh.ProjSuite6()->AEGP_GetProjectByIndex(0, &projH) || !projH) return nullptr;
    AEGP_ItemH itemH = nullptr;
    if (sh.ItemSuite9()->AEGP_GetFirstProjItem(projH, &itemH)) return nullptr;
    while (itemH) {
        A_long id = 0;
        if (!sh.ItemSuite9()->AEGP_GetItemID(itemH, &id) && id == compItemID) {
            AEGP_ItemType type = AEGP_ItemType_NONE;
            if (!sh.ItemSuite9()->AEGP_GetItemType(itemH, &type) && type == AEGP_ItemType_COMP) {
                AEGP_CompH compH = nullptr;
                if (!sh.CompSuite12()->AEGP_GetCompFromItem(itemH, &compH) && compH) return compH;
            }
            return nullptr;  // id present but not a live comp
        }
        AEGP_ItemH nextH = nullptr;
        if (sh.ItemSuite9()->AEGP_GetNextProjItem(projH, itemH, &nextH)) break;
        itemH = nextH;
    }
    return nullptr;  // comp id not found in the live project -> deleted
}

// Enforce exactly one editor window per effect. Closes any window bound to the SAME effect
// (same comp-item + layer id) under a DIFFERENT instance key - e.g. a stale orphan left when
// a delete+undo reseeded this effect's sequence-data uid. Called on Open Editor before
// opening the current key, so a fresh open REPLACES a lingering orphan rather than spawning a
// second window fighting over one effect. Main-thread only (button command), like the idle
// hook, so the un-mutexed idle-hook maps are safe to touch here.
static void CloseDuplicateWindowsForEffect(AEGP_SuiteHandler& sh, PF_ProgPtr effectRef,
                                           cg::editor::InstanceKey currentKey) {
    EffectContext cur;
    if (!ComputeEffectContext(sh, effectRef, cur)) return;
    std::vector<cg::editor::InstanceKey> dups;
    {
        std::lock_guard<std::mutex> lk(g_effMutex);
        for (auto& kv : g_effectCtx) {
            if (kv.first != currentKey && kv.second.compItemID == cur.compItemID &&
                kv.second.layerID == cur.layerID) {
                dups.push_back(kv.first);
            }
        }
    }
    for (auto k : dups) {
        cg::editor::EditorHost::instance().close(k);
        ForgetEffectContextForKey(k);
        g_lastPolled.erase(k);
        g_verifyFailures.erase(k);
        g_previewDrivers.erase(k);
        ForgetAnalysisStateForKey(k);
    }
}

// Outcome of trying to re-derive a live effect ref for an open window this tick.
enum class EffectResolution {
    Alive,         // found our effect on its live, comp-resident layer; outH set (caller disposes)
    ConfirmedGone, // the layer is a live comp member but no longer carries our effect -> deleted
    CannotVerify,  // couldn't determine: no/partial context, or the layer is not a current comp
                   // member (layer/comp deleted) - transient until it PERSISTS (see the hook)
};

// Re-derive a FRESH, valid AEGP effect ref for `key`. Liveness is genuine PROJECT/COMP
// MEMBERSHIP, not call success: AE moves a deleted comp OR layer into the undo buffer, where
// a cached AEGP_CompH/AEGP_LayerH keeps resolving ("zombie") so call-failure counting never
// fires. Instead we re-resolve, from stable ids, every tick: the comp by its project-item id
// (ResolveCompByItemID - fails when the comp left the live project), then the layer by its id
// within that comp (AEGP_GetLayerFromLayerID - fails when the layer left the comp), then the
// effect by installed key. Each fails exactly on real deletion and re-succeeds after an undo,
// handing back the CURRENT handle so the write path never touches a stale ref. On
// EffectResolution::Alive, `outH` receives the matched ref and the CALLER MUST dispose it.
// (Multi-instance note: two CG effects on one layer -> first match; non-crashing; documented.)
static EffectResolution ResolveLiveEffect(AEGP_SuiteHandler& sh, cg::editor::InstanceKey key,
                                          AEGP_EffectRefH& outH) {
    outH = nullptr;
    EffectContext ctx;
    {
        std::lock_guard<std::mutex> lk(g_effMutex);
        auto it = g_effectCtx.find(key);
        if (it == g_effectCtx.end()) return EffectResolution::CannotVerify;  // never captured
        ctx = it->second;
    }
    // Partial/failed capture: nothing to match on. Not a confirmed deletion, just
    // unverifiable (the idle hook's counter still closes it if it never recovers).
    if (ctx.compItemID == 0 || ctx.layerID == AEGP_LayerIDVal_NONE ||
        ctx.installedKey == AEGP_InstalledEffectKey_NONE) {
        return EffectResolution::CannotVerify;
    }
    // Comp membership: is our comp still a live project item? (Deleted comp zombies off the
    // live item tree.) Re-resolves the CURRENT comp handle, robust across comp delete+undo.
    AEGP_CompH compH = ResolveCompByItemID(sh, ctx.compItemID);
    if (!compH) return EffectResolution::CannotVerify;  // comp not in the live project -> gone
    // Layer membership within that live comp. Failure == not a member == gone.
    AEGP_LayerH liveLayerH = nullptr;
    if (sh.LayerSuite9()->AEGP_GetLayerFromLayerID(compH, ctx.layerID, &liveLayerH) ||
        !liveLayerH) {
        return EffectResolution::CannotVerify;  // layer not resident -> persistent -> close
    }
    // Enumerate the CURRENT layer's effects (never the cached handle) and match ours.
    A_long num = 0;
    if (sh.EffectSuite5()->AEGP_GetLayerNumEffects(liveLayerH, &num) || num <= 0) {
        return EffectResolution::ConfirmedGone;  // live layer, no effects -> ours is gone
    }
    for (A_long i = 0; i < num; ++i) {
        AEGP_EffectRefH candH = nullptr;
        if (sh.EffectSuite5()->AEGP_GetLayerEffectByIndex(g_aegpId, liveLayerH,
                                                          static_cast<AEGP_EffectIndex>(i), &candH) ||
            !candH) {
            continue;
        }
        AEGP_InstalledEffectKey candKey = AEGP_InstalledEffectKey_NONE;
        A_Err e = sh.EffectSuite5()->AEGP_GetInstalledKeyFromLayerEffect(candH, &candKey);
        if (!e && candKey == ctx.installedKey) {
            outH = candH;  // caller disposes; write/read/render this tick use this live ref
            return EffectResolution::Alive;
        }
        sh.EffectSuite5()->AEGP_DisposeEffect(candH);
    }
    // Live comp-resident layer, but our effect is no longer on it: it was deleted.
    return EffectResolution::ConfirmedGone;
}

// Read one param stream's scalar (one_d) value at t=0 (our value params don't
// time-vary). Returns false on any AEGP failure so the poll degrades gracefully.
static bool ReadStreamOneD(AEGP_SuiteHandler& sh, AEGP_EffectRefH effectH, PF_ParamIndex idx, double& out) {
    AEGP_StreamRefH streamH = nullptr;
    if (sh.StreamSuite5()->AEGP_GetNewEffectStreamByIndex(g_aegpId, effectH, idx, &streamH) || !streamH) {
        return false;
    }
    // Guard the NO_DATA case: if the effect was deleted out from under an open window, a
    // stale effect_ref yields a AEGP_StreamType_NO_DATA stream, and AEGP_GetNewStreamValue
    // on it raises AE's modal "Cannot get AEGP_StreamValue2 for NO_DATA streams" (5027::247)
    // - which no catch(...) can swallow. Reject exactly NO_DATA (the death signal); any
    // other type is left readable so this never regresses a live popup/slider read.
    AEGP_StreamType stype = AEGP_StreamType_NO_DATA;
    if (sh.StreamSuite5()->AEGP_GetStreamType(streamH, &stype) || stype == AEGP_StreamType_NO_DATA) {
        sh.StreamSuite5()->AEGP_DisposeStream(streamH);
        return false;
    }
    AEGP_StreamValue2 val;
    std::memset(&val, 0, sizeof(val));
    A_Time t;
    t.value = 0;
    t.scale = 1;
    A_Err e = sh.StreamSuite5()->AEGP_GetNewStreamValue(g_aegpId, streamH, AEGP_LTimeMode_LayerTime, &t,
                                                        FALSE, &val);
    bool ok = false;
    if (!e) {
        out = val.val.one_d;
        ok = true;
        sh.StreamSuite5()->AEGP_DisposeStreamValue(&val);
    }
    sh.StreamSuite5()->AEGP_DisposeStream(streamH);
    return ok;
}

// Read the CG_RECIPE arb stream (its arbH is an A_Handle to the RecipeData bytes) via
// AEGP, into `out`. Returns false on any AEGP failure. Best-effort: the poll degrades
// to "recipe unchanged" rather than failing if the arb read is unavailable.
static bool ReadRecipeViaAegp(AEGP_SuiteHandler& sh, AEGP_EffectRefH effectH, RecipeData& out) {
    AEGP_StreamRefH streamH = nullptr;
    if (sh.StreamSuite5()->AEGP_GetNewEffectStreamByIndex(g_aegpId, effectH, CG_RECIPE, &streamH) ||
        !streamH) {
        return false;
    }
    AEGP_StreamType stype = AEGP_StreamType_NO_DATA;
    if (sh.StreamSuite5()->AEGP_GetStreamType(streamH, &stype) || stype == AEGP_StreamType_NO_DATA) {
        sh.StreamSuite5()->AEGP_DisposeStream(streamH);
        return false;
    }
    AEGP_StreamValue2 val;
    std::memset(&val, 0, sizeof(val));
    A_Time t;
    t.value = 0;
    t.scale = 1;
    bool ok = false;
    if (!sh.StreamSuite5()->AEGP_GetNewStreamValue(g_aegpId, streamH, AEGP_LTimeMode_LayerTime, &t,
                                                   FALSE, &val)) {
        if (val.val.arbH) {
            RecipeData* rp = reinterpret_cast<RecipeData*>(
                sh.HandleSuite1()->host_lock_handle(reinterpret_cast<PF_Handle>(val.val.arbH)));
            if (rp) {
                out = *rp;
                ok = true;
                sh.HandleSuite1()->host_unlock_handle(reinterpret_cast<PF_Handle>(val.val.arbH));
            }
        }
        sh.StreamSuite5()->AEGP_DisposeStreamValue(&val);
    }
    sh.StreamSuite5()->AEGP_DisposeStream(streamH);
    return ok;
}

// Read the effect's current value params into a snapshot (the EC->window direction).
static bool PollSnapshotViaAegp(AEGP_SuiteHandler& sh, AEGP_EffectRefH effectH, cg::editor::ParamSnapshot& s) {
    double footage, theme, strength, skin, chroma, lut, exposure, lookMix, temperature;
    if (!ReadStreamOneD(sh, effectH, CG_FOOTAGE_PROFILE, footage)) return false;
    if (!ReadStreamOneD(sh, effectH, CG_THEME, theme)) return false;
    if (!ReadStreamOneD(sh, effectH, CG_STRENGTH, strength)) return false;
    if (!ReadStreamOneD(sh, effectH, CG_SKIN_PROTECTION, skin)) return false;
    if (!ReadStreamOneD(sh, effectH, CG_CHROMA_GAIN, chroma)) return false;
    if (!ReadStreamOneD(sh, effectH, CG_LUT_SOURCE, lut)) return false;
    if (!ReadStreamOneD(sh, effectH, CG_EXPOSURE, exposure)) return false;
    if (!ReadStreamOneD(sh, effectH, CG_LOOK_MIX, lookMix)) return false;
    if (!ReadStreamOneD(sh, effectH, CG_TEMPERATURE, temperature)) return false;
    s.footageProfile = static_cast<int>(footage + 0.5);
    s.theme = static_cast<int>(theme + 0.5);
    s.strength = strength / 100.0;
    s.skinProtection = skin / 100.0;
    s.chromaGain = chroma / 100.0;
    s.lutSource = static_cast<int>(lut + 0.5);
    s.exposure = exposure;
    s.lookMix = lookMix / 100.0;
    s.temperature = temperature;
    // Recipe-backed state (best-effort; keeps the last value if the arb read fails).
    RecipeData recipe;
    if (ReadRecipeViaAegp(sh, effectH, recipe)) {
        s.manual = ManualStateFromRecipe(recipe);
        s.curves = CurvesStateForDisplay(recipe, static_cast<A_long>(theme + 0.5));
        s.wheels = WheelsStateFromRecipe(recipe);
        s.recipeHash = RecipeHash(recipe);
    }
    return true;
}

static bool SnapshotChanged(const cg::editor::ParamSnapshot& a, const cg::editor::ParamSnapshot& b) {
    return a.footageProfile != b.footageProfile || a.theme != b.theme || a.lutSource != b.lutSource ||
           std::fabs(a.strength - b.strength) > 1e-6 ||
           std::fabs(a.skinProtection - b.skinProtection) > 1e-6 ||
           std::fabs(a.chromaGain - b.chromaGain) > 1e-6 ||
           std::fabs(a.exposure - b.exposure) > 1e-6 ||
           std::fabs(a.lookMix - b.lookMix) > 1e-6 ||
           std::fabs(a.temperature - b.temperature) > 1e-6 ||
           a.recipeHash != b.recipeHash || a.manual != b.manual;
}

/* ============= Live preview: layer-frame checkout via AEGP (Phase 4) ======= */
//
// Render the layer frame with an already-configured AEGP_LayerRenderOptionsH into `out`
// (ARGB8 world -> RGBA8, decimated so the long side is <= CG_PREVIEW_MAX_DIM), then check
// the AE frame receipt straight back in - unconditionally, via ScopedCheckin, so an
// exception while copying can never leak the checkout (leaked checkouts destabilize AE).
// Returns false on any AEGP failure; the window then keeps its previous frame.
static bool RenderPreviewFromOptions(AEGP_SuiteHandler& sh, AEGP_LayerRenderOptionsH optsH,
                                     cg::editor::PreviewFrame& out) {
    AEGP_FrameReceiptH receiptH = nullptr;
    // Synchronous checkout on the UI (idle) thread: sanctioned here because the cache
    // makes repeats free and only genuinely new (time/param) frames pay a render. The
    // async variant is a documented future refinement (see native/docs/adr-editor-ui.md).
    if (sh.RenderSuite5()->AEGP_RenderAndCheckoutLayerFrame(optsH, nullptr, nullptr, &receiptH) ||
        !receiptH) {
        return false;
    }
    auto checkin = cg::editor::makeScopedCheckin(
        [&] { sh.RenderSuite5()->AEGP_CheckinFrame(receiptH); });

    AEGP_WorldH worldH = nullptr;
    if (sh.RenderSuite5()->AEGP_GetReceiptWorld(receiptH, &worldH) || !worldH) return false;

    AEGP_WorldSuite3* ws = sh.WorldSuite3();
    AEGP_WorldType wt = AEGP_WorldType_NONE;
    if (ws->AEGP_GetType(worldH, &wt) || wt != AEGP_WorldType_8) return false;  // we asked for 8-bit

    A_long w = 0, h = 0;
    A_u_long rowBytes = 0;
    PF_Pixel8* base = nullptr;
    if (ws->AEGP_GetSize(worldH, &w, &h) || w <= 0 || h <= 0) return false;
    if (ws->AEGP_GetRowBytes(worldH, &rowBytes)) return false;
    if (ws->AEGP_GetBaseAddr8(worldH, &base) || !base) return false;

    // Nearest-neighbour decimation to cap the CPU frame (and thus texture + cache memory)
    // at CG_PREVIEW_MAX_DIM on the long side. AE already downsampled by CG_PREVIEW_DOWNSAMPLE.
    const int longSide = w > h ? w : h;
    int step = 1;
    if (longSide > CG_PREVIEW_MAX_DIM) step = (longSide + CG_PREVIEW_MAX_DIM - 1) / CG_PREVIEW_MAX_DIM;
    const int outW = (w + step - 1) / step;
    const int outH = (h + step - 1) / step;
    out.width = outW;
    out.height = outH;
    out.rgba.assign(static_cast<size_t>(outW) * outH * 4u, 0);

    for (int y = 0; y < outH; ++y) {
        const PF_Pixel8* srcRow = reinterpret_cast<const PF_Pixel8*>(
            reinterpret_cast<const uint8_t*>(base) + static_cast<size_t>(y * step) * rowBytes);
        uint8_t* dstRow = out.rgba.data() + static_cast<size_t>(y) * outW * 4u;
        for (int x = 0; x < outW; ++x) {
            const PF_Pixel8& p = srcRow[x * step];  // AE PF_Pixel8 is A,R,G,B
            uint8_t* d = dstRow + static_cast<size_t>(x) * 4u;
            d[0] = p.red;
            d[1] = p.green;
            d[2] = p.blue;
            // Force opaque: AE worlds are premultiplied and a footage-clip preview is
            // opaque; drawing straight-alpha premult pixels would darken any partial-alpha
            // edges against the pane. (Partial-alpha layers look slightly off - accepted.)
            d[3] = 255;
        }
    }
    return true;  // checkin fires here as `checkin` unwinds
}

// Drive the live preview for one open window: read the current frame time, decide via the
// pure state machine whether the window is already current / a cached frame serves / a
// fresh AE render is needed, and act. UI-thread only (AEGP_NewFromDownstreamOfEffect
// requires it - the idle hook satisfies that). Best-effort: any AEGP failure just leaves
// the last frame up.
static void DrivePreviewForKey(AEGP_SuiteHandler& sh, cg::editor::InstanceKey key,
                               AEGP_EffectRefH effectH) {
    AEGP_LayerRenderOptionsSuite2* lro = sh.LayerRenderOptionsSuite2();
    AEGP_LayerRenderOptionsH optsH = nullptr;
    // "Downstream of effect" = the layer output INCLUDING this effect, so the checked-out
    // pixels are already decoded + graded. AEGP_NewFrom* seeds the options' Time to the
    // layer's current time (the scrub position), so we read it back for the cache key.
    if (lro->AEGP_NewFromDownstreamOfEffect(g_aegpId, effectH, &optsH) || !optsH) return;
    bool disposed = false;
    auto dispose = [&] {
        if (!disposed) {
            lro->AEGP_Dispose(optsH);
            disposed = true;
        }
    };

    A_Time t;
    AEFX_CLR_STRUCT(t);
    t.scale = 1;
    if (lro->AEGP_GetTime(optsH, &t)) { dispose(); return; }

    // Fingerprint the grade-affecting params from the last polled snapshot (the values the
    // window shows, read straight from the effect's streams by the poll above).
    cg::editor::ParamSnapshot ps;
    auto pit = g_lastPolled.find(key);
    if (pit != g_lastPolled.end()) ps = pit->second;
    cg::editor::PreviewKey pk;
    pk.timeValue = static_cast<int64_t>(t.value);
    pk.timeScale = t.scale ? static_cast<uint32_t>(t.scale) : 1u;
    pk.paramFingerprint = cg::editor::previewParamFingerprint(
        ps.footageProfile, ps.theme, ps.lutSource, ps.strength, ps.skinProtection, ps.chromaGain,
        ps.exposure, ps.lookMix, ps.temperature, ps.recipeHash);

    PreviewDriver& drv = g_previewDrivers[key];
    cg::editor::PreviewAction action =
        cg::editor::decidePreviewAction(drv.hasLast, drv.lastKey, pk, drv.cache.contains(pk));

    if (action == cg::editor::PreviewAction::UpToDate) { dispose(); return; }

    if (action == cg::editor::PreviewAction::ServeCached) {
        auto f = drv.cache.get(pk);
        if (f) {
            cg::editor::EditorHost::instance().publishPreviewFrame(key, f);
            drv.lastKey = pk;
            drv.hasLast = true;
        }
        dispose();
        return;
    }

    // Render a fresh frame at reduced resolution, 8-bit (uniform RGBA upload path).
    lro->AEGP_SetWorldType(optsH, AEGP_WorldType_8);
    lro->AEGP_SetDownsampleFactor(optsH, CG_PREVIEW_DOWNSAMPLE, CG_PREVIEW_DOWNSAMPLE);
    auto frame = std::make_shared<cg::editor::PreviewFrame>();
    bool ok = RenderPreviewFromOptions(sh, optsH, *frame);
    dispose();
    if (ok && frame->valid()) {
        std::shared_ptr<const cg::editor::PreviewFrame> cf = frame;
        drv.cache.put(pk, cf);
        cg::editor::EditorHost::instance().publishPreviewFrame(key, cf);
    }
    // Record the attempted key as current EITHER WAY. On success the window now shows it;
    // on failure this suppresses an immediate re-render of the same key next tick (a
    // persistently-failing checkout would otherwise drive a hot synchronous render every
    // idle tick). The next genuine scrub/param change moves the key and retries.
    drv.lastKey = pk;
    drv.hasLast = true;
}

/* ============= Phase 5: in-effect analysis + before/after (AEGP) =========== */
//
// All of this runs on AE's main thread from the idle hook (AEGP_NewFromUpstreamOfEffect is
// UI-thread only, exactly like the Phase 4 downstream checkout). It NEVER runs on the
// editor window's own D3D UI thread, so the editor stays responsive; and each idle tick
// does at most ONE upstream checkout, so AE stays responsive across a multi-frame job.

// Check out the layer frame UPSTREAM of this effect (the raw footage input) at `atTime`
// (or the seeded current time when atTime is null), as a 32-bit float world, and hand the
// pixels to `fn(base, w, h, rowBytes)`. The receipt is checked back in unconditionally via
// ScopedCheckin (a leaked checkout destabilizes AE - load-bearing). Returns false on any
// AEGP failure. UI/idle-thread only.
template <typename Fn>
static bool WithUpstreamFloatFrame(AEGP_SuiteHandler& sh, AEGP_EffectRefH effectH,
                                   const cg::editor::SampleTime* atTime, int downsample, Fn&& fn) {
    AEGP_LayerRenderOptionsSuite2* lro = sh.LayerRenderOptionsSuite2();
    AEGP_LayerRenderOptionsH optsH = nullptr;
    if (lro->AEGP_NewFromUpstreamOfEffect(g_aegpId, effectH, &optsH) || !optsH) return false;
    bool disposed = false;
    auto dispose = [&] {
        if (!disposed) { lro->AEGP_Dispose(optsH); disposed = true; }
    };
    if (atTime) {
        A_Time t;
        AEFX_CLR_STRUCT(t);
        t.value = static_cast<A_long>(atTime->value);
        t.scale = static_cast<A_u_long>(atTime->scale ? atTime->scale : 1);
        if (lro->AEGP_SetTime(optsH, t)) { dispose(); return false; }
    }
    lro->AEGP_SetWorldType(optsH, AEGP_WorldType_32);
    if (downsample > 1) lro->AEGP_SetDownsampleFactor(optsH, downsample, downsample);

    AEGP_FrameReceiptH receiptH = nullptr;
    if (sh.RenderSuite5()->AEGP_RenderAndCheckoutLayerFrame(optsH, nullptr, nullptr, &receiptH) ||
        !receiptH) {
        dispose();
        return false;
    }
    auto checkin = cg::editor::makeScopedCheckin(
        [&] { sh.RenderSuite5()->AEGP_CheckinFrame(receiptH); });

    AEGP_WorldH worldH = nullptr;
    if (sh.RenderSuite5()->AEGP_GetReceiptWorld(receiptH, &worldH) || !worldH) { dispose(); return false; }
    AEGP_WorldSuite3* ws = sh.WorldSuite3();
    AEGP_WorldType wt = AEGP_WorldType_NONE;
    if (ws->AEGP_GetType(worldH, &wt) || wt != AEGP_WorldType_32) { dispose(); return false; }
    A_long w = 0, h = 0;
    A_u_long rowBytes = 0;
    PF_PixelFloat* base = nullptr;
    if (ws->AEGP_GetSize(worldH, &w, &h) || w <= 0 || h <= 0) { dispose(); return false; }
    if (ws->AEGP_GetRowBytes(worldH, &rowBytes)) { dispose(); return false; }
    if (ws->AEGP_GetBaseAddr32(worldH, &base) || !base) { dispose(); return false; }

    fn(static_cast<const PF_PixelFloat*>(base), static_cast<int>(w), static_cast<int>(h),
       static_cast<size_t>(rowBytes));
    dispose();
    return true;  // checkin fires as `checkin` unwinds
}

// Decode one upstream float pixel to Rec.709 via the footage profile - the SAME decode the
// Auto bake composes (Rec.709 -> identity, V-Log -> log decode), so measured stats match
// the applied grade and V-Log is never measured as raw log. Values clamped to [0,1].
static inline cg::core::Vec3d DecodeUpstreamPixel(const PF_PixelFloat& p, const cg::core::LogProfile& profile) {
    const cg::core::Vec3d x{cg::core::clamp01(p.red), cg::core::clamp01(p.green),
                            cg::core::clamp01(p.blue)};
    return cg::core::decodePixelToRec709(x, profile);
}

// Append one analysis frame's decoded RGB (decimated to <= perFrameBudget pixels) to `out`.
static void AppendDecodedFrame(const PF_PixelFloat* base, int w, int h, size_t rowBytes,
                               const cg::core::LogProfile& profile, int perFrameBudget,
                               std::vector<float>& out) {
    const int step = cg::editor::decimationStride(w, h, perFrameBudget);
    for (int y = 0; y < h; y += step) {
        const PF_PixelFloat* row =
            reinterpret_cast<const PF_PixelFloat*>(reinterpret_cast<const uint8_t*>(base) + static_cast<size_t>(y) * rowBytes);
        for (int x = 0; x < w; x += step) {
            const cg::core::Vec3d d = DecodeUpstreamPixel(row[x], profile);
            out.push_back(static_cast<float>(d[0]));
            out.push_back(static_cast<float>(d[1]));
            out.push_back(static_cast<float>(d[2]));
        }
    }
}

// Publish the analysis status to the window (Idle / Sampling n/N / Analyzed[+cache]).
static void PublishAnalysisStatus(cg::editor::InstanceKey key, cg::editor::AnalysisStatus::State st,
                                  int sampled, int total, bool fromCache) {
    cg::editor::AnalysisStatus s;
    s.state = st;
    s.sampled = sampled;
    s.total = total;
    s.fromCache = fromCache;
    cg::editor::EditorHost::instance().publishAnalysisStatus(key, s);
}

// Store measured stats for `key`+`fp` and force the active comp to re-render so the Auto
// grade (and the preview) pick them up. Called on the main thread when a job finalizes or a
// cached result is reused.
static void CommitAnalyzedStats(AEGP_SuiteHandler& sh, cg::editor::InstanceKey key,
                                const cg::editor::AnalysisFingerprint& fp,
                                const cg::core::StatsData& stats) {
    {
        std::lock_guard<std::mutex> lk(g_analyzedMutex);
        AnalyzedStats& a = g_analyzedStats[key];
        a.valid = true;
        a.fp = fp;
        a.stats = stats;
    }
    // Re-render the active item so the new source stats flow into the Auto bake at render.
    try { sh.AdvItemSuite1()->PF_TouchActiveItem(); } catch (...) { /* best-effort */ }
}

// Resolve the analysis fingerprint (footage profile + the layer's clip span) for `key`. The
// layer is re-resolved from the stored stable ids (comp item id + layer id) via the live
// project tree - the same robust path ResolveLiveEffect uses - so no stale handle is touched.
// Returns false if the context is missing or the layer timing can't be read.
static bool ResolveAnalysisFingerprint(AEGP_SuiteHandler& sh, cg::editor::InstanceKey key,
                                       int footageProfile, cg::editor::AnalysisFingerprint& out) {
    EffectContext ctx;
    {
        std::lock_guard<std::mutex> lk(g_effMutex);
        auto it = g_effectCtx.find(key);
        if (it == g_effectCtx.end()) return false;
        ctx = it->second;
    }
    if (ctx.compItemID == 0 || ctx.layerID == AEGP_LayerIDVal_NONE) return false;
    AEGP_CompH compH = ResolveCompByItemID(sh, ctx.compItemID);
    if (!compH) return false;
    AEGP_LayerH layerH = nullptr;
    if (sh.LayerSuite9()->AEGP_GetLayerFromLayerID(compH, ctx.layerID, &layerH) || !layerH) return false;
    A_Time inPoint, dur;
    AEFX_CLR_STRUCT(inPoint);
    AEFX_CLR_STRUCT(dur);
    if (sh.LayerSuite9()->AEGP_GetLayerInPoint(layerH, AEGP_LTimeMode_LayerTime, &inPoint)) return false;
    if (sh.LayerSuite9()->AEGP_GetLayerDuration(layerH, AEGP_LTimeMode_LayerTime, &dur)) return false;
    out.footageProfile = footageProfile;
    out.inPointValue = static_cast<int64_t>(inPoint.value);
    out.durationValue = static_cast<int64_t>(dur.value);
    out.scale = dur.scale ? static_cast<uint32_t>(dur.scale) : 1u;
    return true;
}

// Drive the multi-frame analysis for one open window: debounce a footage change, run the
// incremental job one frame per tick, finalize with computeStats, and cache/commit the
// result. Best-effort; any AEGP failure just leaves the prior stats in place.
static void DriveAnalysisForKey(AEGP_SuiteHandler& sh, cg::editor::InstanceKey key,
                                AEGP_EffectRefH effectH, const cg::editor::ParamSnapshot& ps,
                                uint64_t tick) {
    AnalysisRuntime& rt = g_analysisRuntime[key];

    // A running job takes priority: render its next scheduled frame this tick.
    if (rt.job.state() == cg::editor::AnalysisState::Sampling) {
        cg::editor::SampleTime st;
        if (rt.job.nextSample(st)) {
            const cg::core::LogProfile& profile = ProfileFromFootagePopup(ps.footageProfile);
            const int budget = cg::editor::perFramePixelBudget(CG_ANALYSIS_PIXEL_BUDGET,
                                                               static_cast<int>(rt.job.totalFrames()));
            std::vector<float> frameRgb;
            bool ok = WithUpstreamFloatFrame(sh, effectH, &st, CG_ANALYSIS_DOWNSAMPLE,
                [&](const PF_PixelFloat* base, int w, int h, size_t rb) {
                    AppendDecodedFrame(base, w, h, rb, profile, budget, frameRgb);
                });
            if (ok && !frameRgb.empty()) rt.job.addFrame(frameRgb.data(), frameRgb.size());
            else rt.job.advanceSkip();
        }
        if (rt.job.state() == cg::editor::AnalysisState::Sampling) {
            PublishAnalysisStatus(key, cg::editor::AnalysisStatus::State::Sampling,
                                  static_cast<int>(rt.job.sampledFrames()),
                                  static_cast<int>(rt.job.totalFrames()), false);
            return;
        }
        // Job just finished this tick.
        if (rt.job.state() == cg::editor::AnalysisState::Ready) {
            try {
                cg::core::FootageStats fs = cg::core::computeStats(rt.job.pixels().data(),
                                                                   rt.job.pixels().size());
                cg::core::StatsData sd = cg::core::statsToData(fs);
                rt.cachePut(rt.job.fingerprint(), sd);
                rt.debounce.accept(rt.job.fingerprint());
                CommitAnalyzedStats(sh, key, rt.job.fingerprint(), sd);
                PublishAnalysisStatus(key, cg::editor::AnalysisStatus::State::Analyzed,
                                      static_cast<int>(rt.job.totalFrames()),
                                      static_cast<int>(rt.job.totalFrames()), false);
            } catch (...) { /* computeStats threw (empty) - keep prior stats */ }
        }
        rt.job.reset();
        return;
    }

    // No job running: decide whether to start one.
    cg::editor::AnalysisFingerprint fp;
    if (!ResolveAnalysisFingerprint(sh, key, ps.footageProfile, fp)) return;

    const bool forced = cg::editor::EditorHost::instance().consumeAnalyzeRequest(key);

    // A cached result for this exact fingerprint is reused instantly (unless the user forced
    // a fresh run) - no re-checkout.
    if (!forced) {
        if (const cg::core::StatsData* cached = rt.cacheFind(fp)) {
            if (!rt.debounce.hasAccepted() || rt.debounce.accepted() != fp) {
                rt.debounce.accept(fp);
                CommitAnalyzedStats(sh, key, fp, *cached);
            }
            PublishAnalysisStatus(key, cg::editor::AnalysisStatus::State::Analyzed,
                                  CG_ANALYSIS_FRAMES, CG_ANALYSIS_FRAMES, true);
            return;
        }
    }

    // Debounce a genuine change (or start immediately on an explicit Analyze click).
    const bool start = forced || rt.debounce.observe(fp, tick);
    if (start) {
        rt.job.begin(fp, cg::editor::frameSampleSchedule(fp.inPointValue, fp.durationValue,
                                                         fp.scale, CG_ANALYSIS_FRAMES));
        PublishAnalysisStatus(key, cg::editor::AnalysisStatus::State::Sampling, 0,
                              CG_ANALYSIS_FRAMES, false);
    } else if (!rt.debounce.hasAccepted()) {
        PublishAnalysisStatus(key, cg::editor::AnalysisStatus::State::Idle, 0, CG_ANALYSIS_FRAMES,
                              false);
    }
}

// Drive the "before" frame (the decoded ORIGINAL) for the split/before compare modes. Only
// runs when the window's compare mode needs it. Checks out the upstream frame at the current
// time, decodes it to Rec.709 (V-Log decoded - never raw log), and publishes it, caching by
// (time + footage) so toggling compare modes is instant. Best-effort.
static void DriveBeforeFrameForKey(AEGP_SuiteHandler& sh, cg::editor::InstanceKey key,
                                   AEGP_EffectRefH effectH, const cg::editor::ParamSnapshot& ps) {
    if (!cg::editor::EditorHost::instance().wantsBeforeFrame(key)) return;

    // Read the current (seeded) upstream time for the cache key without rendering yet.
    AEGP_LayerRenderOptionsSuite2* lro = sh.LayerRenderOptionsSuite2();
    AEGP_LayerRenderOptionsH optsH = nullptr;
    if (lro->AEGP_NewFromUpstreamOfEffect(g_aegpId, effectH, &optsH) || !optsH) return;
    A_Time t;
    AEFX_CLR_STRUCT(t);
    t.scale = 1;
    A_Err te = lro->AEGP_GetTime(optsH, &t);
    lro->AEGP_Dispose(optsH);
    if (te) return;

    AnalysisRuntime& rt = g_analysisRuntime[key];
    cg::editor::PreviewKey bk;
    bk.timeValue = static_cast<int64_t>(t.value);
    bk.timeScale = t.scale ? static_cast<uint32_t>(t.scale) : 1u;
    // The before frame depends only on the footage decode, not the grade knobs.
    bk.paramFingerprint = cg::editor::previewParamFingerprint(ps.footageProfile, 0, 0, 0, 0, 0);

    if (rt.beforeHasLast && rt.beforeLastKey == bk) return;  // already showing this before frame
    if (auto cached = rt.beforeCache.get(bk)) {
        cg::editor::EditorHost::instance().publishBeforeFrame(key, cached);
        rt.beforeLastKey = bk;
        rt.beforeHasLast = true;
        return;
    }

    const cg::core::LogProfile& profile = ProfileFromFootagePopup(ps.footageProfile);
    auto frame = std::make_shared<cg::editor::PreviewFrame>();
    bool ok = WithUpstreamFloatFrame(sh, effectH, nullptr, CG_PREVIEW_DOWNSAMPLE,
        [&](const PF_PixelFloat* base, int w, int h, size_t rb) {
            const int longSide = w > h ? w : h;
            int stepd = 1;
            if (longSide > CG_PREVIEW_MAX_DIM)
                stepd = (longSide + CG_PREVIEW_MAX_DIM - 1) / CG_PREVIEW_MAX_DIM;
            const int outW = (w + stepd - 1) / stepd;
            const int outH = (h + stepd - 1) / stepd;
            frame->width = outW;
            frame->height = outH;
            frame->rgba.assign(static_cast<size_t>(outW) * outH * 4u, 255);
            for (int y = 0; y < outH; ++y) {
                const PF_PixelFloat* srcRow = reinterpret_cast<const PF_PixelFloat*>(
                    reinterpret_cast<const uint8_t*>(base) + static_cast<size_t>(y * stepd) * rb);
                uint8_t* dstRow = frame->rgba.data() + static_cast<size_t>(y) * outW * 4u;
                for (int x = 0; x < outW; ++x) {
                    const cg::core::Vec3d d = DecodeUpstreamPixel(srcRow[x * stepd], profile);
                    uint8_t* px = dstRow + static_cast<size_t>(x) * 4u;
                    px[0] = static_cast<uint8_t>(d[0] * 255.0 + 0.5);
                    px[1] = static_cast<uint8_t>(d[1] * 255.0 + 0.5);
                    px[2] = static_cast<uint8_t>(d[2] * 255.0 + 0.5);
                    px[3] = 255;
                }
            }
        });
    if (ok && frame->valid()) {
        std::shared_ptr<const cg::editor::PreviewFrame> cf = frame;
        rt.beforeCache.put(bk, cf);
        cg::editor::EditorHost::instance().publishBeforeFrame(key, cf);
        rt.beforeLastKey = bk;
        rt.beforeHasLast = true;
    }
}

// The AEGP idle hook: main thread, fires while AE is idle. Cheap-noops unless a window
// has pending edits, so it never burdens AE. Applies drained edits inside one undo
// group per tick, polls Effect Controls -> window on change, and reaps effect refs
// whose window has been closed.
static A_Err CG_IdleHook(AEGP_GlobalRefcon, AEGP_IdleRefcon, A_long* max_sleepPL) {
    auto& host = cg::editor::EditorHost::instance();
    std::vector<cg::editor::InstanceKey> keys = host.openKeys();
    const uint64_t tick = g_idleTick.fetch_add(1);  // monotonic tick for the analysis debounce

    // Forget captured context whose window is gone (user closed it). No AEGP handle to
    // dispose (layerH is not owned); this just frees the map slot.
    {
        std::vector<cg::editor::InstanceKey> orphans;
        {
            std::lock_guard<std::mutex> lk(g_effMutex);
            for (auto& kv : g_effectCtx) {
                if (std::find(keys.begin(), keys.end(), kv.first) == keys.end()) orphans.push_back(kv.first);
            }
        }
        for (auto k : orphans) {
            ForgetEffectContextForKey(k);
            g_lastPolled.erase(k);
            g_verifyFailures.erase(k);
            g_previewDrivers.erase(k);  // free the closed window's cached preview frames
            ForgetAnalysisStateForKey(k);
        }
    }

    if (g_spbasic && g_aegpId) {
        for (auto key : keys) {
            // Re-derive a FRESH, valid effect ref by enumerating the live layer's effects
            // (never dereference a possibly-stale captured ref - that crashes AE when the
            // effect was deleted). The tri-state resolution decides how to react:
            //   Alive         -> use the fresh handle this tick; reset the failure counter.
            //   ConfirmedGone -> our effect is gone from its live comp-resident layer: close
            //                    the window promptly (Phase 3 "delete effect -> window closes").
            //   CannotVerify  -> the layer is not a current comp member (layer/comp deleted or
            //                    source removed), or the capture was partial. A single such
            //                    tick is treated as transient and never force-closes a live
            //                    window; only a PERSISTENT run (>= CG_VERIFY_FAIL_LIMIT) closes
            //                    the lingering window.
            // In BOTH not-Alive cases the effect is not confirmed live, so we drop any edits
            // the (now-orphaned) window queued rather than writing them onto a stale/absent
            // effect - which is what replayed the "internal verification" modal after a
            // delete+undo. Writes only ever happen in the Alive branch, on the fresh ref.
            auto closeAndForget = [&](cg::editor::InstanceKey k) {
                host.close(k);
                ForgetEffectContextForKey(k);
                g_lastPolled.erase(k);
                g_verifyFailures.erase(k);
                g_previewDrivers.erase(k);
                ForgetAnalysisStateForKey(k);
            };
            AEGP_EffectRefH effectH = nullptr;
            EffectResolution res = EffectResolution::CannotVerify;
            try {
                AEGP_SuiteHandler sh(g_spbasic);
                res = ResolveLiveEffect(sh, key, effectH);
            } catch (...) { res = EffectResolution::CannotVerify; effectH = nullptr; }

            if (res == EffectResolution::ConfirmedGone) {
                closeAndForget(key);
                continue;
            }
            if (res == EffectResolution::CannotVerify) {
                (void)host.drainEdits(key);  // discard: never write to an unconfirmed effect
                int fails = ++g_verifyFailures[key];
                if (fails >= CG_VERIFY_FAIL_LIMIT) closeAndForget(key);
                continue;  // transient: leave the window open and retry next tick
            }
            g_verifyFailures[key] = 0;  // Alive: clear any accrued transient failures
            // Dispose the fresh ref no matter how we leave this iteration.
            auto effGuard = cg::editor::makeScopedCheckin([&] {
                try { AEGP_SuiteHandler sh(g_spbasic); sh.EffectSuite5()->AEGP_DisposeEffect(effectH); }
                catch (...) {}
            });

            // window -> effect: apply any queued edits (writes param streams).
            if (host.hasPendingEdits(key)) {
                std::vector<cg::editor::ParamEdit> edits = host.drainEdits(key);
                if (!edits.empty()) {
                    try {
                        AEGP_SuiteHandler sh(g_spbasic);
                        sh.UtilitySuite6()->AEGP_StartUndoGroup("Color Grade editor edit");
                        for (const auto& e : edits) {
                            // Recipe-backed controls (manual / curves / wheels): read-modify-write
                            // the CG_RECIPE arb stream (its arbH is an A_Handle to the RecipeData
                            // bytes; lock via the PF HandleSuite, mutate the relevant fields, set
                            // it back). Kept out of the scalar StreamForEdit path.
                            if (cg::editor::isRecipeBackedField(e.field)) {
                                AEGP_StreamRefH rstreamH = nullptr;
                                if (sh.StreamSuite5()->AEGP_GetNewEffectStreamByIndex(
                                        g_aegpId, effectH, CG_RECIPE, &rstreamH) ||
                                    !rstreamH) {
                                    continue;
                                }
                                AEGP_StreamValue2 rval;
                                std::memset(&rval, 0, sizeof(rval));
                                A_Time zt;
                                zt.value = 0;
                                zt.scale = 1;
                                if (!sh.StreamSuite5()->AEGP_GetNewStreamValue(
                                        g_aegpId, rstreamH, AEGP_LTimeMode_LayerTime, &zt, FALSE, &rval)) {
                                    if (rval.val.arbH) {
                                        RecipeData* rp = reinterpret_cast<RecipeData*>(
                                            sh.HandleSuite1()->host_lock_handle(
                                                reinterpret_cast<PF_Handle>(rval.val.arbH)));
                                        if (rp) {
                                            ApplyRecipeEditToRecipe(e, *rp);
                                            sh.HandleSuite1()->host_unlock_handle(
                                                reinterpret_cast<PF_Handle>(rval.val.arbH));
                                            sh.StreamSuite5()->AEGP_SetStreamValue(g_aegpId, rstreamH, &rval);
                                        }
                                    }
                                    sh.StreamSuite5()->AEGP_DisposeStreamValue(&rval);
                                }
                                sh.StreamSuite5()->AEGP_DisposeStream(rstreamH);
                                continue;
                            }
                            PF_ParamIndex idx = 0;
                            double one_d = 0.0;
                            if (!StreamForEdit(e, idx, one_d)) continue;
                            AEGP_StreamRefH streamH = nullptr;
                            A_Err er = sh.StreamSuite5()->AEGP_GetNewEffectStreamByIndex(g_aegpId, effectH, idx, &streamH);
                            if (!er && streamH) {
                                AEGP_StreamValue2 val;
                                std::memset(&val, 0, sizeof(val));
                                val.streamH = streamH;
                                val.val.one_d = one_d;
                                sh.StreamSuite5()->AEGP_SetStreamValue(g_aegpId, streamH, &val);
                                sh.StreamSuite5()->AEGP_DisposeStream(streamH);
                            }
                        }
                        sh.UtilitySuite6()->AEGP_EndUndoGroup();
                    } catch (...) { /* swallow: a bridge hiccup must never destabilize AE */ }
                }
            }

            // Effect Controls -> window: read the effect's current params and publish
            // to the window if they changed (scrub/type in EC reflects live). This does
            // not rely on the render-thread publish, whose sequence-data key is
            // unreliable; the mid-drag guard in the window keeps a user drag from being
            // stomped by a poll.
            try {
                AEGP_SuiteHandler sh(g_spbasic);
                cg::editor::ParamSnapshot polled;
                auto seed = g_lastPolled.find(key);
                if (seed != g_lastPolled.end()) polled = seed->second;
                if (PollSnapshotViaAegp(sh, effectH, polled)) {
                    auto it = g_lastPolled.find(key);
                    if (it == g_lastPolled.end() || SnapshotChanged(it->second, polled)) {
                        polled.revision = g_snapshotRevision.fetch_add(1);
                        g_lastPolled[key] = polled;
                        host.publishSnapshot(key, polled);
                    }
                }
            } catch (...) { /* swallow */ }

            // Live preview (Phase 4): keep the window's centered clip frame current with
            // the scrub position + params. Runs after the poll so the fingerprint uses
            // the freshly-read params. Guarded independently so a preview hiccup never
            // affects the edit/poll paths above.
            try {
                AEGP_SuiteHandler sh(g_spbasic);
                DrivePreviewForKey(sh, key, effectH);
            } catch (...) { /* swallow: preview must never destabilize AE */ }

            // The current params for the analysis / before-frame drivers (the poll above
            // keeps g_lastPolled[key] up to date; default snapshot if not yet polled).
            cg::editor::ParamSnapshot cur;
            {
                auto it = g_lastPolled.find(key);
                if (it != g_lastPolled.end()) cur = it->second;
            }

            // In-effect analysis (Phase 5): debounced multi-frame source-stats measurement,
            // one upstream checkout per tick. Guarded independently.
            try {
                AEGP_SuiteHandler sh(g_spbasic);
                DriveAnalysisForKey(sh, key, effectH, cur, tick);
            } catch (...) { /* swallow: analysis must never destabilize AE */ }

            // Before/after (Phase 5): keep the decoded ORIGINAL frame current for the
            // split/before compare modes (only when the window asks for it).
            try {
                AEGP_SuiteHandler sh(g_spbasic);
                DriveBeforeFrameForKey(sh, key, effectH, cur);
            } catch (...) { /* swallow: before frame must never destabilize AE */ }
        }
    }
    // With a window open, poll promptly (sub-second) so EC edits track; otherwise let
    // AE sleep normally. Units are 1/60 s.
    if (!keys.empty() && max_sleepPL && *max_sleepPL > 8) *max_sleepPL = 8;
    return A_Err_NONE;
}

// Register our AEGP id + the global idle hook once (via PICA; no separate AEGP plugin).
// Best-effort: on a host without AEGP (e.g. Premiere) the idle driver is simply absent.
static void EnsureAegpIdleHook(PF_InData* in_data) {
    if (g_idleHookRegistered) return;
    g_spbasic = in_data->pica_basicP;
    if (!g_spbasic) return;
    try {
        AEGP_SuiteHandler sh(g_spbasic);
        if (!g_aegpId) sh.UtilitySuite6()->AEGP_RegisterWithAEGP(nullptr, "ColorGradeFX", &g_aegpId);
        sh.RegisterSuite5()->AEGP_RegisterIdleHook(g_aegpId, CG_IdleHook, nullptr);
        g_idleHookRegistered = true;
    } catch (...) { /* no AEGP here - window->effect writes fall back to the param-change drain */ }
}

/* =============================== Setup =================================== */

static PF_Err About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    PF_SPRINTF(out_data->return_msg, "%s, v%d.%d\r%s", NAME, MAJOR_VERSION, MINOR_VERSION, DESCRIPTION);
    return PF_Err_NONE;
}

static PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION, STAGE_VERSION, BUILD_VERSION);

    out_data->out_flags = PF_OutFlag_PIX_INDEPENDENT | PF_OutFlag_DEEP_COLOR_AWARE;

    out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE |
                           PF_OutFlag2_SUPPORTS_SMART_RENDER |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

#if HAS_ANY_GPU
    // GPU support is advertised only outside Premiere. GPU_RENDER_F32 declares the effect is
    // GPU-capable (covers CUDA); the DirectX flag additionally opts into that framework. These
    // must match the PiPL's Global_OutFlags_2 or AE silently falls back to CPU.
    if (in_data->appl_id != 'PrMr') {
        out_data->out_flags2 |= PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
#if HAS_HLSL
        out_data->out_flags2 |= PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING;
#endif
    }
#endif

    // Register the AEGP idle hook (the main-thread driver that applies window edits).
    EnsureAegpIdleHook(in_data);
    return PF_Err_NONE;
}

// AE process teardown: close every editor window so no orphan window survives a
// project close / AE quit (Phase 3 lifecycle requirement).
static PF_Err GlobalSetdown(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    cg::editor::EditorHost::instance().shutdownAll();
    return PF_Err_NONE;
}

// Sequence data holds this instance's stable window-registry uid. Flat POD, so no
// flatten/unflatten needed; RESETUP reseeds the uid so a duplicated or reloaded effect
// never shares a key with its source. Mirrors the SDK Gamma_Table lifecycle.
static PF_Err SequenceSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    if (out_data->sequence_data) PF_DISPOSE_HANDLE(out_data->sequence_data);
    PF_Handle h = PF_NEW_HANDLE(sizeof(CG_SequenceData));
    if (!h) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    CG_SequenceData* sd = reinterpret_cast<CG_SequenceData*>(PF_LOCK_HANDLE(h));
    if (sd) {
        sd->magic = CG_SEQ_MAGIC;
        sd->version = CG_SEQ_VERSION;
        PackUid(sd, GenUid());
        PF_UNLOCK_HANDLE(h);
    }
    out_data->sequence_data = h;
    return PF_Err_NONE;
}

static PF_Err SequenceResetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output) {
    if (!in_data->sequence_data) return SequenceSetup(in_data, out_data, params, output);
    // Reseed with a fresh uid so a duplicated/reloaded instance gets its own key.
    CG_SequenceData* sd = reinterpret_cast<CG_SequenceData*>(PF_LOCK_HANDLE(in_data->sequence_data));
    if (sd) {
        sd->magic = CG_SEQ_MAGIC;
        sd->version = CG_SEQ_VERSION;
        PackUid(sd, GenUid());
        PF_UNLOCK_HANDLE(in_data->sequence_data);
    }
    out_data->sequence_data = in_data->sequence_data;
    return PF_Err_NONE;
}

// Effect instance removed (deleted from the layer, comp closed): close its window,
// forget its captured effect context, and free the sequence data.
static PF_Err SequenceSetdown(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    const cg::editor::InstanceKey key = SeqKey(in_data);
    cg::editor::EditorHost::instance().close(key);
    ForgetEffectContextForKey(key);
    g_verifyFailures.erase(key);
    ForgetAnalysisStateForKey(key);
    if (in_data->sequence_data) {
        PF_DISPOSE_HANDLE(in_data->sequence_data);
        out_data->sequence_data = NULL;
    }
    return PF_Err_NONE;
}

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    // Order must match the CG_* param enum (after CG_INPUT).
    // Correct: footage log-format selector (drives the decode stage baked into the
    // Auto LUT). Rec.709 = no decode; V-Log decodes to Rec.709 before the grade.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Footage",
                 2 /* num choices */,
                 CG_FOOT_REC709,
                 CG_FOOTAGE_CHOICES,
                 CG_FOOTAGE_PROFILE);

    // SUPERVISE so AE sends PF_Cmd_USER_CHANGED_PARAM when the theme changes; that
    // handler resets the Strength/Skin sliders to the new theme's authored knobs.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Theme",
                  CG_THEME_COUNT /* num choices */,
                  CG_THEME_TEAL,
                  CG_THEME_CHOICES,
                  PF_ParamFlag_SUPERVISE,
                  CG_THEME);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Strength",
                         CG_STRENGTH_MIN, CG_STRENGTH_MAX,
                         CG_STRENGTH_MIN, CG_STRENGTH_MAX,
                         CG_STRENGTH_DFLT,
                         1, PF_ValueDisplayFlag_PERCENT, 0,
                         CG_STRENGTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Skin Protection",
                         CG_SKIN_MIN, CG_SKIN_MAX,
                         CG_SKIN_MIN, CG_SKIN_MAX,
                         CG_SKIN_DFLT,
                         1, PF_ValueDisplayFlag_PERCENT, 0,
                         CG_SKIN_PROTECTION);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Chroma Gain",
                         CG_CHROMA_MIN, CG_CHROMA_MAX,
                         CG_CHROMA_MIN, CG_CHROMA_SLIDER_MAX,
                         CG_CHROMA_DFLT,
                         1, PF_ValueDisplayFlag_PERCENT, 0,
                         CG_CHROMA_GAIN);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("LUT Source",
                 3 /* num choices */,
                 CG_SRC_AUTO,
                 CG_LUT_SOURCE_CHOICES,
                 CG_LUT_SOURCE);

    // Momentary button opening the native editor window (Phase 3). SUPERVISE so AE
    // sends PF_Cmd_USER_CHANGED_PARAM on click (buttons require it); the handler
    // opens/focuses the single per-instance window.
    AEFX_CLR_STRUCT(def);
    // ASCII "..." - AE param names are narrow (CP-1252), so a UTF-8 ellipsis (E2 80 A6)
    // renders as mojibake ("Open Editorâ€¦"). Round-2 captain finding.
    PF_ADD_BUTTON("Editor",
                  "Open Editor...",
                  0,
                  PF_ParamFlag_SUPERVISE,
                  CG_OPEN_EDITOR);

    // Grade recipe: measured stats + full typed knob space, persisted as arb-data.
    // No custom UI (data only, seeded from the theme); AE stores it in the project.
    AEFX_CLR_STRUCT(def);
    ERR(CreateDefaultRecipe(in_data, &def.u.arb_d.dephault));
    if (!err) {
        // Data-only arb param: ui_width/height MUST be 0 and PF_PUI_NO_ECW_UI set,
        // or AE warns "no custom ui outflag, but param has ui_width/height..."
        // (25::37) and "Unsupported effect control!". NO_ECW_UI needs no
        // PF_OutFlag_CUSTOM_UI (SDK AE_Effect.h) - the recipe is set programmatically.
        PF_ADD_ARBITRARY2("Grade Recipe",
                          0, 0,
                          PF_ParamFlag_CANNOT_TIME_VARY,
                          PF_PUI_NO_ECW_UI,
                          def.u.arb_d.dephault,
                          CG_ARB_ID,
                          CG_ARB_REFCON);
    }

    // Phase 6a keyframeable params, APPENDED after CG_RECIPE (never inserted): the
    // live value overrides the recipe's manual exposure/temperature and drives Look
    // Mix at bake time (decision D1). Neutral defaults keep a fresh effect identity.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Exposure",
                         CG_EXPOSURE_MIN, CG_EXPOSURE_MAX,
                         CG_EXPOSURE_MIN, CG_EXPOSURE_MAX,
                         CG_EXPOSURE_DFLT,
                         2, PF_ValueDisplayFlag_NONE, 0,
                         CG_EXPOSURE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Look Mix",
                         CG_LOOK_MIX_MIN, CG_LOOK_MIX_MAX,
                         CG_LOOK_MIX_MIN, CG_LOOK_MIX_MAX,
                         CG_LOOK_MIX_DFLT,
                         1, PF_ValueDisplayFlag_PERCENT, 0,
                         CG_LOOK_MIX);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Temperature",
                         CG_TEMPERATURE_MIN, CG_TEMPERATURE_MAX,
                         CG_TEMPERATURE_MIN, CG_TEMPERATURE_MAX,
                         CG_TEMPERATURE_DFLT,
                         1, PF_ValueDisplayFlag_NONE, 0,
                         CG_TEMPERATURE);

    out_data->num_params = CG_NUM_PARAMS;
    return err;
}

// The Theme popup is a supervised preset selector: switching it snaps the Strength
// and Skin Protection sliders to the newly-selected theme's authored knob defaults.
// This intentionally overwrites any manual Strength/Skin adjustments (captain's
// decision - a theme is a preset). The Chroma Gain slider is untouched: it is a
// relative multiplier (100% = the theme's authored chromaGain), so it needs no reset.
static PF_Err UserChangedParam(PF_InData* in_data, PF_OutData* out_data,
                               PF_ParamDef* params[], PF_UserChangedParamExtra* extra) {
    if (extra->param_index == CG_THEME) {
        cg::core::Theme theme = ThemeFromPopup(params[CG_THEME]->u.pd.value);
        params[CG_STRENGTH]->u.fs_d.value = theme.knobs.strengthDefault * 100.0;
        params[CG_STRENGTH]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
        params[CG_SKIN_PROTECTION]->u.fs_d.value = theme.knobs.skinProtectionDefault * 100.0;
        params[CG_SKIN_PROTECTION]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
    } else if (extra->param_index == CG_OPEN_EDITOR) {
        // Open (or focus) the editor window, seeded with the current params so its
        // controls start in sync with Effect Controls. The recipe (manual state) is
        // read straight off the PF arb handle here - reliable at button-open.
        RecipeData seedRecipe = RecipeFromHandle(in_data, params[CG_RECIPE]->u.arb_d.value);
        cg::editor::ParamSnapshot seed = MakeSnapshot(
            params[CG_FOOTAGE_PROFILE]->u.pd.value,
            params[CG_THEME]->u.pd.value,
            params[CG_STRENGTH]->u.fs_d.value / 100.0,
            params[CG_SKIN_PROTECTION]->u.fs_d.value / 100.0,
            params[CG_CHROMA_GAIN]->u.fs_d.value / 100.0,
            params[CG_LUT_SOURCE]->u.pd.value,
            params[CG_EXPOSURE]->u.fs_d.value,
            params[CG_LOOK_MIX]->u.fs_d.value / 100.0,
            params[CG_TEMPERATURE]->u.fs_d.value,
            seedRecipe);
        const cg::editor::InstanceKey key = SeqKey(in_data);
        // Exactly one window per effect: close any orphan window bound to this same effect
        // under a stale key (e.g. from a delete+undo that reseeded the uid) before opening.
        if (g_spbasic && g_aegpId && in_data->effect_ref) {
            try {
                AEGP_SuiteHandler sh(g_spbasic);
                CloseDuplicateWindowsForEffect(sh, in_data->effect_ref, key);
            } catch (...) { /* best-effort */ }
        }
        cg::editor::EditorHost::instance().open(key, seed);
        // Capture this instance's stable identity (comp item id + layer id + installed key)
        // so the idle hook re-resolves a fresh, valid effect ref each tick without ever
        // touching a stale handle (see ResolveLiveEffect).
        CaptureEffectContextForKey(in_data, key);
    }

    // Flush any edits the editor window produced back onto the params (spike driver:
    // any param-change command drains the queue; the production continuous driver is
    // the companion-AEGP idle hook - see adr-editor-ui.md).
    ApplyEditorEdits(in_data, params);
    return PF_Err_NONE;
}

/* ========================= Per-pixel LUT apply =========================== */

// blend: out = in*(1-s) + lut(in)*s, per channel. sampleLut clamps its input to [0,1].
static inline void ApplyLut(const cg::Lut3D& lut, float strength,
                            float inR, float inG, float inB,
                            float& outR, float& outG, float& outB) {
    cg::Vec3 g = cg::sampleLut(lut, {inR, inG, inB});
    outR = inR + (g[0] - inR) * strength;
    outG = inG + (g[1] - inG) * strength;
    outB = inB + (g[2] - inB) * strength;
}

static PF_Err FilterImage32(void* refcon, A_long, A_long, PF_PixelFloat* inP, PF_PixelFloat* outP) {
    CG_RenderData* d = reinterpret_cast<CG_RenderData*>(refcon);
    ApplyLut(d->lut, d->applyStrength, inP->red, inP->green, inP->blue, outP->red, outP->green, outP->blue);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err FilterImage16(void* refcon, A_long, A_long, PF_Pixel16* inP, PF_Pixel16* outP) {
    CG_RenderData* d = reinterpret_cast<CG_RenderData*>(refcon);
    const float k = 32768.0f;  // AE 16-bit max channel value
    float r, g, b;
    ApplyLut(d->lut, d->applyStrength, inP->red / k, inP->green / k, inP->blue / k, r, g, b);
    auto to16 = [k](float v) -> A_u_short {
        float s = v * k + 0.5f;
        if (s < 0.0f) s = 0.0f;
        if (s > k) s = k;
        return static_cast<A_u_short>(s);
    };
    outP->red = to16(r); outP->green = to16(g); outP->blue = to16(b);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err FilterImage8(void* refcon, A_long, A_long, PF_Pixel8* inP, PF_Pixel8* outP) {
    CG_RenderData* d = reinterpret_cast<CG_RenderData*>(refcon);
    const float k = 255.0f;
    float r, g, b;
    ApplyLut(d->lut, d->applyStrength, inP->red / k, inP->green / k, inP->blue / k, r, g, b);
    auto to8 = [k](float v) -> A_u_char {
        float s = v * k + 0.5f;
        if (s < 0.0f) s = 0.0f;
        if (s > k) s = k;
        return static_cast<A_u_char>(s);
    };
    outP->red = to8(r); outP->green = to8(g); outP->blue = to8(b);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

/* ============================ Legacy Render ============================== */
// AE 2025 always uses Smart Render when the flag is set, but implement the classic
// path too so the effect degrades gracefully in any non-smart context.

static PF_Err LegacyRender(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output) {
    PF_Err err = PF_Err_NONE;
    CG_RenderData data;
    const double strength01 = params[CG_STRENGTH]->u.fs_d.value / 100.0;
    const double skin01 = params[CG_SKIN_PROTECTION]->u.fs_d.value / 100.0;
    const double chromaGain = params[CG_CHROMA_GAIN]->u.fs_d.value / 100.0;
    const A_long theme = params[CG_THEME]->u.pd.value;
    const A_long footage = params[CG_FOOTAGE_PROFILE]->u.pd.value;
    const A_long source = params[CG_LUT_SOURCE]->u.pd.value;
    const double exposure = params[CG_EXPOSURE]->u.fs_d.value;
    const double lookMix = params[CG_LOOK_MIX]->u.fs_d.value / 100.0;
    const double temperature = params[CG_TEMPERATURE]->u.fs_d.value;
    RecipeData recipe = RecipeFromHandle(in_data, params[CG_RECIPE]->u.arb_d.value);
    InjectAnalyzedStats(SeqKey(in_data), footage, recipe);  // Phase 5: adapt to the analysed clip
    ResolveRenderData(in_data, source, theme, footage, strength01, skin01, chromaGain, exposure,
                      lookMix, temperature, recipe, data);

    PF_EffectWorld* input = &params[CG_INPUT]->u.ld;
    AEFX_SuiteScoper<PF_Iterate8Suite2> iterate8(in_data, kPFIterate8Suite, kPFIterate8SuiteVersion2, out_data);
    ERR(iterate8->iterate(in_data, 0, output->height, input, NULL, &data, FilterImage8, output));
    return err;
}

/* ============================ Smart Render ============================== */

static void DisposePreRenderData(void* pv) {
    if (pv) delete reinterpret_cast<CG_RenderData*>(pv);
}

static PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_CheckoutResult in_result;
    PF_RenderRequest req = extra->input->output_request;

    extra->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;

    CG_RenderData* d = new (std::nothrow) CG_RenderData();
    if (!d) return PF_Err_OUT_OF_MEMORY;

    PF_ParamDef p;
    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_STRENGTH, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    const double strength01 = p.u.fs_d.value / 100.0;
    ERR2(PF_CHECKIN_PARAM(in_data, &p));

    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_SKIN_PROTECTION, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    const double skin01 = p.u.fs_d.value / 100.0;
    ERR2(PF_CHECKIN_PARAM(in_data, &p));

    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_CHROMA_GAIN, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    const double chromaGain = p.u.fs_d.value / 100.0;
    ERR2(PF_CHECKIN_PARAM(in_data, &p));

    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_THEME, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    const A_long theme = p.u.pd.value;
    ERR2(PF_CHECKIN_PARAM(in_data, &p));

    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_FOOTAGE_PROFILE, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    const A_long footage = p.u.pd.value;
    ERR2(PF_CHECKIN_PARAM(in_data, &p));

    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_LUT_SOURCE, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    const A_long source = p.u.pd.value;
    ERR2(PF_CHECKIN_PARAM(in_data, &p));

    // Phase 6a keyframeable params (live values fold into the bake).
    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_EXPOSURE, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    const double exposure = p.u.fs_d.value;
    ERR2(PF_CHECKIN_PARAM(in_data, &p));

    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_LOOK_MIX, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    const double lookMix = p.u.fs_d.value / 100.0;
    ERR2(PF_CHECKIN_PARAM(in_data, &p));

    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_TEMPERATURE, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    const double temperature = p.u.fs_d.value;
    ERR2(PF_CHECKIN_PARAM(in_data, &p));

    // Copy the grade recipe out of its arb handle before checkin (the handle is
    // only valid while checked out).
    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_RECIPE, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    RecipeData recipe = RecipeFromHandle(in_data, p.u.arb_d.value);
    ERR2(PF_CHECKIN_PARAM(in_data, &p));

    InjectAnalyzedStats(SeqKey(in_data), footage, recipe);  // Phase 5: adapt to the analysed clip
    ResolveRenderData(in_data, source, theme, footage, strength01, skin01, chromaGain, exposure,
                      lookMix, temperature, recipe, *d);

    // Mirror the current params into the editor window (if one is open for this
    // instance), so its controls track Effect Controls. Safe from any render thread:
    // it is a locked value copy. Also reaps a window the user has since closed.
    PublishEditorSnapshot(in_data, footage, theme, strength01, skin01, chromaGain, source, exposure,
                          lookMix, temperature, recipe);

    extra->output->pre_render_data = d;
    extra->output->delete_pre_render_data_func = DisposePreRenderData;

    ERR(extra->cb->checkout_layer(in_data->effect_ref, CG_INPUT, CG_INPUT, &req,
                                  in_data->current_time, in_data->time_step, in_data->time_scale, &in_result));

    UnionLRect(&in_result.result_rect, &extra->output->result_rect);
    UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);
    return err;
}

static PF_Err SmartRenderCPU(PF_InData* in_data, PF_OutData* out_data, PF_PixelFormat fmt,
                             PF_EffectWorld* input, PF_EffectWorld* output, CG_RenderData* d) {
    PF_Err err = PF_Err_NONE;
    CG_DBG("SmartRenderCPU: CPU render path active");
    switch (fmt) {
        case PF_PixelFormat_ARGB128: {
            AEFX_SuiteScoper<PF_iterateFloatSuite2> s(in_data, kPFIterateFloatSuite, kPFIterateFloatSuiteVersion2, out_data);
            ERR(s->iterate(in_data, 0, output->height, input, NULL, d, FilterImage32, output));
            break;
        }
        case PF_PixelFormat_ARGB64: {
            AEFX_SuiteScoper<PF_iterate16Suite2> s(in_data, kPFIterate16Suite, kPFIterate16SuiteVersion2, out_data);
            ERR(s->iterate(in_data, 0, output->height, input, NULL, d, FilterImage16, output));
            break;
        }
        case PF_PixelFormat_ARGB32: {
            AEFX_SuiteScoper<PF_Iterate8Suite2> s(in_data, kPFIterate8Suite, kPFIterate8SuiteVersion2, out_data);
            ERR(s->iterate(in_data, 0, output->height, input, NULL, d, FilterImage8, output));
            break;
        }
        default:
            err = PF_Err_BAD_CALLBACK_PARAM;
            break;
    }
    return err;
}

#if HAS_ANY_GPU
#include "ColorGradeGPU.inc"  // GPUDeviceSetup/Setdown + SmartRenderGPU (DirectX + CUDA)
#endif

static PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra, bool isGPU) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_EffectWorld* input = NULL;
    PF_EffectWorld* output = NULL;
    CG_RenderData* d = reinterpret_cast<CG_RenderData*>(extra->input->pre_render_data);
    if (!d) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, CG_INPUT, &input));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &output));

    AEFX_SuiteScoper<PF_WorldSuite2> world_suite(in_data, kPFWorldSuite, kPFWorldSuiteVersion2, out_data);
    PF_PixelFormat fmt = PF_PixelFormat_INVALID;
    ERR(world_suite->PF_GetPixelFormat(input, &fmt));

    if (!err) {
        if (isGPU) {
#if HAS_ANY_GPU
            ERR(SmartRenderGPU(in_data, out_data, fmt, input, output, extra, d));
#else
            err = SmartRenderCPU(in_data, out_data, fmt, input, output, d);
#endif
        } else {
            ERR(SmartRenderCPU(in_data, out_data, fmt, input, output, d));
        }
    }

    ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, CG_INPUT));
    return err;
}

/* ============================ Entry points ============================== */

extern "C" DllExport PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
    SPBasicSuite* inSPBasicSuitePtr, const char* inHostName, const char* inHostVersion) {
    PF_Err result = PF_Err_INVALID_CALLBACK;
    result = PF_REGISTER_EFFECT_EXT2(
        inPtr, inPluginDataCallBackPtr,
        NAME,                              // Name
        "KyleNaluan CG ColorGrade",        // Match Name (must be globally unique)
        "Color Grade",                     // Category
        AE_RESERVED_INFO,
        "EffectMain",
        "https://github.com/KyleNaluan/color-grade-plugin");
    return result;
}

PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data,
                  PF_ParamDef* params[], PF_LayerDef* output, void* extra) {
    PF_Err err = PF_Err_NONE;
    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:
                err = About(in_data, out_data, params, output);
                break;
            case PF_Cmd_GLOBAL_SETUP:
                err = GlobalSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_GLOBAL_SETDOWN:
                err = GlobalSetdown(in_data, out_data, params, output);
                break;
            case PF_Cmd_SEQUENCE_SETUP:
                err = SequenceSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_SEQUENCE_RESETUP:
                err = SequenceResetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_SEQUENCE_SETDOWN:
                err = SequenceSetdown(in_data, out_data, params, output);
                break;
            case PF_Cmd_PARAMS_SETUP:
                err = ParamsSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_ARBITRARY_CALLBACK:
                err = HandleArbitrary(in_data, out_data, (PF_ArbParamsExtra*)extra);
                break;
            case PF_Cmd_USER_CHANGED_PARAM:
                err = UserChangedParam(in_data, out_data, params, (PF_UserChangedParamExtra*)extra);
                break;
#if HAS_ANY_GPU
            case PF_Cmd_GPU_DEVICE_SETUP:
                err = GPUDeviceSetup(in_data, out_data, (PF_GPUDeviceSetupExtra*)extra);
                break;
            case PF_Cmd_GPU_DEVICE_SETDOWN:
                err = GPUDeviceSetdown(in_data, out_data, (PF_GPUDeviceSetdownExtra*)extra);
                break;
#endif
            case PF_Cmd_RENDER:
                err = LegacyRender(in_data, out_data, params, output);
                break;
            case PF_Cmd_SMART_PRE_RENDER:
                err = PreRender(in_data, out_data, (PF_PreRenderExtra*)extra);
                break;
            case PF_Cmd_SMART_RENDER:
                err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra, false);
                break;
            case PF_Cmd_SMART_RENDER_GPU:
                err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra, true);
                break;
        }
    } catch (PF_Err& thrown_err) {
        err = thrown_err;
    } catch (...) {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}
