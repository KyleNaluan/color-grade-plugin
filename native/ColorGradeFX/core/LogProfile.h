/*
 * LogProfile.h - C++ port of the LogProfile type (src/core/color/types.ts) plus
 * the concrete profiles and PROFILES registry (src/core/color/rec709.ts,
 * vlog.ts, index.ts).
 *
 * A LogProfile pairs a per-channel decode/encode with a scene-linear
 * camera-gamut -> Rec.709 matrix. `name` is significant: decodePixelToRec709
 * short-circuits when name == "Rec.709" (identity), exactly as the TS does.
 */
#pragma once
#ifndef CG_CORE_LOGPROFILE_H
#define CG_CORE_LOGPROFILE_H

#include <string>

#include "Mat3.h"
#include "Rec709.h"
#include "Vlog.h"
#include "Slog3.h"
#include "CanonLog.h"
#include "ArriLogC.h"
#include "DjiDlog.h"
#include "BmdFilmGen5.h"
#include "FLog.h"
#include "NLog.h"

namespace cg {
namespace core {

struct LogProfile {
    std::string name;
    double (*decode)(double);
    double (*encode)(double);
    Mat3 gamutToRec709;
};

inline Mat3 identity3() {
    return Mat3{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
}

inline const LogProfile& REC709_PROFILE() {
    static const LogProfile p{"Rec.709", &rec709Decode, &rec709Encode, identity3()};
    return p;
}

inline const LogProfile& VLOG_PROFILE() {
    static const LogProfile p{"V-Log", &vlogDecode, &vlogEncode, VLOG_GAMUT_TO_REC709()};
    return p;
}

inline const LogProfile& SLOG3_PROFILE() {
    static const LogProfile p{"S-Log3", &slog3Decode, &slog3Encode, SLOG3_GAMUT_TO_REC709()};
    return p;
}

inline const LogProfile& CLOG2_PROFILE() {
    static const LogProfile p{"C-Log2", &canonLog2Decode, &canonLog2Encode, CINEMA_GAMUT_TO_REC709()};
    return p;
}

inline const LogProfile& CLOG3_PROFILE() {
    static const LogProfile p{"C-Log3", &canonLog3Decode, &canonLog3Encode, CINEMA_GAMUT_TO_REC709()};
    return p;
}

inline const LogProfile& LOGC3_PROFILE() {
    static const LogProfile p{"LogC3", &logC3Decode, &logC3Encode, AWG3_GAMUT_TO_REC709()};
    return p;
}

inline const LogProfile& LOGC4_PROFILE() {
    static const LogProfile p{"LogC4", &logC4Decode, &logC4Encode, AWG4_GAMUT_TO_REC709()};
    return p;
}

inline const LogProfile& DLOG_PROFILE() {
    static const LogProfile p{"D-Log", &dLogDecode, &dLogEncode, DLOG_GAMUT_TO_REC709()};
    return p;
}

inline const LogProfile& FILM_GEN5_PROFILE() {
    static const LogProfile p{"Blackmagic Film Gen5", &filmGen5Decode, &filmGen5Encode, FILM_GEN5_GAMUT_TO_REC709()};
    return p;
}

inline const LogProfile& FLOG_PROFILE() {
    static const LogProfile p{"F-Log", &flogDecode, &flogEncode, FLOG_GAMUT_TO_REC709()};
    return p;
}

inline const LogProfile& FLOG2_PROFILE() {
    static const LogProfile p{"F-Log2", &flog2Decode, &flog2Encode, FLOG_GAMUT_TO_REC709()};
    return p;
}

inline const LogProfile& NLOG_PROFILE() {
    static const LogProfile p{"N-Log", &nLogDecode, &nLogEncode, NLOG_GAMUT_TO_REC709()};
    return p;
}

// PROFILES registry: mirrors src/core/color/index.ts. Keyed by the same strings.
inline const std::string STANDARD_PROFILE_KEY = "rec709";

inline const LogProfile* getProfile(const std::string& key) {
    if (key == "rec709") return &REC709_PROFILE();
    if (key == "vlog") return &VLOG_PROFILE();
    if (key == "sony-slog3") return &SLOG3_PROFILE();
    if (key == "canon-clog2") return &CLOG2_PROFILE();
    if (key == "canon-clog3") return &CLOG3_PROFILE();
    if (key == "arri-logc3") return &LOGC3_PROFILE();
    if (key == "arri-logc4") return &LOGC4_PROFILE();
    if (key == "dji-dlog") return &DLOG_PROFILE();
    if (key == "bmd-filmgen5") return &FILM_GEN5_PROFILE();
    if (key == "fuji-flog") return &FLOG_PROFILE();
    if (key == "fuji-flog2") return &FLOG2_PROFILE();
    if (key == "nikon-nlog") return &NLOG_PROFILE();
    return nullptr;
}

inline bool isLogProfile(const std::string& key) {
    return key != STANDARD_PROFILE_KEY;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_LOGPROFILE_H
