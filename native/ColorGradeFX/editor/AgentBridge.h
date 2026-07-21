/*
 * AgentBridge.h - the pure, host-agnostic seam for the native editor's agent
 * surfaces (issue: cg-agent-wiring). NO AE SDK, NO Win32, NO ImGui here on
 * purpose: this is the part exercised headlessly by native:agent-test
 * (tests/editor/agent_bridge_test.cpp), so its logic is proven without a running
 * AE and without spawning the bridge subprocess.
 *
 * The native editor cannot run the agent pipeline itself - the vision critic, the
 * rule-based gradeGuard, the auto-grade loop, reference-stats measurement, and
 * cross-clip consistency all live in TypeScript (src/agent + src/core, the
 * correctness oracle). So the editor SPAWNS a short-lived Node subprocess running
 * scripts/agentBridge.ts (see native/docs/adr-agent-execution.md). This header is
 * only the wire protocol + the translation of a returned auto-grade result into
 * the editor's existing ParamEdit queue; the actual CreateProcess spawn, the frame
 * dump, and the file-open dialogs live in EditorWindow.cpp (captain-verified).
 *
 * The wire format mirrors src/agent/bridgeProtocol.ts EXACTLY - a tiny line-based
 * `key value` text (same spirit as the .cube / reference-stats sidecar parsers),
 * so this side needs no JSON library. The committed fixtures in
 * native/tests/fixtures/agent/ are parsed by BOTH sides to guard drift.
 */
#pragma once
#ifndef CG_AGENT_BRIDGE_H
#define CG_AGENT_BRIDGE_H

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "EditorBridge.h"

namespace cg {
namespace editor {

// The four agent surfaces the editor buttons trigger.
enum class AgentCommand { Critique, Autograde, Reference, Batch };

inline const char* agentCommandName(AgentCommand c) {
    switch (c) {
        case AgentCommand::Critique:  return "critique";
        case AgentCommand::Autograde: return "autograde";
        case AgentCommand::Reference: return "reference";
        case AgentCommand::Batch:     return "batch";
    }
    return "critique";
}

// A request the editor writes for the bridge CLI. Paths are native OS paths. The
// Gemini API key is NOT carried here - it is passed to the subprocess via the
// GEMINI_API_KEY environment variable only (BYOK).
struct AgentRequest {
    AgentCommand command = AgentCommand::Critique;
    std::string  mode;            // "correction" | "shot-match"
    std::string  model;           // Gemini model id (empty = CLI default)
    std::string  profile;         // footage decode profile key (vlog/rec709/...)
    std::string  theme;           // theme registry key for auto-grade base
    bool         hasStrength = false;        double strength = 0.0;
    bool         hasSkinProtection = false;  double skinProtection = 0.0;
    bool         hasChromaGain = false;      double chromaGain = 1.0;
    bool         hasRounds = false;          int    rounds = 5;
    std::string  framePath;       // raw RGBA dump (critique/autograde)
    std::string  referencePath;   // reference image (reference / shot-match)
    std::string  outPath;         // reference-stats sidecar output
    std::vector<std::string> clipPaths;   // batch clips
    bool         mock = false;
};

// A single param edit the editor should apply after auto-grade (mirror of
// src/agent/bridgeProtocol.ts AgentApplyEdit; the field names match the wire).
struct AgentApply {
    std::string         field;
    std::vector<double> values;
};

// One diverged clip pair from the batch consistency check.
struct AgentDiverged {
    std::string clipA, clipB, reason;
};

// The result the bridge CLI writes and the editor reads.
struct AgentResponse {
    bool                     ok = false;         // status ok|error
    std::string              message;
    std::vector<std::string> defects;
    std::string              verdict;            // "continue" | "stop" | ""
    bool                     hasBestRound = false;  int bestRound = 0;
    bool                     accepted = false;
    std::string              stopReason;
    std::vector<AgentApply>  apply;
    std::vector<std::string> unmapped;
    bool                     hasCost = false;    double cost = 0.0;
    std::vector<AgentDiverged> diverged;
    bool                     hasPairs = false;   int pairsCompared = 0;
};

// --- request formatting (native writes) -------------------------------------

namespace detail {
// Compact, locale-free number formatting matching the TS side (Number(v.toFixed(6))).
inline std::string fmtNum(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    std::string s(buf);
    // trim trailing zeros / dot
    if (s.find('.') != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (last != std::string::npos && s[last] == '.') --last;
        s.erase(last + 1);
    }
    if (s == "-0") s = "0";
    return s;
}
inline std::string sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back((c == '\r' || c == '\n') ? ' ' : c);
    // trim ends
    size_t b = out.find_first_not_of(' ');
    size_t e = out.find_last_not_of(' ');
    return b == std::string::npos ? std::string() : out.substr(b, e - b + 1);
}
}  // namespace detail

inline std::string formatAgentRequest(const AgentRequest& r) {
    std::ostringstream o;
    o << "command " << agentCommandName(r.command) << "\n";
    if (!r.mode.empty())    o << "mode " << r.mode << "\n";
    if (!r.model.empty())   o << "model " << detail::sanitize(r.model) << "\n";
    if (!r.profile.empty()) o << "profile " << detail::sanitize(r.profile) << "\n";
    if (!r.theme.empty())   o << "theme " << detail::sanitize(r.theme) << "\n";
    if (r.hasStrength)       o << "strength " << detail::fmtNum(r.strength) << "\n";
    if (r.hasSkinProtection) o << "skinProtection " << detail::fmtNum(r.skinProtection) << "\n";
    if (r.hasChromaGain)     o << "chromaGain " << detail::fmtNum(r.chromaGain) << "\n";
    if (r.hasRounds)         o << "rounds " << r.rounds << "\n";
    if (!r.framePath.empty())     o << "frame " << r.framePath << "\n";
    if (!r.referencePath.empty()) o << "reference " << r.referencePath << "\n";
    if (!r.outPath.empty())       o << "out " << r.outPath << "\n";
    for (const auto& c : r.clipPaths) o << "clip " << c << "\n";
    if (r.mock) o << "mock 1\n";
    return o.str();
}

// --- response parsing (native reads) ----------------------------------------

namespace detail {
inline void splitKeyRest(const std::string& line, std::string& key, std::string& rest) {
    size_t sp = line.find(' ');
    if (sp == std::string::npos) { key = line; rest.clear(); return; }
    key = line.substr(0, sp);
    rest = line.substr(sp + 1);
    // trim rest
    size_t b = rest.find_first_not_of(" \t");
    size_t e = rest.find_last_not_of(" \t");
    rest = (b == std::string::npos) ? std::string() : rest.substr(b, e - b + 1);
}
inline std::vector<double> parseNums(const std::string& s) {
    std::vector<double> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(std::strtod(tok.c_str(), nullptr));
    return out;
}
inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
}  // namespace detail

inline AgentResponse parseAgentResponse(const std::string& text) {
    AgentResponse r;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // trim leading whitespace
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        line = line.substr(b);
        std::string key, rest;
        detail::splitKeyRest(line, key, rest);
        if (key == "status") {
            r.ok = (rest == "ok");
        } else if (key == "message") {
            r.message = rest;
        } else if (key == "defect") {
            r.defects.push_back(rest);
        } else if (key == "verdict") {
            r.verdict = rest;
        } else if (key == "bestRound") {
            r.hasBestRound = true; r.bestRound = std::atoi(rest.c_str());
        } else if (key == "accepted") {
            r.accepted = (rest == "true");
        } else if (key == "stopReason") {
            r.stopReason = rest;
        } else if (key == "apply") {
            std::string f, nums;
            detail::splitKeyRest(rest, f, nums);
            AgentApply a;
            a.field = f;
            a.values = detail::parseNums(nums);
            r.apply.push_back(a);
        } else if (key == "unmapped") {
            r.unmapped.push_back(rest);
        } else if (key == "cost") {
            r.hasCost = true; r.cost = std::strtod(rest.c_str(), nullptr);
        } else if (key == "diverged") {
            AgentDiverged d;
            size_t p1 = rest.find('|');
            size_t p2 = rest.find('|', p1 == std::string::npos ? p1 : p1 + 1);
            if (p1 != std::string::npos && p2 != std::string::npos) {
                d.clipA = detail::trim(rest.substr(0, p1));
                d.clipB = detail::trim(rest.substr(p1 + 1, p2 - p1 - 1));
                d.reason = detail::trim(rest.substr(p2 + 1));
            } else {
                d.reason = rest;
            }
            r.diverged.push_back(d);
        } else if (key == "pairsCompared") {
            r.hasPairs = true; r.pairsCompared = std::atoi(rest.c_str());
        }
        // unknown keys ignored (forward compatible)
    }
    return r;
}

// --- translate an auto-grade result into editor ParamEdits ------------------
//
// The editor composes agent edits onto the popup theme via dedicated USER recipe
// fields (Recipe.h applyEditorOverrides): user tints ADD onto the theme, user
// curves REPLACE it, chromaGain is a relative slider. The bridge already produced
// tint DELTAS / a chromaGain RATIO / absolute curves (src/agent/editorApply.ts),
// so here we just route each apply line to the right ParamEdit, folding all tint
// deltas into ONE Wheels edit and all curves into ONE Curves edit (both are single
// arb-recipe payloads). `base` supplies the current wheels/curves to merge into so
// an untouched slot is preserved.

inline void fillCurveFromFlat(CurveState& c, const std::vector<double>& flat) {
    int n = static_cast<int>(flat.size() / 2);
    if (n < 2) return;
    if (n > CG_EDIT_MAX_CURVE_POINTS) n = CG_EDIT_MAX_CURVE_POINTS;
    c.count = n;
    for (int i = 0; i < n; ++i) {
        c.x[i] = flat[i * 2];
        c.y[i] = flat[i * 2 + 1];
    }
    c.dirty = true;
}

inline std::vector<ParamEdit> translateAgentApply(const std::vector<AgentApply>& apply,
                                                   const ParamSnapshot& base) {
    std::vector<ParamEdit> edits;
    WheelsState wheels = base.wheels;   // fold tint deltas into the current wheels
    CurvesState curves = base.curves;   // fold curves into the current curves
    bool wheelsTouched = false, curvesTouched = false;

    for (const auto& a : apply) {
        if (a.field == "strength" && a.values.size() >= 1) {
            edits.push_back(ParamEdit{EditField::Strength, a.values[0]});
        } else if (a.field == "skinProtection" && a.values.size() >= 1) {
            edits.push_back(ParamEdit{EditField::SkinProtection, a.values[0]});
        } else if (a.field == "chromaGain" && a.values.size() >= 1) {
            edits.push_back(ParamEdit{EditField::ChromaGain, a.values[0]});
        } else if (a.field == "shadowTint" && a.values.size() >= 2) {
            wheels.hasShadowTint = true; wheels.shadowTint[0] = a.values[0]; wheels.shadowTint[1] = a.values[1];
            wheelsTouched = true;
        } else if (a.field == "midtoneTint" && a.values.size() >= 2) {
            wheels.hasMidTint = true; wheels.midTint[0] = a.values[0]; wheels.midTint[1] = a.values[1];
            wheelsTouched = true;
        } else if (a.field == "highlightTint" && a.values.size() >= 2) {
            wheels.hasHighTint = true; wheels.highTint[0] = a.values[0]; wheels.highTint[1] = a.values[1];
            wheelsTouched = true;
        } else if (a.field == "toneCurve") {
            fillCurveFromFlat(curves.master, a.values); curvesTouched = true;
        } else if (a.field == "channelR") {
            fillCurveFromFlat(curves.r, a.values); curvesTouched = true;
        } else if (a.field == "channelG") {
            fillCurveFromFlat(curves.g, a.values); curvesTouched = true;
        } else if (a.field == "channelB") {
            fillCurveFromFlat(curves.b, a.values); curvesTouched = true;
        }
    }
    if (wheelsTouched) {
        ParamEdit e{EditField::Wheels, 0.0};
        e.wheels = wheels;
        edits.push_back(e);
    }
    if (curvesTouched) {
        ParamEdit e{EditField::Curves, 0.0};
        e.curves = curves;
        edits.push_back(e);
    }
    return edits;
}

// --- async job status (for the panel's running/result/error UI) -------------
//
// Pure enum + result holder; the window owns a mutex-guarded instance the worker
// thread fills and the UI thread reads. Keeping it here lets the state model be
// reasoned about (and, if needed, unit-tested) apart from the threading.
enum class AgentJobState { Idle, Running, Done, Failed };

struct AgentJobResult {
    AgentJobState state = AgentJobState::Idle;
    AgentCommand  command = AgentCommand::Critique;
    AgentResponse response;   // valid when state == Done
    std::string   error;      // set when state == Failed (spawn/read failure etc.)
};

// --- persisted agent settings (cg-agent-fixes-v4) ---------------------------
//
// The editor persists its BYOK key and the resolved bridge location so the agent
// surfaces work "as installed" - the panel process AE launches never inherits a
// shell's CG_AGENT_BRIDGE / GEMINI_API_KEY env, so relying on those alone left the
// feature dead (bridge "not configured", key re-typed every session). This is the
// pure, host-agnostic model of that config file (a tiny `key=value` text, same
// spirit as the .cube / reference-stats sidecars); the Win32 file I/O, the
// %APPDATA% location, and the DPAPI encrypt/decrypt of the key live in
// EditorWindow.cpp. Keeping parse/format/precedence here lets native:agent-test
// prove them without AE. Unknown keys are preserved on round-trip (forward compat).
//
// Recognised keys: `bridge` (runnable bridge path), `node` (launcher override),
// `apiKeyEnc` (DPAPI-protected key, hex - never the plaintext key).
struct AgentConfig {
    std::vector<std::pair<std::string, std::string> > entries;

    std::string get(const std::string& key) const {
        for (const auto& kv : entries) if (kv.first == key) return kv.second;
        return std::string();
    }
    // Set (replace) a key; an empty value REMOVES it so a cleared field doesn't
    // linger in the file (e.g. Remove-key deletes apiKeyEnc entirely).
    void set(const std::string& key, const std::string& value) {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->first == key) {
                if (value.empty()) entries.erase(it);
                else it->second = value;
                return;
            }
        }
        if (!value.empty()) entries.push_back(std::make_pair(key, value));
    }
};

inline AgentConfig parseAgentConfig(const std::string& text) {
    AgentConfig cfg;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = detail::trim(line);
        if (t.empty() || t[0] == '#') continue;   // blank / comment
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;      // not key=value: skip
        std::string key = detail::trim(t.substr(0, eq));
        std::string val = detail::trim(t.substr(eq + 1));
        if (key.empty()) continue;
        cfg.set(key, val);
    }
    return cfg;
}

inline std::string formatAgentConfig(const AgentConfig& cfg) {
    std::ostringstream o;
    o << "# ColorGradeFX agent settings (auto-managed). Do not commit; contains a\n"
         "# DPAPI-protected API key (apiKeyEnc). bridge/node point at the Node bridge.\n";
    for (const auto& kv : cfg.entries) o << kv.first << "=" << kv.second << "\n";
    return o.str();
}

// Choose the runnable bridge path by precedence: an explicit CG_AGENT_BRIDGE env
// override wins; else the persisted `bridge=` from the settings file; else a path
// discovered next to the plug-in. Each argument is passed already-validated (the
// caller only supplies a candidate that exists on disk / is non-empty), so this is
// a pure first-non-empty pick - the single place the precedence order is defined.
inline std::string chooseBridgePath(const std::string& envOverride,
                                    const std::string& configBridge,
                                    const std::string& discoveredProbe) {
    if (!envOverride.empty())   return envOverride;
    if (!configBridge.empty())  return configBridge;
    return discoveredProbe;     // may be empty -> "not configured"
}

// Choose the launcher: CG_AGENT_NODE env override, else the persisted `node=`, else
// the default ("node", which the spawner auto-upgrades to "npx tsx" for a .ts bridge).
inline std::string chooseLauncher(const std::string& envOverride, const std::string& configNode) {
    if (!envOverride.empty())  return envOverride;
    if (!configNode.empty())   return configNode;
    return "node";
}

}  // namespace editor
}  // namespace cg

#endif  // CG_AGENT_BRIDGE_H
