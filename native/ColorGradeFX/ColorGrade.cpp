/*
 * ColorGrade.cpp - Phase 1 native Color Grade effect: a SmartFX LUT-apply effect.
 *
 * Registers in AE 2025, drags onto a layer, and applies a baked 3D LUT to pixels
 * through the ported trilinear sampleLut (lut/CubeLut.h). CPU Smart Render is the
 * mandatory path; the DirectX GPU path (HAS_HLSL) accelerates it - see ColorGradeGPU.inc.
 *
 * LUT source (param): "Embedded (Teal-Orange)" uses the compiled-in default; "External
 * .cube file" loads from the env var CG_LUT_PATH, else <pluginDir>/ColorGrade_LUT.cube,
 * falling back to the embedded LUT on any failure (so render never fails for a bad path).
 */

#include "ColorGrade.h"

#include <fstream>
#include <sstream>
#include <string>
#include <new>

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

#if HAS_HLSL
    // The GPU path is advertised only outside Premiere. DirectX is the primary path;
    // this flag must match the PiPL's Global_OutFlags_2 or AE silently falls back to CPU.
    if (in_data->appl_id != 'PrMr') {
        out_data->out_flags2 |= PF_OutFlag2_SUPPORTS_GPU_RENDER_F32 | PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING;
    }
#endif
    return PF_Err_NONE;
}

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Strength",
                         CG_STRENGTH_MIN, CG_STRENGTH_MAX,
                         CG_STRENGTH_MIN, CG_STRENGTH_MAX,
                         CG_STRENGTH_DFLT,
                         1, PF_ValueDisplayFlag_PERCENT, 0,
                         CG_STRENGTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("LUT Source",
                 2 /* num choices */,
                 CG_SRC_EMBEDDED,
                 CG_LUT_SOURCE_CHOICES,
                 CG_LUT_SOURCE);

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
    ApplyLut(d->lut, d->strength, inP->red, inP->green, inP->blue, outP->red, outP->green, outP->blue);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err FilterImage16(void* refcon, A_long, A_long, PF_Pixel16* inP, PF_Pixel16* outP) {
    CG_RenderData* d = reinterpret_cast<CG_RenderData*>(refcon);
    const float k = 32768.0f;  // AE 16-bit max channel value
    float r, g, b;
    ApplyLut(d->lut, d->strength, inP->red / k, inP->green / k, inP->blue / k, r, g, b);
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
    ApplyLut(d->lut, d->strength, inP->red / k, inP->green / k, inP->blue / k, r, g, b);
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
    data.strength = static_cast<float>(params[CG_STRENGTH]->u.fs_d.value / 100.0);
    ResolveLut(params[CG_LUT_SOURCE]->u.pd.value, data.lut);

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
    PF_Err err = PF_Err_NONE;
    PF_CheckoutResult in_result;
    PF_RenderRequest req = extra->input->output_request;

    extra->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;

    CG_RenderData* d = new (std::nothrow) CG_RenderData();
    if (!d) return PF_Err_OUT_OF_MEMORY;

    PF_ParamDef p;
    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_STRENGTH, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    d->strength = static_cast<float>(p.u.fs_d.value / 100.0);

    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, CG_LUT_SOURCE, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
    A_long source = p.u.pd.value;

    ResolveLut(source, d->lut);

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

#if HAS_HLSL
#include "ColorGradeGPU.inc"  // GPUDeviceSetup/Setdown + SmartRenderGPU (DirectX)
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
#if HAS_HLSL
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
#if HAS_HLSL
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
