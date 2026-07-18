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

// Per-instance AEGP effect refs, captured on the main thread (button-open), used by
// the idle hook to reach the effect's param streams. Disposed on close/setdown.
static std::mutex                                         g_effMutex;
static std::map<cg::editor::InstanceKey, AEGP_EffectRefH> g_effectRefs;

// Last param values the idle hook published to each window (touched only from the
// idle hook, which runs serially on the main thread - so no mutex). Lets the EC->window
// poll publish only when something actually changed, avoiding revision churn.
static std::map<cg::editor::InstanceKey, cg::editor::ParamSnapshot> g_lastPolled;

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
                        double chromaGain, const cg::core::RecipeData& recipe, cg::Lut3D& dst) {
    cg::core::Theme theme = ThemeFromPopup(themePopup);
    cg::core::ThemeOverrides ov = theme.overrides.value_or(cg::core::ThemeOverrides{});
    const double authored = ov.chromaGain.value_or(1.0);
    ov.chromaGain = authored * chromaGain;  // slider scales the theme's authored gain
    theme.overrides = ov;

    cg::core::FootageStats src = cg::core::statsFromData(recipe.sourceStats);
    cg::core::EngineOptions opts;
    opts.strength = strength01;
    opts.skinProtection = skin01;

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

// Resample a baked LUT so the Footage/Correct decode applies *before* it:
// newLut(x) = rawLut(decode(x)). This makes the decode stage apply in the
// Embedded/External raw-LUT modes too (captain directive: never leave V-Log footage
// undecoded under any LUT Source), while keeping the per-pixel apply a single
// trilinear sample (CPU/GPU identical). Rec.709 decodes to itself, so it is a no-op.
// (The Auto path composes the decode into the *continuous* grade in BakeAutoLut, which
// avoids this resample's extra interpolation; here we only have a baked LUT to compose.)
static cg::Lut3D ComposeDecodeIntoLut(const cg::Lut3D& lut, A_long footagePopup) {
    if (footagePopup != CG_FOOT_VLOG) return lut;
    const cg::core::LogProfile& profile = ProfileFromFootagePopup(footagePopup);
    return cg::core::bakeLut(
        [&lut, &profile](const cg::core::Vec3d& x) -> cg::core::Vec3d {
            const cg::core::Vec3d dec = cg::core::decodePixelToRec709(x, profile);
            const cg::Vec3 s = cg::sampleLut(
                lut, cg::Vec3{static_cast<float>(dec[0]), static_cast<float>(dec[1]),
                              static_cast<float>(dec[2])});
            return cg::core::Vec3d{s[0], s[1], s[2]};
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
                // Only trust a blob whose size matches; otherwise (foreign/older/grown
                // struct) it is unusable, so reseed to the default recipe. Never leave
                // *arbPH unset - AE must always receive a valid handle.
                bool usable = extra->u.unflatten_func_params.buf_sizeLu == sizeof(RecipeData);
                if (usable) {
                    std::memcpy(dstP, extra->u.unflatten_func_params.flat_dataPV, sizeof(RecipeData));
                    usable = dstP->magic == cg::core::RECIPE_MAGIC && dstP->version == cg::core::RECIPE_VERSION;
                }
                if (!usable) {
                    cg::core::Theme th = cg::core::tealOrangeTheme();
                    RecipeData def = cg::core::recipeFromTheme(th, th.targetStats);
                    std::memcpy(dstP, &def, sizeof(RecipeData));
                }
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

// Resolve the LUT + post-blend for a frame from the resolved param values. Auto
// bakes natively (strength baked in); embedded/external keep the Phase 1 blend.
static void ResolveRenderData(PF_InData* in_data, A_long source, A_long themePopup, A_long footagePopup,
                              double strength01, double skin01, double chromaGain,
                              const RecipeData& recipe, CG_RenderData& d) {
    if (source == CG_SRC_AUTO) {
        BakeAutoLut(themePopup, footagePopup, strength01, skin01, chromaGain, recipe, d.lut);
        d.applyStrength = 1.0f;
    } else {
        // Embedded/External raw LUT, with the Footage/Correct decode composed in first
        // so V-Log footage is still decoded to Rec.709 before the LUT (captain directive).
        cg::Lut3D raw;
        ResolveLut(source, raw);
        d.lut = ComposeDecodeIntoLut(raw, footagePopup);
        d.applyStrength = static_cast<float>(strength01);
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

static cg::editor::ParamSnapshot MakeSnapshot(A_long footage, A_long theme, double strength01,
                                              double skin01, double chromaFrac, A_long source) {
    cg::editor::ParamSnapshot s;
    s.footageProfile = static_cast<int>(footage);
    s.theme = static_cast<int>(theme);
    s.strength = strength01;
    s.skinProtection = skin01;
    s.chromaGain = chromaFrac;
    s.lutSource = static_cast<int>(source);
    s.revision = g_snapshotRevision.fetch_add(1);
    return s;
}

// Push the effect's current params to the editor window (if open).
static void PublishEditorSnapshot(PF_InData* in_data, A_long footage, A_long theme, double strength01,
                                  double skin01, double chromaFrac, A_long source) {
    cg::editor::EditorHost::instance().publishSnapshot(
        SeqKey(in_data), MakeSnapshot(footage, theme, strength01, skin01, chromaFrac, source));
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
    }
    return false;
}

// Capture this effect instance's AEGP effect ref (main-thread contexts only), so the
// idle hook can reach its param streams. Replaces any stale ref for the same key.
static void CaptureEffectRefForKey(PF_InData* in_data, cg::editor::InstanceKey key) {
    if (!g_spbasic || !g_aegpId || !in_data->effect_ref) return;
    try {
        AEGP_SuiteHandler sh(g_spbasic);
        AEGP_EffectRefH effectH = nullptr;
        A_Err e = sh.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(g_aegpId, in_data->effect_ref, &effectH);
        if (e || !effectH) return;
        AEGP_EffectRefH stale = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_effMutex);
            auto it = g_effectRefs.find(key);
            if (it != g_effectRefs.end()) stale = it->second;
            g_effectRefs[key] = effectH;
        }
        if (stale) sh.EffectSuite4()->AEGP_DisposeEffect(stale);
    } catch (...) { /* AEGP unavailable - the on-param-change stand-in still applies edits */ }
}

static void DisposeEffectRefForKey(cg::editor::InstanceKey key) {
    AEGP_EffectRefH effectH = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_effMutex);
        auto it = g_effectRefs.find(key);
        if (it == g_effectRefs.end()) return;
        effectH = it->second;
        g_effectRefs.erase(it);
    }
    if (effectH && g_spbasic) {
        try { AEGP_SuiteHandler sh(g_spbasic); sh.EffectSuite4()->AEGP_DisposeEffect(effectH); } catch (...) {}
    }
}

// Read one param stream's scalar (one_d) value at t=0 (our value params don't
// time-vary). Returns false on any AEGP failure so the poll degrades gracefully.
static bool ReadStreamOneD(AEGP_SuiteHandler& sh, AEGP_EffectRefH effectH, PF_ParamIndex idx, double& out) {
    AEGP_StreamRefH streamH = nullptr;
    if (sh.StreamSuite5()->AEGP_GetNewEffectStreamByIndex(g_aegpId, effectH, idx, &streamH) || !streamH) {
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

// Read the effect's current value params into a snapshot (the EC->window direction).
static bool PollSnapshotViaAegp(AEGP_SuiteHandler& sh, AEGP_EffectRefH effectH, cg::editor::ParamSnapshot& s) {
    double footage, theme, strength, skin, chroma, lut;
    if (!ReadStreamOneD(sh, effectH, CG_FOOTAGE_PROFILE, footage)) return false;
    if (!ReadStreamOneD(sh, effectH, CG_THEME, theme)) return false;
    if (!ReadStreamOneD(sh, effectH, CG_STRENGTH, strength)) return false;
    if (!ReadStreamOneD(sh, effectH, CG_SKIN_PROTECTION, skin)) return false;
    if (!ReadStreamOneD(sh, effectH, CG_CHROMA_GAIN, chroma)) return false;
    if (!ReadStreamOneD(sh, effectH, CG_LUT_SOURCE, lut)) return false;
    s.footageProfile = static_cast<int>(footage + 0.5);
    s.theme = static_cast<int>(theme + 0.5);
    s.strength = strength / 100.0;
    s.skinProtection = skin / 100.0;
    s.chromaGain = chroma / 100.0;
    s.lutSource = static_cast<int>(lut + 0.5);
    return true;
}

static bool SnapshotChanged(const cg::editor::ParamSnapshot& a, const cg::editor::ParamSnapshot& b) {
    return a.footageProfile != b.footageProfile || a.theme != b.theme || a.lutSource != b.lutSource ||
           std::fabs(a.strength - b.strength) > 1e-6 ||
           std::fabs(a.skinProtection - b.skinProtection) > 1e-6 ||
           std::fabs(a.chromaGain - b.chromaGain) > 1e-6;
}

// The AEGP idle hook: main thread, fires while AE is idle. Cheap-noops unless a window
// has pending edits, so it never burdens AE. Applies drained edits inside one undo
// group per tick, polls Effect Controls -> window on change, and reaps effect refs
// whose window has been closed.
static A_Err CG_IdleHook(AEGP_GlobalRefcon, AEGP_IdleRefcon, A_long* max_sleepPL) {
    auto& host = cg::editor::EditorHost::instance();
    std::vector<cg::editor::InstanceKey> keys = host.openKeys();

    // Reap effect refs whose window is gone (user closed it) - dispose to avoid leaks.
    {
        std::vector<cg::editor::InstanceKey> orphans;
        {
            std::lock_guard<std::mutex> lk(g_effMutex);
            for (auto& kv : g_effectRefs) {
                if (std::find(keys.begin(), keys.end(), kv.first) == keys.end()) orphans.push_back(kv.first);
            }
        }
        for (auto k : orphans) {
            DisposeEffectRefForKey(k);
            g_lastPolled.erase(k);
        }
    }

    if (g_spbasic && g_aegpId) {
        for (auto key : keys) {
            AEGP_EffectRefH effectH = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_effMutex);
                auto it = g_effectRefs.find(key);
                if (it != g_effectRefs.end()) effectH = it->second;
            }
            if (!effectH) continue;

            // window -> effect: apply any queued edits (writes param streams).
            if (host.hasPendingEdits(key)) {
                std::vector<cg::editor::ParamEdit> edits = host.drainEdits(key);
                if (!edits.empty()) {
                    try {
                        AEGP_SuiteHandler sh(g_spbasic);
                        sh.UtilitySuite6()->AEGP_StartUndoGroup("Color Grade editor edit");
                        for (const auto& e : edits) {
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
                if (PollSnapshotViaAegp(sh, effectH, polled)) {
                    auto it = g_lastPolled.find(key);
                    if (it == g_lastPolled.end() || SnapshotChanged(it->second, polled)) {
                        polled.revision = g_snapshotRevision.fetch_add(1);
                        g_lastPolled[key] = polled;
                        host.publishSnapshot(key, polled);
                    }
                }
            } catch (...) { /* swallow */ }
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
// dispose its captured AEGP effect ref, and free the sequence data.
static PF_Err SequenceSetdown(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    const cg::editor::InstanceKey key = SeqKey(in_data);
    cg::editor::EditorHost::instance().close(key);
    DisposeEffectRefForKey(key);
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
                  3 /* num choices */,
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
    PF_ADD_BUTTON("Editor",
                  "Open Editor\xE2\x80\xA6",  // "Open Editor…" (UTF-8 ellipsis)
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
        // controls start in sync with Effect Controls.
        cg::editor::ParamSnapshot seed = MakeSnapshot(
            params[CG_FOOTAGE_PROFILE]->u.pd.value,
            params[CG_THEME]->u.pd.value,
            params[CG_STRENGTH]->u.fs_d.value / 100.0,
            params[CG_SKIN_PROTECTION]->u.fs_d.value / 100.0,
            params[CG_CHROMA_GAIN]->u.fs_d.value / 100.0,
            params[CG_LUT_SOURCE]->u.pd.value);
        const cg::editor::InstanceKey key = SeqKey(in_data);
        cg::editor::EditorHost::instance().open(key, seed);
        // Capture this instance's AEGP effect ref (main-thread context) so the idle
        // hook can write window edits back onto its param streams.
        CaptureEffectRefForKey(in_data, key);
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
    const RecipeData recipe = RecipeFromHandle(in_data, params[CG_RECIPE]->u.arb_d.value);
    ResolveRenderData(in_data, source, theme, footage, strength01, skin01, chromaGain, recipe, data);

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

    // Copy the grade recipe out of its arb handle before checkin (the handle is
    // only valid while checked out).
    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_RECIPE, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    const RecipeData recipe = RecipeFromHandle(in_data, p.u.arb_d.value);
    ERR2(PF_CHECKIN_PARAM(in_data, &p));

    ResolveRenderData(in_data, source, theme, footage, strength01, skin01, chromaGain, recipe, *d);

    // Mirror the current params into the editor window (if one is open for this
    // instance), so its controls track Effect Controls. Safe from any render thread:
    // it is a locked value copy. Also reaps a window the user has since closed.
    PublishEditorSnapshot(in_data, footage, theme, strength01, skin01, chromaGain, source);

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
