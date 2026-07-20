/*
 * agent_bridge_test.cpp - headless unit test for the pure agent-bridge seam
 * (editor/AgentBridge.h). Compiled and run by `npm run native:agent-test`
 * (native/scripts/agent-bridge-test.ts) with g++/clang, OUTSIDE After Effects and
 * without spawning the Node bridge. Mirrors the native:editor-test convention
 * (local compiler, deliberately NOT in CI).
 *
 * What it covers:
 *   - formatAgentRequest emits the exact line-based wire the TS side parses,
 *   - parseAgentResponse round-trips a full auto-grade + a batch response,
 *   - the COMMITTED fixtures in native/tests/fixtures/agent/ (the cross-language
 *     contract, also parsed by tests/unit/agentBridge.test.ts) parse identically,
 *   - translateAgentApply routes edits to the right ParamEdit: scalar params, tint
 *     deltas folded into ONE Wheels edit, curves folded into ONE Curves edit.
 *
 * Self-asserting: returns non-zero and prints the first failure on any mismatch.
 */
#include "../../ColorGradeFX/editor/AgentBridge.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace cg::editor;

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);   \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static bool approx(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// --- request formatting ------------------------------------------------------
static void test_format_request() {
    AgentRequest r;
    r.command = AgentCommand::Autograde;
    r.mode = "correction";
    r.model = "gemini-flash-latest";
    r.profile = "vlog";
    r.theme = "teal-orange";
    r.hasStrength = true; r.strength = 0.8;
    r.hasSkinProtection = true; r.skinProtection = 0.75;
    r.hasChromaGain = true; r.chromaGain = 1.0;
    r.hasRounds = true; r.rounds = 4;
    r.framePath = "/tmp/cg-agent-frame.bin";
    std::string out = formatAgentRequest(r);
    CHECK(contains(out, "command autograde\n"), "request has command");
    CHECK(contains(out, "profile vlog\n"), "request has profile");
    CHECK(contains(out, "theme teal-orange\n"), "request has theme");
    CHECK(contains(out, "strength 0.8\n"), "request strength compact-formatted");
    CHECK(contains(out, "chromaGain 1\n"), "request chromaGain trims to 1");
    CHECK(contains(out, "rounds 4\n"), "request rounds");
    CHECK(contains(out, "frame /tmp/cg-agent-frame.bin\n"), "request frame path");

    // batch request with clips + mock
    AgentRequest b;
    b.command = AgentCommand::Batch;
    b.theme = "warm-film";
    b.clipPaths = {"/a.tif", "/b.png"};
    b.mock = true;
    std::string bo = formatAgentRequest(b);
    CHECK(contains(bo, "clip /a.tif\n"), "batch clip a");
    CHECK(contains(bo, "clip /b.png\n"), "batch clip b");
    CHECK(contains(bo, "mock 1\n"), "batch mock flag");
}

// --- response parsing --------------------------------------------------------
static void test_parse_response() {
    std::string text =
        "status ok\n"
        "message applied round 2\n"
        "defect residual color cast (6.2 LAB units)\n"
        "defect chroma overshoot in highlights\n"
        "verdict continue\n"
        "bestRound 2\n"
        "accepted true\n"
        "stopReason guard rejected round 3 as a regression; kept round 2\n"
        "apply strength 0.8\n"
        "apply shadowTint 5 -5\n"
        "apply toneCurve 0 0 0.5 0.55 1 1\n"
        "unmapped chromaShape.softLimit=22\n"
        "cost 0\n";
    AgentResponse r = parseAgentResponse(text);
    CHECK(r.ok, "status ok");
    CHECK(r.message == "applied round 2", "message rest-of-line");
    CHECK(r.defects.size() == 2, "two defects");
    CHECK(r.defects[1] == "chroma overshoot in highlights", "second defect");
    CHECK(r.verdict == "continue", "verdict");
    CHECK(r.hasBestRound && r.bestRound == 2, "bestRound");
    CHECK(r.accepted, "accepted");
    CHECK(contains(r.stopReason, "kept round 2"), "stopReason rest-of-line");
    CHECK(r.apply.size() == 3, "three apply edits");
    CHECK(r.apply[0].field == "strength" && approx(r.apply[0].values[0], 0.8), "apply strength");
    CHECK(r.apply[1].field == "shadowTint" && r.apply[1].values.size() == 2 &&
              approx(r.apply[1].values[1], -5), "apply shadowTint two values");
    CHECK(r.apply[2].field == "toneCurve" && r.apply[2].values.size() == 6, "apply toneCurve six values");
    CHECK(r.unmapped.size() == 1 && contains(r.unmapped[0], "softLimit"), "unmapped");
    CHECK(r.hasCost && approx(r.cost, 0.0), "cost");
}

static void test_parse_batch_response() {
    std::string text =
        "status ok\n"
        "message 1 of 3 pair(s) diverged\n"
        "diverged clipA.tif | clipB.tif | exposure drift 0.42; cast direction 31deg\n"
        "pairsCompared 3\n";
    AgentResponse r = parseAgentResponse(text);
    CHECK(r.ok, "batch status ok");
    CHECK(r.diverged.size() == 1, "one diverged pair");
    CHECK(r.diverged[0].clipA == "clipA.tif", "diverged clipA");
    CHECK(r.diverged[0].clipB == "clipB.tif", "diverged clipB");
    CHECK(contains(r.diverged[0].reason, "exposure drift"), "diverged reason");
    CHECK(r.hasPairs && r.pairsCompared == 3, "pairsCompared");
}

static void test_parse_error_response() {
    AgentResponse r = parseAgentResponse("status error\nmessage no billing on paid model\n");
    CHECK(!r.ok, "status error");
    CHECK(contains(r.message, "no billing"), "error message carried");
}

// --- committed fixtures (cross-language contract) ---------------------------
static void test_fixtures(const std::string& fixDir) {
    std::string reqText = readFile(fixDir + "/request.txt");
    CHECK(contains(reqText, "command autograde"), "fixture request loaded");

    AgentResponse resp = parseAgentResponse(readFile(fixDir + "/response.txt"));
    CHECK(resp.ok, "fixture response ok");
    CHECK(resp.defects.size() == 2, "fixture two defects");
    CHECK(resp.hasBestRound && resp.bestRound == 2, "fixture bestRound");
    bool sawShadow = false, sawTone = false;
    for (const auto& a : resp.apply) {
        if (a.field == "shadowTint" && a.values.size() == 2 && approx(a.values[0], 5)) sawShadow = true;
        if (a.field == "toneCurve" && a.values.size() == 6) sawTone = true;
    }
    CHECK(sawShadow, "fixture shadowTint apply");
    CHECK(sawTone, "fixture toneCurve apply");

    AgentResponse batch = parseAgentResponse(readFile(fixDir + "/batch-response.txt"));
    CHECK(batch.diverged.size() == 1, "fixture batch diverged");
    CHECK(batch.diverged[0].clipA == "clipA.tif", "fixture batch clipA");
    CHECK(batch.hasPairs && batch.pairsCompared == 3, "fixture batch pairs");
}

// --- translate apply -> ParamEdits ------------------------------------------
static void test_translate_apply() {
    std::vector<AgentApply> apply = {
        {"strength", {0.6}},
        {"skinProtection", {0.8}},
        {"chromaGain", {0.5}},
        {"shadowTint", {3, -3}},
        {"highlightTint", {4, -4}},
        {"toneCurve", {0, 0, 0.5, 0.6, 1, 1}},
        {"channelR", {0, 0, 0.7, 0.72, 1, 1}},
    };
    ParamSnapshot base;  // neutral wheels/curves
    std::vector<ParamEdit> edits = translateAgentApply(apply, base);

    // Expect: 3 scalar edits + 1 Wheels + 1 Curves = 5.
    CHECK(edits.size() == 5, "translate produced 5 edits");

    bool sStr = false, sSkin = false, sChroma = false;
    const WheelsState* wheels = nullptr;
    const CurvesState* curves = nullptr;
    for (const auto& e : edits) {
        if (e.field == EditField::Strength) { sStr = true; CHECK(approx(e.value, 0.6), "strength value"); }
        if (e.field == EditField::SkinProtection) { sSkin = true; CHECK(approx(e.value, 0.8), "skin value"); }
        if (e.field == EditField::ChromaGain) { sChroma = true; CHECK(approx(e.value, 0.5), "chroma value"); }
        if (e.field == EditField::Wheels) wheels = &e.wheels;
        if (e.field == EditField::Curves) curves = &e.curves;
    }
    CHECK(sStr && sSkin && sChroma, "all three scalar edits present");
    CHECK(wheels != nullptr, "one folded Wheels edit");
    CHECK(curves != nullptr, "one folded Curves edit");
    if (wheels) {
        CHECK(wheels->hasShadowTint && approx(wheels->shadowTint[0], 3) && approx(wheels->shadowTint[1], -3),
              "wheels shadow tint delta");
        CHECK(wheels->hasHighTint && approx(wheels->highTint[0], 4), "wheels high tint delta");
        CHECK(!wheels->hasMidTint, "wheels mid tint untouched");
    }
    if (curves) {
        CHECK(curves->master.count == 3 && curves->master.dirty, "master curve filled + dirty");
        CHECK(approx(curves->master.y[1], 0.6), "master curve mid y");
        CHECK(curves->r.count == 3 && curves->r.dirty, "R curve filled");
        CHECK(curves->g.count == 0, "G curve untouched");
    }

    // Empty apply -> no edits.
    CHECK(translateAgentApply({}, base).empty(), "empty apply -> no edits");
}

// --- persisted settings + bridge resolution (cg-agent-fixes-v4) -------------
static void test_agent_config() {
    // Parse a real settings file with a comment, blank lines, and CRLF.
    std::string text =
        "# ColorGradeFX agent settings\r\n"
        "\r\n"
        "bridge=C:\\dev\\color-grade-plugin\\scripts\\agentBridge.ts\r\n"
        "node=npx tsx\r\n"
        "apiKeyEnc=deadbeef01\r\n";
    AgentConfig cfg = parseAgentConfig(text);
    CHECK(cfg.get("bridge") == "C:\\dev\\color-grade-plugin\\scripts\\agentBridge.ts", "cfg bridge");
    CHECK(cfg.get("node") == "npx tsx", "cfg node (value keeps spaces)");
    CHECK(cfg.get("apiKeyEnc") == "deadbeef01", "cfg apiKeyEnc");
    CHECK(cfg.get("missing").empty(), "cfg missing key -> empty");

    // Round-trip preserves all entries.
    AgentConfig re = parseAgentConfig(formatAgentConfig(cfg));
    CHECK(re.get("bridge") == cfg.get("bridge"), "round-trip bridge");
    CHECK(re.get("apiKeyEnc") == cfg.get("apiKeyEnc"), "round-trip apiKeyEnc");

    // set() replaces; empty value REMOVES (so Remove-key deletes the line entirely).
    cfg.set("apiKeyEnc", "");
    CHECK(cfg.get("apiKeyEnc").empty(), "cleared apiKeyEnc removed");
    CHECK(formatAgentConfig(cfg).find("apiKeyEnc=") == std::string::npos, "apiKeyEnc entry absent after clear");
    CHECK(cfg.get("bridge") == "C:\\dev\\color-grade-plugin\\scripts\\agentBridge.ts",
          "clearing one key preserves the others");
    cfg.set("node", "node");  // replace in place
    CHECK(cfg.get("node") == "node", "set() replaces existing");
}

static void test_bridge_precedence() {
    // env override always wins.
    CHECK(chooseBridgePath("E:\\env.ts", "C:\\cfg.ts", "D:\\probe.mjs") == "E:\\env.ts",
          "env override wins");
    // no env -> config next.
    CHECK(chooseBridgePath("", "C:\\cfg.ts", "D:\\probe.mjs") == "C:\\cfg.ts", "config beats probe");
    // no env/config -> discovered probe.
    CHECK(chooseBridgePath("", "", "D:\\probe.mjs") == "D:\\probe.mjs", "probe is last resort");
    // nothing -> empty ("not configured").
    CHECK(chooseBridgePath("", "", "").empty(), "nothing configured -> empty");

    // launcher precedence: env -> config -> default "node".
    CHECK(chooseLauncher("npx tsx", "node") == "npx tsx", "launcher env wins");
    CHECK(chooseLauncher("", "custom-node") == "custom-node", "launcher config next");
    CHECK(chooseLauncher("", "") == "node", "launcher defaults to node");
}

int main(int argc, char** argv) {
    std::string fixDir = argc > 1 ? argv[1] : "native/tests/fixtures/agent";
    test_format_request();
    test_parse_response();
    test_parse_batch_response();
    test_parse_error_response();
    test_fixtures(fixDir);
    test_translate_apply();
    test_agent_config();
    test_bridge_precedence();
    if (g_failures) {
        std::printf("agent_bridge_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("agent_bridge_test: all checks passed\n");
    return 0;
}
