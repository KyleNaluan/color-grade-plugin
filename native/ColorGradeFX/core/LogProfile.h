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

// PROFILES registry: mirrors src/core/color/index.ts. Keyed by the same strings.
inline const std::string STANDARD_PROFILE_KEY = "rec709";

inline const LogProfile* getProfile(const std::string& key) {
    if (key == "vlog") return &VLOG_PROFILE();
    if (key == "rec709") return &REC709_PROFILE();
    return nullptr;
}

inline bool isLogProfile(const std::string& key) {
    return key != STANDARD_PROFILE_KEY;
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_LOGPROFILE_H
