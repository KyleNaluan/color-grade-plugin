/*
 * ColorGrade.h - Color Grade native AE effect (Phase 1).
 *
 * A SmartFX effect that applies a baked 3D LUT to a layer via the ported trilinear
 * sampleLut (lut/CubeLut.h). CPU path is the mandatory fallback; a DirectX GPU path
 * accelerates it (guarded by HAS_HLSL). See native/BUILDING.md for the toolchain.
 *
 * Phase 1 scope: registration + drag + SmartFX render + reused LUT-apply IP. The
 * theme popup / knob sliders / arb-data stats and the in-effect bake land in Phase 2.
 */
#pragma once
#ifndef COLORGRADE_H
#define COLORGRADE_H

#include "AEConfig.h"
#include "entry.h"
#include "AEFX_SuiteHelper.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectGPUSuites.h"
#include "AE_Macros.h"
#include "AEGP_SuiteHandler.h"
#include "String_Utils.h"
#include "Param_Utils.h"
#include "Smart_Utils.h"

#include <math.h>

#ifdef AE_OS_WIN
#include <Windows.h>
#endif

// GPU paths are compiled in per-framework by the build: CG_GPU=true defines HAS_HLSL=1
// (DirectX) and HAS_CUDA=1 (NVIDIA). The CPU-only configuration leaves both 0. DirectX is
// the vendor-neutral path; CUDA is what AE's Mercury actually offers on the NVIDIA dev box
// (AE 2025 there exposes only CUDA/OpenCL/Software - no DirectX), so both are shipped.
#ifndef HAS_HLSL
#define HAS_HLSL 0
#endif
#ifndef HAS_CUDA
#define HAS_CUDA 0
#endif
#define HAS_ANY_GPU (HAS_HLSL || HAS_CUDA)

#include "lut/CubeLut.h"
#include "embedded/EmbeddedLut.h"
#include "core/Recipe.h"   // POD grade recipe (arb-data) + in-effect bake
#include "core/Themes.h"   // ported shipping themes (getTheme by name)
#include "editor/EditorWindow.h"  // Phase 3 editor-window host (ImGui spike) + bridge

#define DESCRIPTION "\nApplies a Color Grade (theme + analysis, baked natively). Phase 2 native re-platform."
#define NAME        "CG Color Grade"
#define MAJOR_VERSION  1
#define MINOR_VERSION  1
#define BUG_VERSION    0
#define STAGE_VERSION  PF_Stage_DEVELOP
#define BUILD_VERSION  1

// Param order. Phase 2 adds the theme popup, the skin-protection + chroma-gain
// knob sliders, and the grade-recipe arb-data param (measured stats + the full
// typed knob space). Pre-release, so reordering vs Phase 1 is fine.
enum {
    CG_INPUT = 0,
    CG_FOOTAGE_PROFILE,  // Correct: the clip's log format (drives the decode stage)
    CG_THEME,
    CG_STRENGTH,
    CG_SKIN_PROTECTION,
    CG_CHROMA_GAIN,
    CG_LUT_SOURCE,
    CG_OPEN_EDITOR,   // momentary button: opens the native editor window (Phase 3)
    CG_RECIPE,
    // Phase 6a keyframeable params. MUST stay APPENDED AFTER CG_RECIPE (never
    // inserted mid-roster): AE stores project param values by index and the editor
    // bridge addresses streams by index, so inserting mid-roster remaps saved values.
    CG_EXPOSURE,      // stops, keyframeable; live value overrides recipe at bake
    CG_LOOK_MIX,      // 0..100%, keyframeable theme-look blend
    CG_TEMPERATURE,   // -100..100, keyframeable white-balance
    CG_NUM_PARAMS
};

// Footage-profile popup order (1-based in AE); maps to cg::core::getProfile keys.
// Rec.709 = standard (no decode); V-Log decodes to Rec.709 before the grade.
enum {
    CG_FOOT_REC709 = 1,
    CG_FOOT_VLOG
};
#define CG_FOOTAGE_CHOICES "Rec.709 (standard)|V-Log"

// Theme popup order (1-based in AE); maps to cg::core::getTheme keys. None (Manual)
// is appended (Phase 6a), Reference Match (Phase 7) after that: adding a popup CHOICE
// keeps CG_THEME's param index, so existing projects with value 1-4 still resolve.
enum {
    CG_THEME_TEAL = 1,
    CG_THEME_WARM,
    CG_THEME_COOL,
    CG_THEME_NONE,
    CG_THEME_REFERENCE
};
#define CG_THEME_CHOICES "Teal-Orange|Warm-Film|Cool-Noir|None (Manual)|Reference Match"
#define CG_THEME_COUNT 5

// LUT-source popup order (1-based). Auto bakes the grade from theme + recipe.
enum {
    CG_SRC_AUTO = 1,
    CG_SRC_EMBEDDED,
    CG_SRC_EXTERNAL
};
#define CG_LUT_SOURCE_CHOICES "Auto (Theme + Analysis)|Embedded (Teal-Orange)|External .cube file"

#define CG_STRENGTH_MIN    0
#define CG_STRENGTH_MAX    100
#define CG_STRENGTH_DFLT   80    // mirrors the default theme's authored strength (teal-orange 0.8)

#define CG_SKIN_MIN        0
#define CG_SKIN_MAX        100
#define CG_SKIN_DFLT       78    // mirrors the default theme's authored skinProtection (teal-orange 0.78)

// Chroma gain as a percentage: 100% = 1.0 (theme default), <100 mutes, >100 boosts.
#define CG_CHROMA_MIN      0
#define CG_CHROMA_MAX      300
#define CG_CHROMA_SLIDER_MAX 200
#define CG_CHROMA_DFLT     100

// Phase 6a keyframeable params. Exposure in stops; Look Mix a percent (100 = full
// look); Temperature -100..100. Neutral defaults keep a fresh effect byte-identical.
#define CG_EXPOSURE_MIN    (-5.0)
#define CG_EXPOSURE_MAX    5.0
#define CG_EXPOSURE_DFLT   0.0

#define CG_LOOK_MIX_MIN    0
#define CG_LOOK_MIX_MAX    100
#define CG_LOOK_MIX_DFLT   100

#define CG_TEMPERATURE_MIN (-100.0)
#define CG_TEMPERATURE_MAX 100.0
#define CG_TEMPERATURE_DFLT 0.0

// arb-data identity: unique id + refcon guard (see the arb callbacks).
#define CG_ARB_ID          1
#define CG_ARB_REFCON      (reinterpret_cast<void*>(static_cast<uintptr_t>(0xC0107ADEULL)))

// Which grid the in-effect Auto bake uses (matches the TS grade default).
#define CG_GRADE_LUT_SIZE  33

// Data passed from PreRender to (Smart)Render. Owns a parsed/baked LUT for this frame.
struct CG_RenderData {
    cg::Lut3D lut;            // resolved LUT (auto-baked, embedded, or external)
    float     applyStrength;  // post-LUT blend: 1.0 for Auto (strength is baked in), else the slider
};

extern "C" {
DllExport PF_Err EffectMain(
    PF_Cmd      cmd,
    PF_InData*  in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    void*       extra);
}

#endif  // COLORGRADE_H
