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

// Enable the DirectX GPU path only where the HLSL toolchain is present. The build
// defines HAS_HLSL=1 for the GPU configuration; the CPU-only configuration leaves it 0.
#ifndef HAS_HLSL
#define HAS_HLSL 0
#endif

#include "lut/CubeLut.h"
#include "embedded/EmbeddedLut.h"

#define DESCRIPTION "\nApplies a baked 3D LUT (Color Grade). Phase 1 native re-platform."
#define NAME        "CG Color Grade"
#define MAJOR_VERSION  1
#define MINOR_VERSION  0
#define BUG_VERSION    0
#define STAGE_VERSION  PF_Stage_DEVELOP
#define BUILD_VERSION  1

enum {
    CG_INPUT = 0,
    CG_STRENGTH,
    CG_LUT_SOURCE,
    CG_NUM_PARAMS
};

// Popup order (1-based in AE).
enum {
    CG_SRC_EMBEDDED = 1,
    CG_SRC_EXTERNAL
};

#define CG_STRENGTH_MIN    0
#define CG_STRENGTH_MAX    100
#define CG_STRENGTH_DFLT   100

#define CG_LUT_SOURCE_CHOICES "Embedded (Teal-Orange)|External .cube file"

// Data passed from PreRender to (Smart)Render. Owns a parsed LUT for this frame.
struct CG_RenderData {
    cg::Lut3D lut;      // resolved LUT (embedded or external)
    float     strength; // 0..1
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
