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
#include "core/FootageCatalog.h"  // multi-camera footage-profile catalog (flat popup + cascade)
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

// Footage-profile popup order (1-based in AE). The canonical camera+log-profile
// list is data in core/FootageCatalog.h (the mirror of FOOTAGE_PROFILES in
// src/core/color/index.ts): Standard first, then cameras alphabetical. The popup
// CHOICES string and num-choices are built from that catalog at ParamsSetup, and
// ProfileFromFootagePopup / FootageIndexIsLog resolve an index through it, so this
// enum only names the two indices referenced by name elsewhere. Standard = index
// 1 = no decode (fresh-apply default). Reordering the catalog is free pre-release
// (AE keys by index but this param has no project-compat constraint - see the
// decision doc); the editor Camera->Profile cascade derives from the same catalog.
enum {
    CG_FOOT_REC709 = 1,  // Standard (Rec.709) - always index 1
    CG_FOOT_VLOG = 11    // Panasonic V-Log - named for the parity/decode default only
};

// Theme popup order (1-based in AE); maps to cg::core::getTheme keys. None (Manual)
// leads the list (captain decision [key=none-first]): a bare AE popup stores its
// selection as a 1-based index, so this reorder DELIBERATELY breaks the stored
// mapping - projects saved with the earlier Teal-first order have their theme
// selection shifted by the permutation and must be re-picked once (accepted:
// ~2 pre-release test clips). ThemeFromPopup + the editor's kThemeNames[] switch
// on these named constants, so both surfaces track this order automatically.
// The default popup value stays CG_THEME_TEAL (see PF_ADD_POPUPX) so a freshly
// applied effect keeps the teal-orange look the Strength/Skin defaults mirror.
// Adding future looks stays append-at-end; keep labels ASCII (AE strings are
// narrow/CP-1252).
enum {
    CG_THEME_NONE = 1,
    CG_THEME_TEAL,
    CG_THEME_WARM,
    CG_THEME_COOL,
    CG_THEME_REFERENCE,
    // Curated library (PR #36), appended after the original set.
    CG_THEME_GOLDEN_HOUR,
    CG_THEME_BLEACH_BYPASS,
    CG_THEME_VINTAGE_FADE,
    CG_THEME_HIGH_KEY_CLEAN,
    CG_THEME_LOW_KEY_MOODY,
    CG_THEME_WINTER_BLUE,
    CG_THEME_WARM_PORTRAIT,
    CG_THEME_PASTEL_DREAM,
    CG_THEME_NEON_CYBERPUNK,
    CG_THEME_DAY_FOR_NIGHT,
    CG_THEME_AUTUMN,
    CG_THEME_SUMMER_BLOCKBUSTER,
    CG_THEME_MUTED_TEAL_ORANGE,
    CG_THEME_MONOCHROME_BW,
    CG_THEME_SEPIA,
    CG_THEME_CINEMATIC_GREEN,
    CG_THEME_DESATURATED_DOC,
    CG_THEME_PUNCHY_SOCIAL,
    CG_THEME_CROSS_PROCESS,
    CG_THEME_ROSE_ROMANCE
};
#define CG_THEME_CHOICES \
    "None (Manual)|Teal-Orange|Warm-Film|Cool-Noir|Reference Match|" \
    "Golden Hour|Bleach Bypass|Vintage Fade|High-Key Clean|Low-Key Moody|" \
    "Winter Blue|Warm Portrait|Pastel Dream|Neon Cyberpunk|Day for Night|" \
    "Autumn|Summer Blockbuster|Muted Teal-Orange|Monochrome B&W|Sepia|" \
    "Cinematic Green|Desaturated Doc|Punchy Social|Cross Process|Rose Romance"
#define CG_THEME_COUNT 25

// LUT-source popup order (1-based). Auto bakes the grade from theme + recipe.
// "External .cube + Correct/Basics" applies the footage decode AND the manual
// primary correction (Basics + LGG wheels from the recipe) UNDER the user's own
// creative .cube: decode -> correct/basics -> user LUT (fm/cg-lut-correct-stack).
// Appending a popup CHOICE is the established safe pattern (no arb-data change,
// no AE_Effect_Version bump, no new editor-bridge field - the LUT Source stream
// already round-trips through the editor). Plain "External .cube file" keeps its
// prior decode -> user LUT behavior unchanged (correction disabled).
enum {
    CG_SRC_AUTO = 1,
    CG_SRC_EMBEDDED,
    CG_SRC_EXTERNAL,
    CG_SRC_EXTERNAL_CORRECT
};
#define CG_LUT_SOURCE_CHOICES \
    "Auto (Theme + Analysis)|Embedded (Teal-Orange)|External .cube file|External .cube + Correct/Basics"

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
