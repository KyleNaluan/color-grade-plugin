/*
 * FootageCatalog.h - C++ mirror of FOOTAGE_PROFILES in src/core/color/index.ts.
 *
 * The single ordered, UI-facing view of the log-profile registry. Both the AE
 * Effect-Controls flat popup ("Sony - S-Log3") and the editor window's
 * Camera->Profile cascade derive from this one table, so a single ordering keeps
 * the two surfaces in sync. `key` matches the getProfile() / TS PROFILES key.
 *
 * Ordering (captain-approved, data/cg-camera-profiles-decision.md): Standard
 * first, then cameras alphabetical. Reordering is free pre-release - AE keys this
 * popup by index, but there is no append-only/project-compat constraint on it
 * (same rationale as the theme popup's none-first reorder). To add a profile:
 * add a getProfile() entry + a row here (Standard stays index 1). Pure and
 * dependency-free so the editor test compiles it headless.
 */
#pragma once
#ifndef CG_CORE_FOOTAGECATALOG_H
#define CG_CORE_FOOTAGECATALOG_H

#include <algorithm>
#include <string>
#include <vector>

namespace cg {
namespace core {

struct FootageProfileEntry {
    int index;                  // 1-based AE popup position
    const char* key;            // getProfile() / TS PROFILES key
    const char* camera;         // maker name for the editor cascade ("" for Standard)
    const char* profileLabel;   // editor cascade profile label ("S-Log3")
    const char* flatLabel;      // AE flat-popup label ("Sony - S-Log3")
};

inline const std::vector<FootageProfileEntry>& footageCatalog() {
    static const std::vector<FootageProfileEntry> c = {
        {1, "rec709", "", "Standard (Rec.709)", "Standard (Rec.709)"},
        {2, "arri-logc3", "ARRI", "LogC3 (EI800)", "ARRI - LogC3 (EI800)"},
        {3, "arri-logc4", "ARRI", "LogC4", "ARRI - LogC4"},
        {4, "bmd-filmgen5", "Blackmagic", "Film Gen5", "Blackmagic - Film Gen5"},
        {5, "canon-clog2", "Canon", "C-Log2", "Canon - C-Log2"},
        {6, "canon-clog3", "Canon", "C-Log3", "Canon - C-Log3"},
        {7, "dji-dlog", "DJI", "D-Log", "DJI - D-Log"},
        {8, "fuji-flog", "Fujifilm", "F-Log", "Fujifilm - F-Log"},
        {9, "fuji-flog2", "Fujifilm", "F-Log2", "Fujifilm - F-Log2"},
        {10, "nikon-nlog", "Nikon", "N-Log", "Nikon - N-Log"},
        {11, "vlog", "Panasonic", "V-Log", "Panasonic - V-Log"},
        {12, "sony-slog3", "Sony", "S-Log3", "Sony - S-Log3"},
    };
    return c;
}

inline int footageProfileCount() { return static_cast<int>(footageCatalog().size()); }

// The Standard/Rec.709 flat index (fresh-apply default = no decode).
inline int footageStandardIndex() { return 1; }

// Resolve a 1-based flat index to its catalog row (clamped to a valid range).
inline const FootageProfileEntry& footageEntry(int flatIndex) {
    const auto& c = footageCatalog();
    int i = flatIndex - 1;
    if (i < 0) i = 0;
    if (i >= static_cast<int>(c.size())) i = 0;
    return c[static_cast<size_t>(i)];
}

// getProfile() key for a 1-based flat index.
inline std::string footageKeyForIndex(int flatIndex) { return footageEntry(flatIndex).key; }

// Whether a 1-based flat index denotes log footage (anything but Standard).
inline bool footageIndexIsLog(int flatIndex) { return flatIndex != footageStandardIndex(); }

// The '|'-joined flat-popup choice string for PF_ADD_POPUP.
inline std::string footageFlatChoicesString() {
    std::string s;
    const auto& c = footageCatalog();
    for (size_t i = 0; i < c.size(); ++i) {
        if (i) s += '|';
        s += c[i].flatLabel;
    }
    return s;
}

/* --------------------------- editor cascade ---------------------------- */

// Unique camera names in catalog order (alphabetical), excluding Standard.
inline std::vector<std::string> footageCameras() {
    std::vector<std::string> cams;
    for (const auto& e : footageCatalog()) {
        if (e.camera[0] == '\0') continue;  // Standard has no camera
        if (std::find(cams.begin(), cams.end(), e.camera) == cams.end()) cams.push_back(e.camera);
    }
    return cams;
}

// The Profile-dropdown options for one camera: "Standard (Rec.709)" first (maps
// to the Standard flat index), then that camera's log profiles. Never empty.
struct FootageCameraProfiles {
    std::vector<std::string> labels;
    std::vector<int> flatIndices;
};

inline FootageCameraProfiles footageProfilesForCamera(const std::string& camera) {
    FootageCameraProfiles r;
    r.labels.push_back("Standard (Rec.709)");
    r.flatIndices.push_back(footageStandardIndex());
    for (const auto& e : footageCatalog()) {
        if (camera == e.camera) {
            r.labels.push_back(e.profileLabel);
            r.flatIndices.push_back(e.index);
        }
    }
    return r;
}

// Where a stored flat index sits in the cascade: which camera row, which option
// within that camera. A Standard flat index (ambiguous camera) resolves to the
// first camera's Standard option, per the decision doc's auto-select behavior.
struct FootageCascadePos {
    int cameraIndex;         // index into footageCameras()
    int profileOptionIndex;  // index into footageProfilesForCamera(camera).labels
};

inline FootageCascadePos footageCascadePosForFlat(int flatIndex) {
    if (!footageIndexIsLog(flatIndex)) return {0, 0};
    const std::string cam = footageEntry(flatIndex).camera;
    const auto cams = footageCameras();
    int camIdx = 0;
    for (size_t i = 0; i < cams.size(); ++i) {
        if (cams[i] == cam) {
            camIdx = static_cast<int>(i);
            break;
        }
    }
    const auto profs = footageProfilesForCamera(cam);
    int optIdx = 0;
    for (size_t i = 0; i < profs.flatIndices.size(); ++i) {
        if (profs.flatIndices[i] == flatIndex) {
            optIdx = static_cast<int>(i);
            break;
        }
    }
    return {camIdx, optIdx};
}

}  // namespace core
}  // namespace cg

#endif  // CG_CORE_FOOTAGECATALOG_H
