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

#include <fstream>
#include <sstream>
#include <string>
#include <cstring>
#include <new>

// Debug-only path tracer: prints to the debugger / DebugView so the active render
// path (CPU vs DirectX GPU) can be confirmed WITHOUT attaching a debugger/breakpoint.
// See native/BUILDING.md "Verifying the GPU path". No-op in Release.
#if defined(_DEBUG) && defined(AE_OS_WIN)
#define CG_DBG(msg) ::OutputDebugStringA("[ColorGradeFX] " msg "\n")
#else
#define CG_DBG(msg) ((void)0)
#endif

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

// Bake the Auto-mode grade LUT natively: the popup theme supplies the look, the
// arb-data recipe supplies the measured source stats, and the sliders drive the
// engine knobs (strength / skinProtection as EngineOptions, chroma-gain as a
// relative multiplier on the theme's authored chromaGain). This is the in-effect
// counterpart to the TS bakeGradeLut; the cross-engine golden harness proves the
// two agree. The chromaGain arg is the slider fraction (slider/100), so 100% (=1.0)
// preserves each theme's authored chromaGain exactly and the slider scales from there.
static void BakeAutoLut(A_long themePopup, double strength01, double skin01, double chromaGain,
                        const cg::core::RecipeData& recipe, cg::Lut3D& dst) {
    cg::core::Theme theme = ThemeFromPopup(themePopup);
    cg::core::ThemeOverrides ov = theme.overrides.value_or(cg::core::ThemeOverrides{});
    const double authored = ov.chromaGain.value_or(1.0);
    ov.chromaGain = authored * chromaGain;  // slider scales the theme's authored gain
    theme.overrides = ov;

    cg::core::FootageStats src = cg::core::statsFromData(recipe.sourceStats);
    cg::core::EngineOptions opts;
    opts.strength = strength01;
    opts.skinProtection = skin01;
    dst = cg::core::bakeGradeLut(src, theme, opts, CG_GRADE_LUT_SIZE);
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
static void ResolveRenderData(PF_InData* in_data, A_long source, A_long themePopup,
                              double strength01, double skin01, double chromaGain,
                              const RecipeData& recipe, CG_RenderData& d) {
    if (source == CG_SRC_AUTO) {
        BakeAutoLut(themePopup, strength01, skin01, chromaGain, recipe, d.lut);
        d.applyStrength = 1.0f;
    } else {
        ResolveLut(source, d.lut);
        d.applyStrength = static_cast<float>(strength01);
    }
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
    return PF_Err_NONE;
}

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    // Order must match the CG_* param enum (after CG_INPUT).
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Theme",
                 3 /* num choices */,
                 CG_THEME_TEAL,
                 CG_THEME_CHOICES,
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
    const A_long source = params[CG_LUT_SOURCE]->u.pd.value;
    const RecipeData recipe = RecipeFromHandle(in_data, params[CG_RECIPE]->u.arb_d.value);
    ResolveRenderData(in_data, source, theme, strength01, skin01, chromaGain, recipe, data);

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
    ERR(PF_CHECKOUT_PARAM(in_data, CG_LUT_SOURCE, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    const A_long source = p.u.pd.value;
    ERR2(PF_CHECKIN_PARAM(in_data, &p));

    // Copy the grade recipe out of its arb handle before checkin (the handle is
    // only valid while checked out).
    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_RECIPE, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    const RecipeData recipe = RecipeFromHandle(in_data, p.u.arb_d.value);
    ERR2(PF_CHECKIN_PARAM(in_data, &p));

    ResolveRenderData(in_data, source, theme, strength01, skin01, chromaGain, recipe, *d);

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
            case PF_Cmd_PARAMS_SETUP:
                err = ParamsSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_ARBITRARY_CALLBACK:
                err = HandleArbitrary(in_data, out_data, (PF_ArbParamsExtra*)extra);
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
