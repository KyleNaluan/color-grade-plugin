#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif

resource 'PiPL' (16000) {
	{	/* array properties: 12 elements */
		/* [1] */
		Kind {
			AEEffect
		},
		/* [2] */
		Name {
			"CG Color Grade"
		},
		/* [3] */
		Category {
			"Color Grade"
		},
#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
		CodeWin64X86 {"EffectMain"},
    #elif defined(AE_PROC_ARM64)
		CodeWinARM64 {"EffectMain"},
    #endif
#elif defined(AE_OS_MAC)
		CodeMacIntel64 {"EffectMain"},
		CodeMacARM64 {"EffectMain"},
#endif
		/* [6] */
		AE_PiPL_Version {
			2,
			0
		},
		/* [7] */
		AE_Effect_Spec_Version {
			PF_PLUG_IN_VERSION,
			PF_PLUG_IN_SUBVERS
		},
		/* [8] */
		/* MUST equal PF_VERSION(MAJOR,MINOR,BUG,STAGE,BUILD) from ColorGrade.h, or AE
		   warns "version mismatch (88001)". Packed = (vers<<19)+(subvers<<15)+(bug<<11)+(stage<<9)+build.
		   1.1.0 develop(0) build 1 = (1<<19)+(1<<15)+1 = 557057. Bump in lockstep with the code version. */
		AE_Effect_Version {
			557057	/* 1.1.0 develop build 1 */
		},
		/* [9] */
		AE_Effect_Info_Flags {
			0
		},
		/* [10] */
		/* out_flags  = PIX_INDEPENDENT | DEEP_COLOR_AWARE           = 0x2000400 */
		AE_Effect_Global_OutFlags {
			0x2000400
		},
		/* out_flags2 = FLOAT_COLOR_AWARE | SUPPORTS_SMART_RENDER | SUPPORTS_THREADED_RENDERING
		                                                             = 0x8001400 (CPU build)
		   GPU build (HAS_HLSL) adds GPU_RENDER_F32 + DIRECTX_RENDERING = 0x2a001400.
		   These flags MUST match GlobalSetup's out_flags2 or AE silently falls back to CPU. */
#ifdef HAS_HLSL
		AE_Effect_Global_OutFlags_2 {
			0x2a001400
		},
#else
		AE_Effect_Global_OutFlags_2 {
			0x8001400
		},
#endif
		/* [11] */
		AE_Effect_Match_Name {
			"KyleNaluan CG ColorGrade"
		},
		/* [12] */
		AE_Reserved_Info {
			0
		},
		/* [13] */
		AE_Effect_Support_URL {
			"https://github.com/KyleNaluan/color-grade-plugin"
		}
	}
};
