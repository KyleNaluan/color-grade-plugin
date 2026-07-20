/*
 * EditorWindow.cpp - Win32 + D3D11 + Dear ImGui backend for the Phase 3 editor
 * spike. Implements EditorHost (EditorWindow.h): a single-instance-per-effect
 * registry of modeless windows, each on its own UI thread, wired to the effect
 * through the pure EditorBridge.h seam.
 *
 * Toolkit chosen in native/docs/adr-editor-ui.md (captain-approved: Dear ImGui,
 * vendored/compiled into the .aex - zero external runtime deps, D3D11/DXGI are
 * OS-provided). This hosts the full Correct/Grade control set: Correct = Footage
 * profile; Grade = Theme, Strength, Skin Protection, Chroma Gain, LUT Source - each
 * round-trips through the bridge to the effect's params. Live preview + scopes
 * (Phases 4-5) are laid out as the center placeholder.
 *
 * Windows-only. On any non-Windows build every EditorHost method is a no-op stub
 * (bottom of file) so the interface still links; the macOS backend (Cocoa + Metal)
 * lands behind the same header when the Mac target opens.
 */
#include "EditorWindow.h"

#ifdef _WIN32

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>
#include <commdlg.h>   // cg-agent-wiring: GetOpenFileName for the reference/batch pickers
#include <d3d11.h>

#pragma comment(lib, "comdlg32.lib")

#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/backends/imgui_impl_win32.h"
#include "../../third_party/imgui/backends/imgui_impl_dx11.h"

#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>

#include <vector>

#include "AgentBridge.h"  // cg-agent-wiring: pure agent-bridge protocol + apply translation
#include "Scopes.h"  // Phase 5: waveform / histogram / vectorscope image synthesis
#include "../core/FootageCatalog.h"  // multi-camera footage-profile catalog (Camera->Profile cascade)
#include "../core/MonotoneCurve.h"  // Phase 6b: shape-preserving PCHIP for the curve preview
#include "../core/Lab.h"      // Phase 6c: LAB->RGB for the 3-way tint disc color ring
#include "../core/Rec709.h"   // Phase 6c: encode the tint ring's linear RGB to display

// Forward-decl of the backend's Win32 message handler (defined in imgui_impl_win32.cpp).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Storage for imconfig.h's thread-local GImGui override (#define GImGui MyImGuiTLS).
// Each editor UI thread gets its own current-context pointer so two open windows
// never race a shared global context.
thread_local ImGuiContext* MyImGuiTLS = nullptr;

namespace cg {
namespace editor {

namespace {

constexpr wchar_t kWindowClass[] = L"CG_ColorGradeEditorWindow";

// Plugin DLL module handle (so our window class is owned by the plugin, not the
// host exe - survives plugin reloads without a class-name clash).
HINSTANCE PluginInstance() {
    HMODULE hm = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&PluginInstance), &hm);
    return reinterpret_cast<HINSTANCE>(hm);
}

// A GPU RGBA8 texture + its shader-resource view and current size. Owned by the window's
// UI thread. Reused by the live preview, the before frame, and each scope (Phase 4/5).
struct GpuTex {
    ID3D11Texture2D*          tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int                       w = 0;
    int                       h = 0;
    bool ready() const { return srv != nullptr && w > 0 && h > 0; }
};

// One window instance: its own thread, D3D11 device/swapchain, ImGui context, and
// the bridge state shared with the effect (snapshot in, edits out).
struct WindowImpl {
    InstanceKey key = 0;

    std::thread          thread;
    std::atomic<bool>    running{false};    // false asks the UI loop to exit
    std::atomic<bool>    finished{false};   // true once the thread fully tore down
    std::atomic<bool>    closeRequested{false};  // user clicked the window's close box
    std::atomic<bool>    stopRequested{false};   // teardown requested; survives startup

    // Bridge: effect -> window (snapshot) guarded; window -> effect (queue) is
    // itself thread-safe. seededTheme etc. let the UI init from the effect's state.
    std::mutex     snapMutex;
    ParamSnapshot  snapshot;   // latest published effect params
    EditQueue      edits;      // pending user edits for the effect to drain

    std::atomic<HWND>       hwnd{nullptr};   // written by UI thread, read from main thread
    ImGuiContext*           imguiCtx = nullptr;  // per-window ImGui context (UI thread only)
    ID3D11Device*           device = nullptr;
    ID3D11DeviceContext*    context = nullptr;
    IDXGISwapChain*         swapChain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;

    // Phase 4 live preview: the effect pushes a CPU frame (pendingPreview); the UI
    // thread uploads it into previewTex (recreated on a size change) and draws it. The
    // texture + its SRV are owned by the UI thread; only pendingPreview crosses threads.
    std::mutex                         previewMutex;
    std::shared_ptr<const PreviewFrame> pendingPreview;   // newest frame awaiting upload
    bool                               previewDirty = false;
    GpuTex                             previewTex;   // UI thread only

    // Phase 5 before/after: the effect pushes the ORIGINAL decoded frame (pendingBefore);
    // the UI thread uploads it and draws it against the graded preview per compareMode.
    std::mutex                         beforeMutex;
    std::shared_ptr<const PreviewFrame> pendingBefore;
    bool                               beforeDirty = false;
    GpuTex                             beforeTex;    // UI thread only

    // Phase 5 scopes: recomputed on the UI thread from the newest graded preview frame
    // (Scopes.h) and uploaded into these textures. UI thread only.
    GpuTex waveTex, histTex, vecTex;
    std::shared_ptr<const PreviewFrame> lastPreviewForScopes;  // source of the current scopes

    // Phase 5 analysis status (effect -> window), shown as an "Analyzing n/N" badge.
    std::mutex     statusMutex;
    AnalysisStatus analysisStatus;

    // Phase 5 UI state written by the UI thread, read by the bridge getters (atomics):
    //   compareMode 0=AfterOnly 1=BeforeOnly 2=Split; wantsBefore = compareMode != 0.
    std::atomic<int>  compareMode{0};
    std::atomic<bool> showScopes{true};
    std::atomic<bool> analyzeRequested{false};  // one-shot: "Analyze" button clicked
    float             splitFraction = 0.5f;      // UI-thread only: split divider position
    // Curve editor (Phase 6b), UI-thread only: which control point (if any) is mid-drag
    // per curve [master, R, G, B]; -1 = none. Persists across frames during a drag.
    int               curveDrag[4] = {-1, -1, -1, -1};
    // cg-ui-polish, all UI-thread only: Curves-tab active channel (0=M,1=R,2=G,3=B); the
    // collapsible confined-indigo agent dock's expand + BYOK-key state (session-local); and
    // the agent dock's inner tab. The agent dock is the single Pro/BYOK seam (kAgentDockEnabled).
    int               curveChannel = 0;
    // Correct tab: the camera the user last picked in the cascade. footageProfile alone can't
    // drive the Camera combo, because Standard (flat index 1) is camera-ambiguous and always
    // resolves back to the first camera - so a fresh camera choice whose profile is Standard
    // would snap the dropdown back to camera 0 (the round-1 "only ARRI selectable" bug). -1 =
    // derive from footageProfile.
    int               footageCamera = -1;
    bool              agentOpen = false;      // agent dock expanded (default: out of the way)
    bool              agentKeySet = false;    // a Gemini key was entered this session (BYOK)
    char              agentKeyBuf[160] = {0}; // key entry buffer (never persisted here)
    int               agentTab = 0;           // 0 = Critique, 1 = Reference, 2 = Batch

    // cg-agent-wiring: the agent EXECUTES from the editor by spawning a short-lived Node
    // subprocess (scripts/agentBridge.ts) on a worker thread; the pure protocol + apply
    // translation live in AgentBridge.h. This state is the UI-visible job status the worker
    // fills and the panel polls. Exactly one job at a time (agentBusy guards launch).
    std::mutex               agentMutex;         // guards agentResult + agentBusy transitions
    AgentJobResult           agentResult;        // Idle/Running/Done/Failed + response/error
    std::thread              agentThread;        // the in-flight worker (joined at teardown)
    std::atomic<void*>       agentChildProcess{nullptr};  // live bridge child HANDLE, so teardown can TerminateProcess it and not block the join up to 120s
    std::atomic<bool>        agentBusy{false};   // a job is running (blocks a second launch)
    std::atomic<bool>        agentWantsFrame{false};  // ask the idle hook to publish the source frame
    bool                     agentApplied = false;    // UI-thread: accepted result already applied once
    std::string              agentRefSidecar;         // where the last reference measurement was written

    ParamSnapshot readSnapshot() {
        std::lock_guard<std::mutex> lk(snapMutex);
        return snapshot;
    }
    void writeSnapshot(const ParamSnapshot& s) {
        std::lock_guard<std::mutex> lk(snapMutex);
        // Only accept a strictly newer publish, so a stale re-publish can't stomp
        // the value the user is mid-drag on (see ParamSnapshot::revision).
        if (s.revision >= snapshot.revision) snapshot = s;
    }
    void writePreview(std::shared_ptr<const PreviewFrame> f) {
        std::lock_guard<std::mutex> lk(previewMutex);
        pendingPreview = std::move(f);
        previewDirty = true;
    }
    // UI thread: take the newest pending frame if one arrived since the last upload.
    std::shared_ptr<const PreviewFrame> takePendingPreview() {
        std::lock_guard<std::mutex> lk(previewMutex);
        if (!previewDirty) return nullptr;
        previewDirty = false;
        return pendingPreview;  // keep it (harmless) so a resize can re-upload if needed
    }
    void writeBefore(std::shared_ptr<const PreviewFrame> f) {
        std::lock_guard<std::mutex> lk(beforeMutex);
        pendingBefore = std::move(f);
        beforeDirty = true;
    }
    std::shared_ptr<const PreviewFrame> takePendingBefore() {
        std::lock_guard<std::mutex> lk(beforeMutex);
        if (!beforeDirty) return nullptr;
        beforeDirty = false;
        return pendingBefore;
    }
    void writeStatus(const AnalysisStatus& s) {
        std::lock_guard<std::mutex> lk(statusMutex);
        analysisStatus = s;
    }
    AnalysisStatus readStatus() {
        std::lock_guard<std::mutex> lk(statusMutex);
        return analysisStatus;
    }
};

// --- D3D11 device / render target helpers (canonical ImGui dx11 example) -----

bool CreateRenderTarget(WindowImpl* w) {
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(w->swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer) return false;
    HRESULT hr = w->device->CreateRenderTargetView(backBuffer, nullptr, &w->rtv);
    backBuffer->Release();
    return SUCCEEDED(hr);
}

void CleanupRenderTarget(WindowImpl* w) {
    if (w->rtv) { w->rtv->Release(); w->rtv = nullptr; }
}

// --- RGBA8 GPU textures (UI thread only) ------------------------------------
// Shared by the live preview, the before frame, and each scope image.

void ReleaseGpuTex(GpuTex& t) {
    if (t.srv) { t.srv->Release(); t.srv = nullptr; }
    if (t.tex) { t.tex->Release(); t.tex = nullptr; }
    t.w = 0;
    t.h = 0;
}

// Upload tightly-packed RGBA8 pixels into `t`, (re)creating the texture + SRV on a size
// change and updating pixels in place otherwise. Runs on the window's UI thread with its
// D3D device. Leaves the SRV null (and returns) on any failure so the draw path falls back
// to the placeholder rather than showing garbage.
void UploadRGBA(ID3D11Device* dev, ID3D11DeviceContext* ctx, GpuTex& t,
                const uint8_t* rgba, int width, int height) {
    if (!dev || !ctx || !rgba || width <= 0 || height <= 0) return;
    if (!t.tex || t.w != width || t.h != height) {
        ReleaseGpuTex(t);
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width = static_cast<UINT>(width);
        td.Height = static_cast<UINT>(height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA sd;
        ZeroMemory(&sd, sizeof(sd));
        sd.pSysMem = rgba;
        sd.SysMemPitch = static_cast<UINT>(width) * 4u;

        if (FAILED(dev->CreateTexture2D(&td, &sd, &t.tex)) || !t.tex) {
            t.tex = nullptr;
            return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srvd;
        ZeroMemory(&srvd, sizeof(srvd));
        srvd.Format = td.Format;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(t.tex, &srvd, &t.srv))) {
            ReleaseGpuTex(t);
            return;
        }
        t.w = width;
        t.h = height;
    } else {
        ctx->UpdateSubresource(t.tex, 0, nullptr, rgba, static_cast<UINT>(width) * 4u, 0);
    }
}

inline void UploadFrame(WindowImpl* w, GpuTex& t, const PreviewFrame& f) {
    if (f.valid()) UploadRGBA(w->device, w->context, t, f.rgba.data(), f.width, f.height);
}
inline void UploadScope(WindowImpl* w, GpuTex& t, const ScopeImage& s) {
    if (s.valid()) UploadRGBA(w->device, w->context, t, s.rgba.data(), s.width, s.height);
}

// Recompute the three scopes from the newest graded preview frame and upload them. Runs on
// the UI thread; cheap because the preview frame is already decimated to <= 960px.
void UpdateScopes(WindowImpl* w, const PreviewFrame& f) {
    if (!f.valid()) return;
    Histogram hist = computeHistogram(f.rgba.data(), f.width, f.height, 256);
    UploadScope(w, w->histTex, renderHistogram(hist, 256, 128));
    UploadScope(w, w->waveTex, renderWaveform(f.rgba.data(), f.width, f.height, 256, 128));
    UploadScope(w, w->vecTex, renderVectorscope(f.rgba.data(), f.width, f.height, 160));
}

bool CreateDeviceD3D(WindowImpl* w) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = w->hwnd.load();
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
        D3D11_SDK_VERSION, &sd, &w->swapChain, &w->device, &featureLevel, &w->context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {  // fall back to the WARP software rasterizer
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2,
            D3D11_SDK_VERSION, &sd, &w->swapChain, &w->device, &featureLevel, &w->context);
    }
    if (FAILED(hr)) return false;
    return CreateRenderTarget(w);
}

void CleanupDeviceD3D(WindowImpl* w) {
    CleanupRenderTarget(w);
    if (w->swapChain) { w->swapChain->Release(); w->swapChain = nullptr; }
    if (w->context)   { w->context->Release();   w->context = nullptr; }
    if (w->device)    { w->device->Release();    w->device = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    WindowImpl* w = reinterpret_cast<WindowImpl*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
        case WM_SIZE:
            if (w && w->device && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget(w);
                w->swapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                                            DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget(w);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // disable ALT app menu
            break;
        case WM_CLOSE:
            // User dismissed the window: ask the UI loop to exit and flag the effect.
            if (w) { w->closeRequested.store(true); w->running.store(false); }
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

void EnsureWindowClass() {
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = PluginInstance();
        wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        ::RegisterClassExW(&wc);
    });
}

// ============================================================================
// cg-ui-polish - "Darkroom" visual language, ported from the captain-approved
// mockup (firstmate/data/cg-ui-mockup-final/mockup-final.html). Warm near-black
// grading world with ONE disciplined brass "safelight" accent; the agent dock is a
// confined cooler-indigo world (its palette lives in the agent draw code). This is a
// presentation-layer overhaul: the bridge/edit-push seam and the engine are untouched.
// ============================================================================

// Warm grading-world palette (mockup :root). ImU32 for ImDrawList; ImVec4 where a style
// slot or a widget colour is needed.
constexpr ImU32 COL_BG       = IM_COL32(0x10, 0x0f, 0x0e, 255);
constexpr ImU32 COL_BG2      = IM_COL32(0x15, 0x13, 0x12, 255);
constexpr ImU32 COL_PANEL    = IM_COL32(0x19, 0x17, 0x16, 255);
constexpr ImU32 COL_RAISE    = IM_COL32(0x21, 0x1e, 0x1c, 255);
constexpr ImU32 COL_RAISE2   = IM_COL32(0x26, 0x22, 0x20, 255);
constexpr ImU32 COL_LINE     = IM_COL32(0x2b, 0x27, 0x24, 255);
constexpr ImU32 COL_LINESOFT = IM_COL32(0x22, 0x1f, 0x1d, 255);
constexpr ImU32 COL_HAIR     = IM_COL32(0x0a, 0x09, 0x08, 255);
constexpr ImU32 COL_INK      = IM_COL32(0xef, 0xe9, 0xe2, 255);
constexpr ImU32 COL_INK2     = IM_COL32(0xa4, 0x9c, 0x92, 255);
constexpr ImU32 COL_INK3     = IM_COL32(0x6f, 0x67, 0x5e, 255);
constexpr ImU32 COL_INK4     = IM_COL32(0x4a, 0x44, 0x3d, 255);
constexpr ImU32 COL_BRASS    = IM_COL32(0xc7, 0x9a, 0x5a, 255);
constexpr ImU32 COL_BRASS2   = IM_COL32(0xe0, 0xbd, 0x7f, 255);
constexpr ImU32 COL_BRASSDIM = IM_COL32(0x7a, 0x5f, 0x38, 255);
constexpr ImU32 COL_OK       = IM_COL32(0x5a, 0xa0, 0x66, 255);
constexpr ImU32 COL_ERR      = IM_COL32(0xd0, 0x6a, 0x5a, 255);  // muted terracotta for error text
// Confined agent-world palette (indigo signal; never touches a grading surface).
constexpr ImU32 COL_IRIS     = IM_COL32(0x81, 0x80, 0xf2, 255);
constexpr ImU32 COL_IRIS2    = IM_COL32(0xb7, 0xb6, 0xfb, 255);
constexpr ImU32 COL_IRISDIM  = IM_COL32(0x4a, 0x4a, 0x86, 255);
constexpr ImU32 COL_IRISBG   = IM_COL32(0x14, 0x14, 0x25, 255);
constexpr ImU32 COL_IRISPANEL= IM_COL32(0x17, 0x17, 0x30, 255);
constexpr ImU32 COL_IRISRAISE= IM_COL32(0x1f, 0x1f, 0x3a, 255);
constexpr ImU32 COL_IRISLINE = IM_COL32(0x2d, 0x2d, 0x4c, 255);

inline ImVec4 V4(ImU32 c) {
    return ImVec4(((c >> IM_COL32_R_SHIFT) & 0xff) / 255.0f, ((c >> IM_COL32_G_SHIFT) & 0xff) / 255.0f,
                  ((c >> IM_COL32_B_SHIFT) & 0xff) / 255.0f, ((c >> IM_COL32_A_SHIFT) & 0xff) / 255.0f);
}
inline ImU32 WithAlpha(ImU32 c, int a) { return (c & 0x00ffffff) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT); }

// The single Pro/BYOK seam: the entire agent dock (critique / auto-grade / reference /
// batch) integrates behind this one capability check, per data/cg-monetization-decision.md.
// Free core (decode/Correct/Basics/curves/wheels/preview/scopes) never depends on it; a
// later license/entitlement gate flips this flag alone, no refactor. See AGENTS.md.
constexpr bool kAgentDockEnabled = true;

// Apply the Darkroom style (replaces StyleColorsDark). Rounded, airy, warm; brass is the
// only accent, reserved for the active/hover grading state.
void ApplyColorGradeStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 9.0f;
    s.FrameRounding = 8.0f;
    s.GrabRounding = 8.0f;
    s.PopupRounding = 8.0f;
    s.TabRounding = 8.0f;
    s.ScrollbarRounding = 8.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowPadding = ImVec2(12, 12);
    s.FramePadding = ImVec2(9, 5);
    s.ItemSpacing = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(7, 5);
    s.ScrollbarSize = 11.0f;
    s.GrabMinSize = 11.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]           = V4(COL_BG);
    c[ImGuiCol_ChildBg]            = V4(COL_PANEL);
    c[ImGuiCol_PopupBg]            = V4(COL_RAISE);
    c[ImGuiCol_Border]             = V4(COL_LINE);
    c[ImGuiCol_FrameBg]            = V4(COL_RAISE);
    c[ImGuiCol_FrameBgHovered]     = V4(COL_RAISE2);
    c[ImGuiCol_FrameBgActive]      = V4(COL_RAISE2);
    c[ImGuiCol_Text]               = V4(COL_INK);
    c[ImGuiCol_TextDisabled]       = V4(COL_INK3);
    c[ImGuiCol_Button]             = V4(COL_RAISE);
    c[ImGuiCol_ButtonHovered]      = V4(COL_RAISE2);
    c[ImGuiCol_ButtonActive]       = V4(COL_BRASSDIM);
    c[ImGuiCol_Header]             = V4(COL_RAISE2);
    c[ImGuiCol_HeaderHovered]      = V4(COL_RAISE2);
    c[ImGuiCol_HeaderActive]       = V4(COL_BRASSDIM);
    c[ImGuiCol_CheckMark]          = V4(COL_BRASS2);
    c[ImGuiCol_SliderGrab]         = V4(COL_INK2);
    c[ImGuiCol_SliderGrabActive]   = V4(COL_BRASS);
    c[ImGuiCol_Tab]                = V4(COL_PANEL);
    c[ImGuiCol_TabHovered]         = V4(COL_RAISE2);
    c[ImGuiCol_TabActive]          = V4(COL_BG2);
    c[ImGuiCol_TabUnfocused]       = V4(COL_PANEL);
    c[ImGuiCol_TabUnfocusedActive] = V4(COL_BG2);
    c[ImGuiCol_TitleBg]            = V4(COL_BG);
    c[ImGuiCol_TitleBgActive]      = V4(COL_BG);
    c[ImGuiCol_ScrollbarBg]        = V4(COL_BG2);
    c[ImGuiCol_ScrollbarGrab]      = V4(COL_RAISE);
    c[ImGuiCol_ScrollbarGrabHovered] = V4(COL_RAISE2);
    c[ImGuiCol_Separator]          = V4(COL_LINE);
    c[ImGuiCol_NavHighlight]       = V4(COL_BRASSDIM);
}

// A small "reset to default" circular-arrow glyph, drawn with the draw list so it renders
// regardless of the loaded font (the default ImGui font has no such glyph). Used by the
// per-control hover reset affordance.
void DrawResetGlyph(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->PathClear();
    dl->PathArcTo(c, r, 0.6f, 6.05f, 20);  // ~open ring leaving a gap at the top-right
    dl->PathStroke(col, 0, 1.4f);
    // arrowhead at the ring's opening
    const float a = 0.6f;
    ImVec2 tip(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
    dl->AddTriangleFilled(ImVec2(tip.x + 2.6f, tip.y - 1.0f), ImVec2(tip.x - 2.0f, tip.y - 2.8f),
                          ImVec2(tip.x - 1.2f, tip.y + 2.6f), col);
}

// The Lumetri/DaVinci-style control row (cg-ui-polish core deliverable): a LINE with a
// draggable KNOB, a hover-reveal reset arrow, double-click-to-reset (on the track or the
// value), and a typed value box with legal-range validation. `v` is edited in place; returns
// true if it changed this frame. `bipolar` draws a centre origin tick and fills from it.
// `fmt` is a printf format for the value box (may carry a unit / sign, e.g. "%+.2f EV").
bool SliderRow(const char* label, float* v, float vmin, float vmax, float def,
               const char* fmt, bool bipolar) {
    ImGui::PushID(label);
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float labelW = 84.0f, valueW = 66.0f, resetW = 16.0f, pad = 8.0f;
    const float rowH = 22.0f;
    const float rowW = ImGui::GetContentRegionAvail().x;
    float trackW = rowW - labelW - valueW - resetW - pad * 3.0f;
    if (trackW < 32.0f) trackW = 32.0f;
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const float cy = c0.y + rowH * 0.5f;
    bool changed = false;

    // label (vertically centred, clipped to its column)
    dl->PushClipRect(ImVec2(c0.x, c0.y), ImVec2(c0.x + labelW - 4.0f, c0.y + rowH), true);
    dl->AddText(ImVec2(c0.x, cy - ImGui::GetTextLineHeight() * 0.5f), COL_INK2, label);
    dl->PopClipRect();

    // track drag region
    const float tx0 = c0.x + labelW + pad;
    ImGui::SetCursorScreenPos(ImVec2(tx0, c0.y));
    ImGui::InvisibleButton("##trk", ImVec2(trackW, rowH));
    const bool active = ImGui::IsItemActive();
    const bool trkHovered = ImGui::IsItemHovered();
    // A double-click resets to default, but the mouse is usually still held afterwards. Without
    // guarding, the held pointer re-applies the click position on the following frames and snaps
    // the value back (round-1 "reset flashes then snaps back"). Suppress drag-follow for the rest
    // of that activation via per-row state storage (public API; ImGui::ClearActiveID is internal).
    ImGuiStorage* rowState = ImGui::GetStateStorage();
    const ImGuiID suppressId = ImGui::GetID("##noDragAfterReset");
    bool suppressDrag = rowState->GetBool(suppressId, false);
    if (trkHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *v = def;
        changed = true;
        suppressDrag = true;
        rowState->SetBool(suppressId, true);
    }
    if (active && !suppressDrag) {
        float p = (io.MousePos.x - tx0) / trackW;
        p = p < 0 ? 0 : (p > 1 ? 1 : p);
        *v = vmin + p * (vmax - vmin);
        changed = true;
    }
    if (!active && suppressDrag) rowState->SetBool(suppressId, false);  // activation ended - re-enable drag

    // draw rail / fill / bipolar tick / knob
    const float span = (vmax - vmin) != 0.0f ? (vmax - vmin) : 1.0f;
    float p = (*v - vmin) / span;
    p = p < 0 ? 0 : (p > 1 ? 1 : p);
    const float origin = bipolar ? (0.0f - vmin) / span : 0.0f;
    dl->AddLine(ImVec2(tx0, cy), ImVec2(tx0 + trackW, cy), COL_LINE, 1.5f);
    const float fx0 = tx0 + std::min(origin, p) * trackW;
    const float fx1 = tx0 + std::max(origin, p) * trackW;
    dl->AddLine(ImVec2(fx0, cy), ImVec2(fx1, cy), active ? COL_BRASS : COL_BRASSDIM, 2.0f);
    if (bipolar)
        dl->AddLine(ImVec2(tx0 + origin * trackW, cy - 4.0f), ImVec2(tx0 + origin * trackW, cy + 4.0f),
                    COL_INK4, 1.0f);
    const float kx = tx0 + p * trackW;
    dl->AddCircleFilled(ImVec2(kx, cy), 6.5f, COL_PANEL);
    dl->AddCircle(ImVec2(kx, cy), 6.5f, active ? COL_BRASS : (trkHovered ? COL_INK : COL_INK2), 20, 1.8f);
    if (active) dl->AddCircle(ImVec2(kx, cy), 9.5f, WithAlpha(COL_BRASS, 60), 20, 2.0f);

    // hover-reveal reset arrow (double-click also resets; this is the discoverable affordance)
    const bool rowHovered = ImGui::IsMouseHoveringRect(c0, ImVec2(c0.x + rowW, c0.y + rowH));
    const float rstCx = tx0 + trackW + pad + resetW * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(rstCx - resetW * 0.5f, cy - resetW * 0.5f));
    ImGui::InvisibleButton("##rst", ImVec2(resetW, resetW));
    const bool rstHovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) { *v = def; changed = true; }
    if (rowHovered || rstHovered)
        DrawResetGlyph(dl, ImVec2(rstCx, cy), 5.5f, rstHovered ? COL_BRASS : WithAlpha(COL_INK2, 210));

    // typed value box with legal-range validation (accepts a number, clamps to [min,max])
    const float frameH = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(c0.x + rowW - valueW, c0.y + (rowH - frameH) * 0.5f));
    ImGui::SetNextItemWidth(valueW);
    float typed = *v;
    if (ImGui::InputFloat("##val", &typed, 0.0f, 0.0f, fmt,
                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
        *v = static_cast<float>(clampRange(typed, vmin, vmax));
        changed = true;
    }

    // advance to the next row (InputFloat left the cursor after its frame)
    ImGui::SetCursorScreenPos(ImVec2(c0.x, c0.y + rowH + 6.0f));
    ImGui::PopID();
    return changed;
}

// A full-width luminance slider with the value centred BENEATH it (captain refinement for the
// wheels' master control). Same track + knob + typed-entry behaviour as SliderRow, laid out
// vertically to sit under a colour disc.
bool WheelLumSlider(const char* id, float* v, float vmin, float vmax, float def,
                    const char* fmt, float width) {
    ImGui::PushID(id);
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = 16.0f;
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const float cy = c0.y + rowH * 0.5f;
    bool changed = false;

    ImGui::SetCursorScreenPos(c0);
    ImGui::InvisibleButton("##wtrk", ImVec2(width, rowH));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    // Suppress drag-follow for the rest of the activation after a double-click reset, so the held
    // mouse can't re-apply the click position and snap it back (public state storage; see SliderRow).
    ImGuiStorage* rowState = ImGui::GetStateStorage();
    const ImGuiID suppressId = ImGui::GetID("##noDragAfterReset");
    bool suppressDrag = rowState->GetBool(suppressId, false);
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *v = def;
        changed = true;
        suppressDrag = true;
        rowState->SetBool(suppressId, true);
    }
    if (active && !suppressDrag) {
        float p = (io.MousePos.x - c0.x) / width;
        p = p < 0 ? 0 : (p > 1 ? 1 : p);
        *v = vmin + p * (vmax - vmin);
        changed = true;
    }
    if (!active && suppressDrag) rowState->SetBool(suppressId, false);

    const float span = (vmax - vmin) != 0.0f ? (vmax - vmin) : 1.0f;
    float p = (*v - vmin) / span;
    p = p < 0 ? 0 : (p > 1 ? 1 : p);
    dl->AddLine(ImVec2(c0.x, cy), ImVec2(c0.x + width, cy), COL_LINE, 1.5f);
    dl->AddLine(ImVec2(c0.x, cy), ImVec2(c0.x + p * width, cy), active ? COL_BRASS : COL_BRASSDIM, 2.0f);
    const float kx = c0.x + p * width;
    dl->AddCircleFilled(ImVec2(kx, cy), 5.5f, COL_PANEL);
    dl->AddCircle(ImVec2(kx, cy), 5.5f, active ? COL_BRASS : (hovered ? COL_INK : COL_INK2), 18, 1.6f);

    // value centred beneath
    char buf[32];
    std::snprintf(buf, sizeof(buf), fmt, *v);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(c0.x + (width - ts.x) * 0.5f, c0.y + rowH + 1.0f), COL_INK2, buf);
    ImGui::SetCursorScreenPos(ImVec2(c0.x, c0.y + rowH + ImGui::GetTextLineHeight() + 4.0f));
    ImGui::PopID();
    return changed;
}

// --- viewer: toolbar + preview + scopes (Phase 4/5) -------------------------

// Compare-mode + scopes toolbar above the preview. Writes the window's UI-state atomics
// (compareMode, showScopes). The split position is now set by DIRECTLY dragging the divider
// in the preview (DrawPreviewPane), matching the mockup - no separate position slider.
void DrawViewerToolbar(WindowImpl* w) {
    int mode = w->compareMode.load();
    ImGui::TextUnformatted("View");
    ImGui::SameLine();
    if (ImGui::RadioButton("After", mode == 0)) mode = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Before", mode == 1)) mode = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("Split", mode == 2)) mode = 2;
    w->compareMode.store(mode);

    ImGui::SameLine();
    ImGui::Dummy(ImVec2(16, 0));
    ImGui::SameLine();
    bool scopes = w->showScopes.load();
    if (ImGui::Checkbox("Scopes", &scopes)) w->showScopes.store(scopes);
    if (mode == 2) {
        ImGui::SameLine();
        ImGui::TextDisabled("drag the divider to compare");
    }
}

// Draw a GpuTex letterboxed into [p0,p1], optionally clipped to a horizontal sub-range
// [clipX0, clipX1] (for split view). Returns the letterbox dst for divider placement.
FitRect DrawLetterboxed(ImDrawList* dl, const GpuTex& t, ImVec2 p0, ImVec2 p1,
                        float clipX0, float clipX1, bool clip) {
    FitRect fit = letterboxFit(p1.x - p0.x, p1.y - p0.y, t.w, t.h);
    ImVec2 imgMin(p0.x + fit.x, p0.y + fit.y);
    ImVec2 imgMax(imgMin.x + fit.w, imgMin.y + fit.h);
    if (clip) dl->PushClipRect(ImVec2(clipX0, p0.y), ImVec2(clipX1, p1.y), true);
    dl->AddImage(reinterpret_cast<ImTextureID>(t.srv), imgMin, imgMax);
    if (clip) dl->PopClipRect();
    return fit;
}

// The live preview pane, honouring the before/after/split compare mode.
void DrawPreviewPane(WindowImpl* w) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float scopesReserve = w->showScopes.load() ? 156.0f : 0.0f;
    float paneH = avail.y - scopesReserve;
    if (paneH < 60.0f) paneH = 60.0f;
    ImGui::Dummy(ImVec2(avail.x, paneH));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin();
    ImVec2 p1 = ImGui::GetItemRectMax();
    dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 28, 255));

    const int mode = w->compareMode.load();
    const bool haveAfter = w->previewTex.ready();
    const bool haveBefore = w->beforeTex.ready();

    if (!haveAfter && !haveBefore) {
        const char* label = "Live preview - waiting for frame (scrub the timeline)";
        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f),
                    IM_COL32(150, 150, 160, 255), label);
        return;
    }

    // Draw an overlay label centred in [x0,x1], but HIDDEN when that region collapses (divider
    // at its extreme) and CLAMPED inside the visible image [imgL,imgR] so it never floats off the
    // frame (round-1: labels left the image at the split extremes).
    auto tag = [&](const char* s, float x0, float x1, float y, float imgL, float imgR) {
        const float regionW = x1 - x0;
        ImVec2 ts = ImGui::CalcTextSize(s);
        if (regionW < ts.x + 10.0f) return;  // region collapsed - hide the label
        const float loL = std::max(x0 + 4.0f, imgL + 4.0f);
        const float hiL = std::min(x1 - ts.x - 4.0f, imgR - ts.x - 4.0f);
        if (hiL < loL) return;  // no room inside the visible image
        float tx = (x0 + x1 - ts.x) * 0.5f;
        tx = tx < loL ? loL : (tx > hiL ? hiL : tx);
        dl->AddText(ImVec2(tx, y + 4.0f), IM_COL32(210, 210, 220, 255), s);
    };

    if (mode == 1) {  // BeforeOnly (fall back to after if the before frame isn't in yet)
        const GpuTex& t = haveBefore ? w->beforeTex : w->previewTex;
        FitRect fr = DrawLetterboxed(dl, t, p0, p1, 0, 0, false);
        const float iL = p0.x + fr.x, iR = iL + fr.w;
        tag(haveBefore ? "Before (original)" : "Before (pending)", iL, iR, p0.y, iL, iR);
    } else if (mode == 2 && haveBefore && haveAfter) {  // Split
        // Both frames share the clip dimensions -> one letterbox rect; the divider splits it.
        SplitGeometry g = splitViewGeometry(p1.x - p0.x, p1.y - p0.y, w->previewTex.w,
                                            w->previewTex.h, w->splitFraction);
        const float dstX0 = p0.x + g.dst.x;
        const float dstX1 = p0.x + g.dst.x + g.dst.w;
        const float dstY0 = p0.y + g.dst.y;
        const float splitX = p0.x + g.splitX;
        DrawLetterboxed(dl, w->beforeTex, p0, p1, dstX0, splitX, true);   // before: left
        DrawLetterboxed(dl, w->previewTex, p0, p1, splitX, dstX1, true);  // after: right

        // Directly draggable divider (cg-ui-polish): a hit-strip over the split line updates
        // splitFraction, so the user drags the divider in the preview - not just a slider.
        const ImVec2 cursorSave = ImGui::GetCursorScreenPos();
        const float handleW = 24.0f;
        ImGui::SetCursorScreenPos(ImVec2(splitX - handleW * 0.5f, dstY0));
        ImGui::InvisibleButton("##splitdrag", ImVec2(handleW, g.dst.h));
        if (ImGui::IsItemActive() && g.dst.w > 1.0f) {
            float px = (ImGui::GetIO().MousePos.x - dstX0) / g.dst.w;
            w->splitFraction = px < 0.0f ? 0.0f : (px > 1.0f ? 1.0f : px);
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::SetCursorScreenPos(cursorSave);

        dl->AddLine(ImVec2(splitX, dstY0), ImVec2(splitX, dstY0 + g.dst.h),
                    IM_COL32(255, 255, 255, 210), 1.5f);
        // knob on the split line with left/right chevrons
        const float ky = dstY0 + g.dst.h * 0.5f;
        dl->AddCircleFilled(ImVec2(splitX, ky), 11.0f, IM_COL32(20, 17, 15, 190), 24);
        dl->AddCircle(ImVec2(splitX, ky), 11.0f, IM_COL32(255, 255, 255, 220), 24, 1.6f);
        dl->AddTriangleFilled(ImVec2(splitX - 5.0f, ky), ImVec2(splitX - 1.5f, ky - 3.5f),
                              ImVec2(splitX - 1.5f, ky + 3.5f), IM_COL32(255, 255, 255, 230));
        dl->AddTriangleFilled(ImVec2(splitX + 5.0f, ky), ImVec2(splitX + 1.5f, ky - 3.5f),
                              ImVec2(splitX + 1.5f, ky + 3.5f), IM_COL32(255, 255, 255, 230));
        tag("Before", dstX0, splitX, p0.y, dstX0, dstX1);
        tag("After", splitX, dstX1, p0.y, dstX0, dstX1);
    } else {  // AfterOnly (default), or Split before the before frame arrives
        DrawLetterboxed(dl, w->previewTex, p0, p1, 0, 0, false);
    }
}

// The scopes strip below the preview: waveform | histogram | vectorscope in three EQUAL
// columns that fill the strip's width at ANY window size. Each plot is a GPU texture drawn
// with AddImage (same GPU path as the preview) and hard-clipped to its box so it never
// bleeds outside. Waveform + histogram FILL their box interior (they're resolution-agnostic
// signal plots, so horizontal stretch is expected); the vectorscope keeps a 1:1 square,
// centered (letterboxFit). Sizes are recomputed from the live content region every frame,
// so windowed / fullscreen / live-resize all track correctly.
void DrawScopesStrip(WindowImpl* w) {
    ImGui::Spacing();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 30.0f || avail.y < 24.0f) return;  // too small to draw anything sensible
    const float gap = 8.0f;
    const float h = avail.y < 150.0f ? avail.y : 150.0f;  // fit even a short window
    const float colW = (avail.x - gap * 2.0f) / 3.0f;     // three equal columns
    if (colW < 20.0f) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const float labelH = ImGui::GetTextLineHeight() + 4.0f;
    const float pad = 4.0f;

    auto panel = [&](const GpuTex& t, const char* label, int idx, bool square) {
        const float x0 = base.x + static_cast<float>(idx) * (colW + gap);
        const ImVec2 a(x0, base.y);
        const ImVec2 b(x0 + colW, base.y + h);
        dl->AddRectFilled(a, b, IM_COL32(16, 16, 20, 255));
        dl->AddRect(a, b, IM_COL32(70, 70, 80, 255));
        // Interior below the label, inset by pad on every side.
        const float ix0 = a.x + pad;
        const float iy0 = a.y + labelH;
        const float iw = colW - pad * 2.0f;
        const float ih = (b.y - pad) - iy0;
        if (t.ready() && iw > 2.0f && ih > 2.0f) {
            dl->PushClipRect(a, b, true);  // hard clip: nothing draws outside the box
            if (square) {
                // Vectorscope: preserve 1:1, centered in the interior.
                FitRect fit = letterboxFit(iw, ih, t.w, t.h);
                ImVec2 mn(ix0 + fit.x, iy0 + fit.y);
                ImVec2 mx(mn.x + fit.w, mn.y + fit.h);
                dl->AddImage(reinterpret_cast<ImTextureID>(t.srv), mn, mx);
            } else {
                // Waveform / histogram: fill the interior.
                dl->AddImage(reinterpret_cast<ImTextureID>(t.srv), ImVec2(ix0, iy0),
                             ImVec2(ix0 + iw, iy0 + ih));
            }
            dl->PopClipRect();
        }
        dl->AddText(ImVec2(a.x + 4, a.y + 2), IM_COL32(180, 180, 190, 255), label);
    };
    panel(w->waveTex, "Waveform", 0, false);
    panel(w->histTex, "Histogram", 1, false);
    panel(w->vecTex, "Vectorscope", 2, true);
    ImGui::Dummy(ImVec2(avail.x, h));
}

// --- Curves widget (Phase 6b) -----------------------------------------------
//
// An interactive monotone-curve editor: a square plot with draggable control points.
// Points live in gamma-Rec.709 [0,1]x[0,1] (the recipe's CurveData); the drawn line is
// the SAME shape-preserving PCHIP the engine evaluates, so what the user sees is what
// bakes. Left-drag a point to move it; left-click empty space to add one (up to 16);
// right-click a point to remove it (endpoints excluded). Returns true if the state
// changed this frame (the caller pushes one coalesced Curves edit).

bool DrawCurveWidget(const char* id, cg::editor::CurveState& c, int& dragIdx, ImU32 lineColor,
                     float size) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();

    // background + grid
    const ImVec2 p1(p0.x + size, p0.y + size);
    dl->AddRectFilled(p0, p1, IM_COL32(18, 18, 22, 255));
    dl->AddRect(p0, p1, IM_COL32(70, 70, 80, 255));
    for (int i = 1; i < 4; ++i) {
        const float t = static_cast<float>(i) / 4.0f;
        dl->AddLine(ImVec2(p0.x + t * size, p0.y), ImVec2(p0.x + t * size, p1.y),
                    IM_COL32(40, 40, 48, 255));
        dl->AddLine(ImVec2(p0.x, p0.y + t * size), ImVec2(p1.x, p0.y + t * size),
                    IM_COL32(40, 40, 48, 255));
    }

    // Static identity reference (slope 1, corner to corner), dashed + subdued gray - Lumetri-style
    // so the neutral is visible at a glance behind the actual curve (round-1 refinement 5). Gray
    // is clearly distinct from the master (white) and R/G/B channel curve colors.
    {
        const ImVec2 a(p0.x, p1.y);  // (0,0) bottom-left
        const ImVec2 b(p1.x, p0.y);  // (1,1) top-right
        const int kDash = 22;        // even count -> on/off dashes
        for (int i = 0; i < kDash; i += 2) {
            const float t0 = static_cast<float>(i) / kDash;
            const float t1 = static_cast<float>(i + 1) / kDash;
            dl->AddLine(ImVec2(a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0),
                        ImVec2(a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1),
                        IM_COL32(92, 88, 82, 205), 1.0f);
        }
    }

    auto toScreen = [&](double nx, double ny) {
        return ImVec2(p0.x + static_cast<float>(nx) * size,
                      p0.y + static_cast<float>(1.0 - ny) * size);
    };
    auto toNorm = [&](ImVec2 s) {
        double nx = (s.x - p0.x) / size;
        double ny = 1.0 - (s.y - p0.y) / size;
        nx = nx < 0 ? 0 : (nx > 1 ? 1 : nx);
        ny = ny < 0 ? 0 : (ny > 1 ? 1 : ny);
        return ImVec2(static_cast<float>(nx), static_cast<float>(ny));
    };

    bool changed = false;

    // Find the point nearest the mouse (for hit-testing add/drag/remove).
    const float hitR = 9.0f;
    int nearest = -1;
    float nearestD2 = hitR * hitR;
    for (int i = 0; i < c.count; ++i) {
        ImVec2 sp = toScreen(c.x[i], c.y[i]);
        const float dx = sp.x - io.MousePos.x, dy = sp.y - io.MousePos.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= nearestD2) { nearestD2 = d2; nearest = i; }
    }

    // Right-click removes an interior point.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && nearest > 0 &&
        nearest < c.count - 1) {
        cg::editor::curveRemovePoint(c, nearest);
        changed = true;
        nearest = -1;
    }

    // Left press: start dragging the nearest point, or add a new one at the cursor.
    if (ImGui::IsItemActivated()) {
        cg::editor::curveEnsureEndpoints(c);
        if (nearest >= 0) {
            dragIdx = nearest;
        } else {
            ImVec2 n = toNorm(io.MousePos);
            dragIdx = cg::editor::curveInsertPoint(c, n.x, n.y);
            changed = true;
        }
    }

    // Drag: move the held point via the pure monotone clamp (y stays within [prev.y, next.y],
    // interior x between neighbors, endpoints x-fixed) so the drawn PCHIP always passes
    // through the dots - no detach even on a far drag.
    if (active && dragIdx >= 0 && dragIdx < c.count) {
        ImVec2 n = toNorm(io.MousePos);
        cg::editor::curveClampPoint(c, dragIdx, n.x, n.y);
        changed = true;
    }
    if (!active) dragIdx = -1;

    // Draw the curve as the actual PCHIP the engine will bake (identity if <2 points).
    const int kSeg = 48;
    ImVec2 prev;
    if (c.count >= 2) {
        std::vector<double> xs(c.x, c.x + c.count), ys(c.y, c.y + c.count);
        cg::core::MonotoneCurve mc = cg::core::MonotoneCurve::make(xs, ys, /*forceMonotoneY=*/true);
        for (int i = 0; i <= kSeg; ++i) {
            const double xx = static_cast<double>(i) / kSeg;
            double yy = mc(xx);
            yy = yy < 0 ? 0 : (yy > 1 ? 1 : yy);
            ImVec2 sp = toScreen(xx, yy);
            if (i > 0) dl->AddLine(prev, sp, lineColor, 2.0f);
            prev = sp;
        }
    } else {
        dl->AddLine(toScreen(0, 0), toScreen(1, 1), lineColor, 2.0f);
    }

    // Draw the control points.
    for (int i = 0; i < c.count; ++i) {
        ImVec2 sp = toScreen(c.x[i], c.y[i]);
        const bool hot = (i == nearest) || (i == dragIdx);
        dl->AddCircleFilled(sp, hot ? 5.5f : 4.0f, hot ? IM_COL32(255, 255, 255, 255)
                                                        : IM_COL32(210, 210, 220, 255));
        dl->AddCircle(sp, hot ? 5.5f : 4.0f, IM_COL32(20, 20, 24, 255));
    }
    return changed;
}

// --- Wheels widget (Phase 6c) -----------------------------------------------
//
// A color disc + draggable dot + master slider per wheel. Three unit-vector "primary"
// directions 120deg apart (R top, G lower-left, B lower-right) form a tight frame, so a
// disc position maps LINEARLY to a zero-sum per-channel color offset and inverts exactly
// (needed to place the dot from a stored triple). The master carries the luminance /
// uniform component; the disc carries color balance. Used for BOTH the DaVinci LGG wheels
// (offset applied to lift/gamma/gain) and, for the 3-way secondary mode, the band tints.

struct DiscDir { float x, y; };
inline const DiscDir* WheelPrimaries() {
    // R at 90deg, G at 210deg, B at 330deg (unit vectors), y up.
    static const DiscDir d[3] = {
        {0.0f, 1.0f},
        {-0.8660254f, -0.5f},
        {0.8660254f, -0.5f},
    };
    return d;
}

// Map a disc position v=(vx,vy) in the unit circle to a zero-sum per-channel offset.
inline void DiscToOffset(float vx, float vy, double k, double out[3]) {
    const DiscDir* d = WheelPrimaries();
    for (int c = 0; c < 3; ++c) out[c] = k * (vx * d[c].x + vy * d[c].y);
}
// Invert: recover the disc position from a per-channel offset (tight-frame pseudo-inverse:
// v = (2/3) * sum_c (offset_c/k) * dir_c). Robust for any offset (uses only its color part).
inline void OffsetToDisc(const double off[3], double k, float& vx, float& vy) {
    const DiscDir* d = WheelPrimaries();
    double sx = 0, sy = 0;
    for (int c = 0; c < 3; ++c) {
        sx += (off[c] / k) * d[c].x;
        sy += (off[c] / k) * d[c].y;
    }
    vx = static_cast<float>((2.0 / 3.0) * sx);
    vy = static_cast<float>((2.0 / 3.0) * sy);
    const float r = std::sqrt(vx * vx + vy * vy);
    if (r > 1.0f) { vx /= r; vy /= r; }
}

// Draw a hue/color reference as a fan of solid wedges filling the disc, so a push direction
// reads as a color choice (the round-1 UX ask - wheels had no color). `colorFn(angle)` gives
// the wedge color at a screen angle (radians, CCW from +x, y up). Semi-transparent so it
// tints the dark disc face beneath.
template <typename ColorFn>
void DrawColorRing(ImDrawList* dl, ImVec2 center, float radius, ColorFn colorFn) {
    const int N = 48;
    const float twoPi = 6.2831853f;
    for (int i = 0; i < N; ++i) {
        const float a0 = static_cast<float>(i) / N * twoPi;
        const float a1 = static_cast<float>(i + 1) / N * twoPi;
        const float am = 0.5f * (a0 + a1);
        const ImVec2 p0(center.x + std::cos(a0) * radius, center.y - std::sin(a0) * radius);
        const ImVec2 p1(center.x + std::cos(a1) * radius, center.y - std::sin(a1) * radius);
        dl->AddTriangleFilled(center, p0, p1, colorFn(am));
    }
}

// Wedge color for the LGG color-balance disc: hue from the disc's primary layout (R at top /
// 90deg, G at 210deg, B at 330deg), so the ring matches DiscToOffset's push direction.
inline ImU32 LggRingColor(float angle) {
    const float hueDeg = std::fmod(angle * 57.29578f - 90.0f + 360.0f, 360.0f);
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hueDeg / 360.0f, 0.80f, 0.92f, r, g, b);
    return IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255),
                    130);
}

// Wedge color for the 3-way LAB-tint disc: convert the disc direction to a small LAB tint
// (a = cos, b = sin) and through the core LAB->Rec.709 so the ring shows the ACTUAL tint hue.
inline ImU32 TintRingColor(float angle) {
    const double chroma = 42.0;
    const cg::core::Vec3d lin = cg::core::labToLinearRec709(
        {70.0, std::cos(angle) * chroma, std::sin(angle) * chroma});
    auto enc = [](double v) {
        double e = cg::core::rec709Encode(v < 0 ? 0 : v);
        e = e < 0 ? 0 : (e > 1 ? 1 : e);
        return static_cast<int>(e * 255);
    };
    return IM_COL32(enc(lin[0]), enc(lin[1]), enc(lin[2]), 150);
}

// Draw a color-balance disc. `triple` is a per-channel value about its mean (the master
// luminance). `k` scales the disc's color swing. On drag, the disc's zero-sum color offset
// is combined with the CURRENT master so luminance set by the slider is preserved. Returns
// true if `triple` changed. `radius` is the disc radius in px.
bool DrawWheelDisc(const char* id, double triple[3], double k, float radius) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float sz = radius * 2.0f;
    ImGui::InvisibleButton(id, ImVec2(sz, sz));
    const bool active = ImGui::IsItemActive();
    const ImVec2 center(p0.x + radius, p0.y + radius);

    // disc face + hue color ring + crosshair
    dl->AddCircleFilled(center, radius, IM_COL32(30, 30, 36, 255), 48);
    DrawColorRing(dl, center, radius, LggRingColor);
    dl->AddCircle(center, radius, IM_COL32(90, 90, 100, 255), 48, 1.5f);
    dl->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y),
                IM_COL32(55, 55, 62, 120));
    dl->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius),
                IM_COL32(55, 55, 62, 120));

    const double master = (triple[0] + triple[1] + triple[2]) / 3.0;
    double off[3] = {triple[0] - master, triple[1] - master, triple[2] - master};

    bool changed = false;
    if (active) {
        float vx = (io.MousePos.x - center.x) / radius;
        float vy = -(io.MousePos.y - center.y) / radius;  // y up
        const float r = std::sqrt(vx * vx + vy * vy);
        if (r > 1.0f) { vx /= r; vy /= r; }
        DiscToOffset(vx, vy, k, off);
        for (int c = 0; c < 3; ++c) triple[c] = master + off[c];
        changed = true;
    }

    // draw the dot at the current color offset
    float vx, vy;
    OffsetToDisc(off, k, vx, vy);
    const ImVec2 dot(center.x + vx * radius, center.y - vy * radius);
    dl->AddCircleFilled(dot, 5.0f, IM_COL32(255, 255, 255, 255));
    dl->AddCircle(dot, 5.0f, IM_COL32(20, 20, 24, 255));
    return changed;
}

// Set the uniform (luminance) component of a triple to `master`, preserving the per-channel
// color offset the disc set. Used by the LGG master sliders and the 3-way luminance sliders.
inline void WheelSetMaster(double triple[3], double master) {
    const double cur = (triple[0] + triple[1] + triple[2]) / 3.0;
    for (int c = 0; c < 3; ++c) triple[c] = master + (triple[c] - cur);
}
inline double WheelMaster(const double triple[3]) {
    return (triple[0] + triple[1] + triple[2]) / 3.0;
}

// Draw a LAB-tint disc for the Adobe 3-way secondary mode: the disc position maps directly
// to a band's LAB [a,b] offset (x = a green<->magenta, y = b blue<->amber), `scale` LAB
// units at the disc edge. Returns true if `ab` changed. Distinct from DrawWheelDisc (which
// carries a per-channel luminance-preserving color offset); a band tint is a plain 2D value.
bool DrawTintDisc(const char* id, double ab[2], double scale, float radius) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float sz = radius * 2.0f;
    ImGui::InvisibleButton(id, ImVec2(sz, sz));
    const bool active = ImGui::IsItemActive();
    const ImVec2 center(p0.x + radius, p0.y + radius);
    dl->AddCircleFilled(center, radius, IM_COL32(30, 30, 36, 255), 48);
    DrawColorRing(dl, center, radius, TintRingColor);
    dl->AddCircle(center, radius, IM_COL32(90, 90, 100, 255), 48, 1.5f);
    dl->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y),
                IM_COL32(55, 55, 62, 120));
    dl->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius),
                IM_COL32(55, 55, 62, 120));
    bool changed = false;
    if (active) {
        float vx = (io.MousePos.x - center.x) / radius;
        float vy = -(io.MousePos.y - center.y) / radius;  // y up
        const float r = std::sqrt(vx * vx + vy * vy);
        if (r > 1.0f) { vx /= r; vy /= r; }
        ab[0] = static_cast<double>(vx) * scale;
        ab[1] = static_cast<double>(vy) * scale;
        changed = true;
    }
    float vx = static_cast<float>(ab[0] / scale);
    float vy = static_cast<float>(ab[1] / scale);
    const ImVec2 dot(center.x + vx * radius, center.y - vy * radius);
    dl->AddCircleFilled(dot, 5.0f, IM_COL32(255, 255, 255, 255));
    dl->AddCircle(dot, 5.0f, IM_COL32(20, 20, 24, 255));
    return changed;
}

// --- the per-window UI content (the editor's controls) ----------------------

// "Reference Match" (Phase 7) bakes a Theme from a reference-stats sidecar file (no
// image codec in native/ - see ColorGrade.cpp's LoadReferenceStats / ThemeFromPopup and
// data/cg-agents-study/report.md sec 1d); falls back to identity (None/Manual) with no
// sidecar loaded. The polished drop-zone/picker ships later with the UI overhaul.
// Order MUST match the CG_THEME_* popup enum in ColorGrade.h (1-based). None
// (Manual) leads (captain decision [key=none-first]); Reference Match then the
// PR #36 library follow.
const char* const kThemeNames[] = {
    "None (Manual)", "Teal-Orange", "Warm-Film", "Cool-Noir", "Reference Match",
    "Golden Hour", "Bleach Bypass", "Vintage Fade", "High-Key Clean", "Low-Key Moody",
    "Winter Blue", "Warm Portrait", "Pastel Dream", "Neon Cyberpunk", "Day for Night",
    "Autumn", "Summer Blockbuster", "Muted Teal-Orange", "Monochrome B&W", "Sepia",
    "Cinematic Green", "Desaturated Doc", "Punchy Social", "Cross Process", "Rose Romance"};
constexpr int kThemeCount = 25;
constexpr int kThemeReferenceIndex = 5;  // 1-based CG_THEME_REFERENCE (kThemeNames[4])

// Registry keys parallel to kThemeNames (1-based popup index -> THEMES key), so the agent
// bridge can seed auto-grade against the same look the popup selects (matches each
// core/Themes.h builder's t.name). Index 5 (Reference Match) and 1 (None/Manual) have no
// stat-match look, so the auto-grade base falls back to a self stats-only target (empty key).
const char* const kThemeKeys[] = {
    "none-manual", "teal-orange", "warm-film", "cool-noir", "",  // "" = Reference Match
    "golden-hour", "bleach-bypass", "vintage-fade", "high-key-clean", "low-key-moody",
    "winter-blue", "warm-portrait", "pastel-dream", "neon-cyberpunk", "day-for-night",
    "autumn", "summer-blockbuster", "muted-teal-orange", "monochrome-bw", "sepia",
    "cinematic-green", "desaturated-doc", "punchy-social", "cross-process", "rose-romance"};

// The auto-grade base theme key for a 1-based popup index (empty for None/Reference).
inline std::string themeKeyForPopup(int oneBased) {
    if (oneBased < 1 || oneBased > kThemeCount) return "";
    if (oneBased == 1 || oneBased == kThemeReferenceIndex) return "";  // no stat-match look
    return kThemeKeys[oneBased - 1];
}
// LUT-Source choices are data-driven from this array (count = size), so the in-flight
// External-cube+Correct/Basics mode (fm/cg-lut-correct-stack, PR #39) can extend/relabel
// this list on rebase with no layout change. External mode reads a .cube next to the .aex
// (or env CG_LUT_PATH); the note under the control documents it.
const char* const kLutSourceNames[] = {"Auto (Theme + Analysis)", "Embedded (Teal-Orange)",
                                       "External .cube file",
                                       "External .cube + Correct/Basics"};
constexpr int kLutSourceCount = static_cast<int>(sizeof(kLutSourceNames) / sizeof(kLutSourceNames[0]));

// A small uppercase "eyebrow" section label with a trailing hairline (mockup .group>.eyebrow).
void Eyebrow(const char* text) {
    ImGui::Dummy(ImVec2(0, 2));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c = ImGui::GetCursorScreenPos();
    dl->AddText(c, COL_INK3, text);
    const float tw = ImGui::CalcTextSize(text).x;
    const float availW = ImGui::GetContentRegionAvail().x;
    const float ly = c.y + ImGui::GetTextLineHeight() * 0.5f;
    dl->AddLine(ImVec2(c.x + tw + 9.0f, ly), ImVec2(c.x + availW, ly), COL_LINESOFT, 1.0f);
    ImGui::Dummy(ImVec2(availW, ImGui::GetTextLineHeight() + 6.0f));
}

// A soft "note" box (mockup .note) for the explanatory captions under each control group.
void NoteBox(const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float wrap = ImGui::GetContentRegionAvail().x;
    const ImVec2 c = ImGui::GetCursorScreenPos();
    const float pad = 8.0f;
    const ImVec2 sz = ImGui::CalcTextSize(text, nullptr, false, wrap - pad * 2.0f);
    const ImVec2 boxMax(c.x + wrap, c.y + sz.y + pad * 2.0f);
    dl->AddRectFilled(c, boxMax, COL_HAIR, 7.0f);
    dl->AddRect(c, boxMax, COL_LINESOFT, 7.0f);
    dl->AddText(nullptr, 0.0f, ImVec2(c.x + pad, c.y + pad), COL_INK3, text, nullptr, wrap - pad * 2.0f);
    ImGui::Dummy(ImVec2(wrap, sz.y + pad * 2.0f + 4.0f));
}

// --- inspector tabs (the Correct / Basics / Grade / Curves / Wheels control set) --------

// Forward declarations: the agent-execution helpers are defined below (just before the agent
// dock), but the Correct tab's reference picker above them calls these three. C++ needs the
// declaration visible at the call site (cg-agent-wiring; fixes the Win C3861 build errors).
std::string AgentTempPath(const std::string& leaf);
std::vector<std::string> PickFiles(WindowImpl* w, bool multi);
void LaunchReferenceAgent(WindowImpl* w, const ParamSnapshot& ui, const std::string& imagePath,
                          const std::string& sidecarPath);

void DrawCorrectTab(WindowImpl* w, ParamSnapshot& ui) {
    Eyebrow("FOOTAGE");
    // Camera -> Profile cascade (both combos derive from core/FootageCatalog.h and resolve to
    // the flat 1-based popup index the effect stores). A LOG profile pins its camera; Standard
    // (flat 1) is camera-AMBIGUOUS and always resolves to the first camera, so we remember the
    // user's chosen camera in w->footageCamera to keep the Camera combo from snapping back to
    // ARRI when its profile is Standard (round-1 bug: "only ARRI selectable").
    cg::core::FootageCascadePos pos = cg::core::footageCascadePosForFlat(ui.footageProfile);
    std::vector<std::string> cams = cg::core::footageCameras();
    std::vector<const char*> camPtrs;
    camPtrs.reserve(cams.size());
    for (const auto& c : cams) camPtrs.push_back(c.c_str());
    // A log profile identifies its camera unambiguously - adopt it; Standard keeps the
    // remembered camera (seeded from the flat resolution on first draw).
    if (cg::core::footageIndexIsLog(ui.footageProfile)) w->footageCamera = pos.cameraIndex;
    else if (w->footageCamera < 0) w->footageCamera = pos.cameraIndex;
    int camIdx = w->footageCamera;
    if (camIdx < 0 || camIdx >= static_cast<int>(cams.size())) camIdx = 0;
    ImGui::TextColored(V4(COL_INK2), "Camera");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##camera", &camIdx, camPtrs.data(), static_cast<int>(camPtrs.size()))) {
        w->footageCamera = camIdx;  // remember the choice so a Standard profile can't reset it
        cg::core::FootageCameraProfiles p = cg::core::footageProfilesForCamera(cams[camIdx]);
        ui.footageProfile = p.flatIndices.front();  // auto-select that camera's Standard
        w->edits.push({EditField::FootageProfile, static_cast<double>(ui.footageProfile)});
    }
    cg::core::FootageCameraProfiles profs = cg::core::footageProfilesForCamera(cams[camIdx]);
    std::vector<const char*> profPtrs;
    profPtrs.reserve(profs.labels.size());
    for (const auto& l : profs.labels) profPtrs.push_back(l.c_str());
    int optIdx = 0;
    for (size_t i = 0; i < profs.flatIndices.size(); ++i)
        if (profs.flatIndices[i] == ui.footageProfile) { optIdx = static_cast<int>(i); break; }
    ImGui::TextColored(V4(COL_INK2), "Log / gamma profile");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##profile", &optIdx, profPtrs.data(), static_cast<int>(profPtrs.size()))) {
        ui.footageProfile = profs.flatIndices[static_cast<size_t>(optIdx)];
        w->footageCamera = camIdx;  // keep the camera fixed even when selecting Standard
        w->edits.push({EditField::FootageProfile, static_cast<double>(ui.footageProfile)});
    }
    NoteBox("Decode-then-grade. The profile decodes the clip before any grade, so V-Log is "
            "never handled as raw log. Analysis re-runs on change.");

    ImGui::Dummy(ImVec2(0, 4));
    if (ImGui::Button("Analyze clip")) w->analyzeRequested.store(true);
    ImGui::SameLine();
    AnalysisStatus st = w->readStatus();
    if (st.state == AnalysisStatus::State::Sampling)
        ImGui::TextColored(V4(COL_BRASS2), "Analyzing %d/%d...", st.sampled, st.total);
    else if (st.state == AnalysisStatus::State::Analyzed)
        ImGui::TextColored(V4(COL_OK), st.fromCache ? "Analyzed (cached)" : "Analyzed");
    else
        ImGui::TextDisabled("Not analyzed");

    Eyebrow("REFERENCE MATCH");
    NoteBox("Match this clip to a still. Pick an image and it is measured for you (TIFF/PNG, on "
            "the TS side) into the sidecar the effect reads - no file to hand-produce. Pure stat "
            "transfer, no model call.");
    ImGui::BeginDisabled(w->agentBusy.load());
    if (ImGui::Button("Pick reference image...", ImVec2(-FLT_MIN, 0))) {
        std::vector<std::string> picked = PickFiles(w, /*multi=*/false);
        if (!picked.empty()) {
            w->agentApplied = false;
            w->agentOpen = true;   // surface the result/progress in the agent dock
            const std::string sidecar = AgentTempPath("ColorGrade_Reference.stats");
            LaunchReferenceAgent(w, ui, picked[0], sidecar);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::Button("Use existing sidecar", ImVec2(-FLT_MIN, 0))) {
        // Power-user path: env CG_REF_STATS_PATH or ColorGrade_Reference.stats next to the .aex.
        ui.theme = kThemeReferenceIndex;
        w->edits.push({EditField::Theme, static_cast<double>(ui.theme)});
    }
    NoteBox("Sets Theme -> Reference and rides your Basics / Curves / Wheels edits on top. Override: "
            "env CG_REF_STATS_PATH, or ColorGrade_Reference.stats next to the plug-in.");
}

void DrawBasicsTab(WindowImpl* w, ParamSnapshot& ui) {
    ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK3));
    ImGui::TextWrapped("Manual primary correction, applied before the theme look. Exposure, "
                       "Look Mix and Temperature are keyframeable in Effect Controls. Uses LUT "
                       "Source = Auto.");
    ImGui::PopStyleColor();
    auto pushManual = [w](ParamSnapshot& s) {
        ParamEdit e;
        e.field = EditField::Manual;
        e.manual = s.manual;
        w->edits.push(e);
    };

    Eyebrow("TONE");
    float exposure = static_cast<float>(ui.exposure);
    if (SliderRow("Exposure", &exposure, -5.0f, 5.0f, 0.0f, "%+.2f EV", true)) {
        ui.exposure = exposure;
        w->edits.push({EditField::Exposure, ui.exposure});
    }
    float contrast = static_cast<float>(ui.manual.contrast);
    if (SliderRow("Contrast", &contrast, -100.0f, 100.0f, 0.0f, "%+.0f", true)) {
        ui.manual.contrast = contrast; pushManual(ui);
    }
    float pivot = static_cast<float>(ui.manual.pivot);
    if (SliderRow("Pivot", &pivot, 0.05f, 0.95f, 0.435f, "%.3f", false)) {
        ui.manual.pivot = pivot; pushManual(ui);
    }
    float highlights = static_cast<float>(ui.manual.highlights);
    if (SliderRow("Highlights", &highlights, -100.0f, 100.0f, 0.0f, "%+.0f", true)) {
        ui.manual.highlights = highlights; pushManual(ui);
    }
    float shadows = static_cast<float>(ui.manual.shadows);
    if (SliderRow("Shadows", &shadows, -100.0f, 100.0f, 0.0f, "%+.0f", true)) {
        ui.manual.shadows = shadows; pushManual(ui);
    }
    float whites = static_cast<float>(ui.manual.whites);
    if (SliderRow("Whites", &whites, -100.0f, 100.0f, 0.0f, "%+.0f", true)) {
        ui.manual.whites = whites; pushManual(ui);
    }
    float blacks = static_cast<float>(ui.manual.blacks);
    if (SliderRow("Blacks", &blacks, -100.0f, 100.0f, 0.0f, "%+.0f", true)) {
        ui.manual.blacks = blacks; pushManual(ui);
    }

    Eyebrow("COLOR");
    float temperature = static_cast<float>(ui.temperature);
    if (SliderRow("Temperature", &temperature, -100.0f, 100.0f, 0.0f, "%+.0f", true)) {
        ui.temperature = temperature;
        w->edits.push({EditField::Temperature, ui.temperature});
    }
    float tint = static_cast<float>(ui.manual.tint);
    if (SliderRow("Tint", &tint, -100.0f, 100.0f, 0.0f, "%+.0f", true)) {
        ui.manual.tint = tint; pushManual(ui);
    }
    float satPct = static_cast<float>(ui.manual.saturation * 100.0);
    if (SliderRow("Saturation", &satPct, 0.0f, 200.0f, 100.0f, "%.0f%%", false)) {
        ui.manual.saturation = satPct / 100.0; pushManual(ui);
    }
    float vibrance = static_cast<float>(ui.manual.vibrance);
    if (SliderRow("Vibrance", &vibrance, -100.0f, 100.0f, 0.0f, "%+.0f", true)) {
        ui.manual.vibrance = vibrance; pushManual(ui);
    }

    Eyebrow("BLEND");
    float lookMixPct = static_cast<float>(ui.lookMix * 100.0);
    if (SliderRow("Look Mix", &lookMixPct, 0.0f, 100.0f, 100.0f, "%.0f%%", false)) {
        ui.lookMix = clamp01(lookMixPct / 100.0);
        w->edits.push({EditField::LookMix, ui.lookMix});
    }
}

void DrawGradeTab(WindowImpl* w, ParamSnapshot& ui) {
    Eyebrow("LOOK");
    int themeIdx = ui.theme - 1;
    if (themeIdx < 0) themeIdx = 0;
    if (themeIdx > kThemeCount - 1) themeIdx = kThemeCount - 1;
    ImGui::TextColored(V4(COL_INK2), "Theme");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##theme", &themeIdx, kThemeNames, kThemeCount)) {
        ui.theme = themeIdx + 1;
        w->edits.push({EditField::Theme, static_cast<double>(ui.theme)});
    }
    ImGui::Dummy(ImVec2(0, 4));

    float strengthPct = static_cast<float>(fractionToPercent(ui.strength));
    if (SliderRow("Strength", &strengthPct, 0.0f, 100.0f, 100.0f, "%.0f%%", false)) {
        ui.strength = clamp01(percentToFraction(strengthPct));
        w->edits.push({EditField::Strength, ui.strength});
    }
    float skinPct = static_cast<float>(fractionToPercent(ui.skinProtection));
    if (SliderRow("Skin protect", &skinPct, 0.0f, 100.0f, 65.0f, "%.0f%%", false)) {
        ui.skinProtection = clamp01(percentToFraction(skinPct));
        w->edits.push({EditField::SkinProtection, ui.skinProtection});
    }
    float chromaPct = static_cast<float>(fractionToPercent(ui.chromaGain));
    if (SliderRow("Chroma gain", &chromaPct, 0.0f, 200.0f, 100.0f, "%.0f%%", false)) {
        ui.chromaGain = clampChromaFraction(percentToFraction(chromaPct));
        w->edits.push({EditField::ChromaGain, ui.chromaGain});
    }

    Eyebrow("OUTPUT");
    int srcIdx = ui.lutSource - 1;
    if (srcIdx < 0) srcIdx = 0;
    if (srcIdx > kLutSourceCount - 1) srcIdx = kLutSourceCount - 1;
    ImGui::TextColored(V4(COL_INK2), "LUT source");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##lutsource", &srcIdx, kLutSourceNames, kLutSourceCount)) {
        ui.lutSource = srcIdx + 1;
        w->edits.push({EditField::LutSource, static_cast<double>(ui.lutSource)});
    }
    if (ui.theme == 1)
        NoteBox("None (Manual). Resting state - exact identity, no imposed look. Build the grade "
                "by hand in Basics / Curves / Wheels, or pick a theme.");
    else
        NoteBox("Stat-matched look baked in-effect. Strength dilutes it toward your manual base; "
                "Skin protect guards memory tones. External .cube uses a file next to the plug-in.");
}

void DrawCurvesTab(WindowImpl* w, ParamSnapshot& ui) {
    Eyebrow("TONE CURVES");
    // Channel selector (Master / R / G / B) - one big plot for the active channel, so per-slot
    // dirty tracking is natural: editing a channel marks only that slot user-owned.
    struct Ch { const char* label; cg::editor::CurveState* c; int idx; ImU32 col; ImU32 tab; };
    const Ch chans[4] = {
        {"Master", &ui.curves.master, 0, IM_COL32(0xe8, 0xe0, 0xd6, 255), IM_COL32(0x4b, 0x46, 0x3f, 255)},
        {"R",      &ui.curves.r,      1, IM_COL32(0xe0, 0x60, 0x60, 255), IM_COL32(0x7d, 0x3a, 0x35, 255)},
        {"G",      &ui.curves.g,      2, IM_COL32(0x60, 0xc0, 0x70, 255), IM_COL32(0x38, 0x66, 0x3d, 255)},
        {"B",      &ui.curves.b,      3, IM_COL32(0x70, 0x98, 0xe8, 255), IM_COL32(0x35, 0x50, 0x7d, 255)},
    };
    const float availW = ImGui::GetContentRegionAvail().x;
    const float btnW = (availW - 3.0f * 5.0f) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        const bool on = w->curveChannel == i;
        if (on) { ImGui::PushStyleColor(ImGuiCol_Button, V4(chans[i].tab));
                  ImGui::PushStyleColor(ImGuiCol_Text, V4(IM_COL32(255, 255, 255, 255))); }
        if (ImGui::Button(chans[i].label, ImVec2(btnW, 26))) w->curveChannel = i;
        if (on) ImGui::PopStyleColor(2);
        if (i < 3) ImGui::SameLine(0, 5);
    }
    ImGui::Dummy(ImVec2(0, 4));

    const int ch = w->curveChannel;
    const float plot = ImGui::GetContentRegionAvail().x;  // square, full width
    cg::editor::CurveState* cur = chans[ch].c;
    if (DrawCurveWidget("##curveplot", *cur, w->curveDrag[ch], chans[ch].col,
                        plot < 260.0f ? plot : 260.0f)) {
        cg::editor::curveMarkDirty(*cur);  // this slot is now user-owned (persist it)
        ParamEdit e; e.field = EditField::Curves; e.curves = ui.curves;
        w->edits.push(e);
    }
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(V4(COL_INK3), "Left-drag move  -  click add  -  right-click remove");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 92.0f);
    if (ImGui::Button("Reset curve", ImVec2(92, 0))) {
        *cur = cg::editor::CurveState{};  // absent + not dirty -> this channel follows the theme
        w->curveDrag[ch] = -1;
        ParamEdit e; e.field = EditField::Curves; e.curves = ui.curves;
        w->edits.push(e);
    }
    NoteBox("The drawn line is the exact PCHIP the engine bakes. Editing a channel keeps only "
            "that channel as your own - unedited channels keep tracking the theme when you switch "
            "looks. Uses LUT Source = Auto.");
}

void DrawWheelsTab(WindowImpl* w, ParamSnapshot& ui) {
    Eyebrow("COLOR WHEELS");
    bool wheelsChanged = false;
    int mode = ui.wheels.mode;
    if (ImGui::RadioButton("Lift / Gamma / Gain", mode == 0)) mode = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("3-Way", mode == 1)) mode = 1;
    if (mode != ui.wheels.mode) { ui.wheels.mode = mode; wheelsChanged = true; }
    ImGui::Dummy(ImVec2(0, 4));

    const float availW = ImGui::GetContentRegionAvail().x;
    const float gap = 10.0f;
    const float colW = (availW - gap * 2.0f) / 3.0f;
    const float radius = std::min(46.0f, colW * 0.5f - 4.0f);

    if (mode == 0) {
        struct LggWheel { const char* name; double* triple; double neutral; double colorK;
                          float mMin, mMax; const char* fmt; };
        const LggWheel wheels[3] = {
            {"Lift", ui.wheels.lift, 0.0, 0.20, -0.5f, 0.5f, "%.3f"},
            {"Gamma", ui.wheels.gamma, 1.0, 0.60, 0.2f, 3.0f, "%.2f"},
            {"Gain", ui.wheels.gain, 1.0, 0.40, 0.0f, 2.0f, "%.2f"},
        };
        for (int i = 0; i < 3; ++i) {
            const LggWheel& wl = wheels[i];
            ImGui::BeginGroup();
            const float nmW = ImGui::CalcTextSize(wl.name).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (colW - nmW) * 0.5f);
            ImGui::TextColored(V4(COL_INK2), "%s", wl.name);
            const float discX = ImGui::GetCursorPosX() + (colW - radius * 2.0f) * 0.5f;
            ImGui::SetCursorPosX(discX);
            std::string discId = std::string("##lgg") + std::to_string(i);
            if (DrawWheelDisc(discId.c_str(), wl.triple, wl.colorK, radius)) wheelsChanged = true;
            float m = static_cast<float>(WheelMaster(wl.triple));
            std::string mId = std::string("##lggm") + std::to_string(i);
            if (WheelLumSlider(mId.c_str(), &m, wl.mMin, wl.mMax, static_cast<float>(wl.neutral),
                               wl.fmt, colW)) {
                WheelSetMaster(wl.triple, m); wheelsChanged = true;
            }
            std::string rId = std::string("Reset##lgg") + std::to_string(i);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (colW - 54.0f) * 0.5f);
            if (ImGui::SmallButton(rId.c_str())) {
                wl.triple[0] = wl.triple[1] = wl.triple[2] = wl.neutral; wheelsChanged = true;
            }
            ImGui::EndGroup();
            if (i < 2) ImGui::SameLine(0, gap);
        }
        NoteBox("A full-saturation hue ring; each disc places a printer-lights colour offset, the "
                "slider carries luminance. Uses LUT Source = Auto.");
    } else {
        struct ThreeWay { const char* name; double* ab; bool* has; double* triple; double neutral;
                          float mMin, mMax; const char* fmt; };
        ThreeWay bands[3] = {
            {"Shadows", ui.wheels.shadowTint, &ui.wheels.hasShadowTint, ui.wheels.lift, 0.0,
             -0.5f, 0.5f, "%.3f"},
            {"Midtones", ui.wheels.midTint, &ui.wheels.hasMidTint, ui.wheels.gamma, 1.0,
             0.2f, 3.0f, "%.2f"},
            {"Highlights", ui.wheels.highTint, &ui.wheels.hasHighTint, ui.wheels.gain, 1.0,
             0.0f, 2.0f, "%.2f"},
        };
        for (int i = 0; i < 3; ++i) {
            ThreeWay& bd = bands[i];
            ImGui::BeginGroup();
            const float nmW = ImGui::CalcTextSize(bd.name).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (colW - nmW) * 0.5f);
            ImGui::TextColored(V4(COL_INK2), "%s", bd.name);
            const float discX = ImGui::GetCursorPosX() + (colW - radius * 2.0f) * 0.5f;
            ImGui::SetCursorPosX(discX);
            std::string discId = std::string("##tw") + std::to_string(i);
            if (DrawTintDisc(discId.c_str(), bd.ab, 25.0, radius)) {
                *bd.has = (bd.ab[0] != 0.0 || bd.ab[1] != 0.0); wheelsChanged = true;
            }
            float m = static_cast<float>(WheelMaster(bd.triple));
            std::string mId = std::string("##twm") + std::to_string(i);
            if (WheelLumSlider(mId.c_str(), &m, bd.mMin, bd.mMax, static_cast<float>(bd.neutral),
                               bd.fmt, colW)) {
                WheelSetMaster(bd.triple, m); wheelsChanged = true;
            }
            std::string rId = std::string("Reset##tw") + std::to_string(i);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (colW - 54.0f) * 0.5f);
            if (ImGui::SmallButton(rId.c_str())) {
                bd.ab[0] = bd.ab[1] = 0.0; *bd.has = false;
                bd.triple[0] = bd.triple[1] = bd.triple[2] = bd.neutral; wheelsChanged = true;
            }
            ImGui::EndGroup();
            if (i < 2) ImGui::SameLine(0, gap);
        }
        NoteBox("Desaturated LAB tint ring over feathered shadow / mid / highlight bands; these "
                "tints add to the theme, the sliders reuse the Lift/Gamma/Gain luminance.");
    }
    if (wheelsChanged) {
        ParamEdit e; e.field = EditField::Wheels; e.wheels = ui.wheels;
        w->edits.push(e);
    }
}

void DrawInspectorTabs(WindowImpl* w, ParamSnapshot& ui) {
    if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Correct")) { DrawCorrectTab(w, ui); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Basics"))  { DrawBasicsTab(w, ui);  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Grade"))   { DrawGradeTab(w, ui);   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Curves"))  { DrawCurvesTab(w, ui);  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Wheels"))  { DrawWheelsTab(w, ui);  ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

// --- title strip ------------------------------------------------------------

// Reset the manual grade STACK (Basics / Curves / Wheels + the keyframeable exposure /
// temperature / look-mix) to neutral and push the edits. Leaves the format choices (footage
// profile, theme selection, strength/skin/chroma knobs) alone - those are look decisions,
// not slider values.
void ResetManualStack(WindowImpl* w, ParamSnapshot& ui) {
    ui.manual = ManualState{};
    ui.curves = CurvesState{};
    ui.wheels = WheelsState{};
    ui.exposure = 0.0; ui.temperature = 0.0; ui.lookMix = 1.0;
    for (int i = 0; i < 4; ++i) w->curveDrag[i] = -1;
    ParamEdit m; m.field = EditField::Manual; m.manual = ui.manual; w->edits.push(m);
    ParamEdit c; c.field = EditField::Curves; c.curves = ui.curves; w->edits.push(c);
    ParamEdit wh; wh.field = EditField::Wheels; wh.wheels = ui.wheels; w->edits.push(wh);
    w->edits.push({EditField::Exposure, 0.0});
    w->edits.push({EditField::Temperature, 0.0});
    w->edits.push({EditField::LookMix, 1.0});
}

void DrawTitleStrip(WindowImpl* w, ParamSnapshot& ui) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c = ImGui::GetCursorScreenPos();
    // brass "safelight" dot
    const float dotR = 7.0f;
    const ImVec2 dc(c.x + dotR, c.y + ImGui::GetTextLineHeight() * 0.5f + 1.0f);
    dl->AddCircleFilled(dc, dotR, COL_BRASS, 20);
    dl->AddCircleFilled(ImVec2(dc.x - 2.0f, dc.y - 2.0f), 2.4f, COL_BRASS2, 12);
    ImGui::Dummy(ImVec2(dotR * 2.0f + 6.0f, 0));
    ImGui::SameLine();
    ImGui::TextUnformatted("ColorGrade");
    ImGui::SameLine();
    ImGui::TextColored(V4(COL_INK3), "|");
    ImGui::SameLine();
    if (ui.theme == kThemeReferenceIndex)
        ImGui::TextColored(V4(COL_BRASS2), "Reference match");
    else {
        int titleThemeIdx = ui.theme - 1;
        if (titleThemeIdx < 0) titleThemeIdx = 0;
        if (titleThemeIdx > kThemeCount - 1) titleThemeIdx = kThemeCount - 1;
        ImGui::TextColored(V4(COL_INK2), "%s", kThemeNames[titleThemeIdx]);
    }

    // right-aligned: analysis chip + Reset all
    ImGuiStyle& style = ImGui::GetStyle();
    const char* resetLbl = "Reset all";
    const float resetW = ImGui::CalcTextSize(resetLbl).x + style.FramePadding.x * 2.0f;
    AnalysisStatus st = w->readStatus();
    char chip[32];
    std::snprintf(chip, sizeof(chip), "* %s",
                  st.state == AnalysisStatus::State::Analyzed ? "analyzed"
                  : st.state == AnalysisStatus::State::Sampling ? "analyzing" : "not analyzed");
    const float chipW = ImGui::CalcTextSize(chip).x;
    float rightX = ImGui::GetWindowWidth() - style.WindowPadding.x - resetW - chipW - 12.0f;
    if (rightX < ImGui::GetCursorPosX()) rightX = ImGui::GetCursorPosX();
    ImGui::SameLine();
    ImGui::SetCursorPosX(rightX);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(st.state == AnalysisStatus::State::Analyzed ? V4(COL_OK) : V4(COL_INK3),
                       "%s", chip);
    ImGui::SameLine();
    if (ImGui::Button(resetLbl)) ResetManualStack(w, ui);
    ImGui::Separator();
}

// --- agent execution (cg-agent-wiring) --------------------------------------
//
// The editor buttons run the real agent pipeline by spawning a short-lived Node
// subprocess (scripts/agentBridge.ts) that reuses src/agent + src/core (the TS
// oracle). The pure protocol + apply translation are in AgentBridge.h and are
// headless-tested (native:agent-test); everything below is the Win32 glue
// (frame dump, file dialogs, CreateProcess) that AE-verification covers. All of
// it is isolated so a spawn/read failure surfaces as a specific panel error and
// never a silent no-op (DoD item 5). BYOK: the Gemini key rides GEMINI_API_KEY
// on the child only, never the request file.

std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// A per-session temp file path (native OS path) under %TEMP%.
std::string AgentTempPath(const std::string& leaf) {
    wchar_t dir[MAX_PATH];
    DWORD n = ::GetTempPathW(MAX_PATH, dir);
    std::wstring p = (n > 0 && n < MAX_PATH) ? std::wstring(dir, n) : std::wstring(L".\\");
    p += WidenUtf8(leaf);
    return NarrowUtf8(p);
}

// Write a PreviewFrame (RGBA8) as the bridge's raw dump: "CGF1" + int32 w/h/channels + bytes.
bool WriteFrameDump(const std::string& path, const PreviewFrame& f) {
    if (!f.valid()) return false;
    std::ofstream out(WidenUtf8(path).c_str(), std::ios::binary);
    if (!out) return false;
    int32_t hdr[4] = {0, f.width, f.height, 4};
    std::memcpy(hdr, "CGF1", 4);
    out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(f.rgba.data()),
              static_cast<std::streamsize>(f.rgba.size()));
    return out.good();
}

// Resolve the bridge command line. CG_AGENT_BRIDGE = the runnable script (required); the
// launcher is CG_AGENT_NODE (default "node"). On the dev box the captain sets, e.g.,
// CG_AGENT_BRIDGE=C:\dev\color-grade-plugin\scripts\agentBridge.ts and CG_AGENT_NODE="npx tsx".
// Returns "" when the bridge is not configured / not found, so the panel shows a clear error.
std::string ResolveAgentBridge() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = ::GetEnvironmentVariableW(L"CG_AGENT_BRIDGE", buf, MAX_PATH * 4);
    if (n == 0 || n >= MAX_PATH * 4) return "";
    std::wstring bridge(buf, n);
    if (::GetFileAttributesW(bridge.c_str()) == INVALID_FILE_ATTRIBUTES) return "";
    return NarrowUtf8(bridge);
}
std::string ResolveAgentLauncher() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = ::GetEnvironmentVariableW(L"CG_AGENT_NODE", buf, MAX_PATH * 4);
    if (n == 0 || n >= MAX_PATH * 4) return "node";
    return NarrowUtf8(std::wstring(buf, n));
}

// Spawn `<launcher> "<bridge>" "<req>" "<resp>"` via cmd.exe (so npx/.cmd shims and PATH
// resolve), set GEMINI_API_KEY for the child, and wait. Returns true on a clean exit(0).
bool SpawnBridge(WindowImpl* w, const std::string& bridge, const std::string& reqPath, const std::string& respPath,
                 const std::string& key, std::string& err) {
    std::string launcher = ResolveAgentLauncher();
    // A .ts bridge needs tsx; if the launcher is the plain default, upgrade it so the repo
    // script runs out of the box (CG_AGENT_NODE still overrides for a prebuilt .js/.mjs).
    if (launcher == "node" && bridge.size() > 3 && bridge.compare(bridge.size() - 3, 3, ".ts") == 0)
        launcher = "npx tsx";
    // Build the command run under cmd.exe /c. Quote paths defensively.
    std::string cmd = "cmd.exe /c " + launcher + " \"" + bridge + "\" \"" + reqPath + "\" \"" + respPath + "\"";
    std::wstring wcmd = WidenUtf8(cmd);
    std::vector<wchar_t> mutableCmd(wcmd.begin(), wcmd.end());
    mutableCmd.push_back(L'\0');

    // The key is set on THIS process's env right before spawn so the child inherits it (a
    // single job runs at a time). Empty key = leave whatever is set (mock/free paths still run).
    if (!key.empty()) ::SetEnvironmentVariableW(L"GEMINI_API_KEY", WidenUtf8(key).c_str());

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    // Run in the bridge's directory so a repo-relative tsx resolves node_modules.
    std::wstring bridgeDir = WidenUtf8(bridge);
    size_t slash = bridgeDir.find_last_of(L"\\/");
    std::wstring cwd = slash == std::wstring::npos ? std::wstring() : bridgeDir.substr(0, slash);

    BOOL ok = ::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (!ok) {
        err = "could not launch the agent bridge (CreateProcess failed)";
        return false;
    }
    // Publish the live child handle so teardown/close can TerminateProcess it and let this
    // wait return promptly instead of blocking the join (and g_mapMutex) up to 120s.
    w->agentChildProcess.store(pi.hProcess);
    DWORD wait = ::WaitForSingleObject(pi.hProcess, 120000);  // 2 min ceiling for a model round
    if (wait == WAIT_TIMEOUT) {
        // Child overran the ceiling: kill it so it isn't orphaned and can't later collide
        // on the per-window resp/req temp paths.
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    // Retire the handle before closing it, so a concurrent teardown can't TerminateProcess a
    // handle we're about to close (single owner of CloseHandle).
    w->agentChildProcess.store(nullptr);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    // The child inherited the key; clear it from this (host) process env so it does
    // not linger for any later child AE spawns.
    if (!key.empty()) ::SetEnvironmentVariableW(L"GEMINI_API_KEY", nullptr);
    if (wait == WAIT_TIMEOUT) {
        err = "the agent bridge timed out";
        return false;
    }
    if (code != 0) {
        err = "the agent bridge exited with an error";
        return false;
    }
    return true;
}

// Store a finished/failed job result and clear the busy flag (worker thread).
void FinishAgentJob(WindowImpl* w, AgentJobState state, const AgentResponse& resp, const std::string& error) {
    {
        std::lock_guard<std::mutex> lk(w->agentMutex);
        w->agentResult.state = state;
        w->agentResult.response = resp;
        w->agentResult.error = error;
    }
    w->agentWantsFrame.store(false);
    w->agentBusy.store(false);
}

// The worker: write inputs, spawn the bridge, parse the result. For critique/autograde a frame
// dump path is required; the worker POLLS the window for the frame (the idle hook may only
// publish the decoded-source "before" frame a tick after agentWantsFrame is set). needFrame
// picks whether a frame is required; wantSource picks the decoded-source vs graded preview.
void AgentWorker(WindowImpl* w, AgentRequest req, std::string key, std::string framePath,
                 bool needFrame, bool wantSource) {
    AgentResponse resp;
    std::string err;
    const std::string bridge = ResolveAgentBridge();
    if (bridge.empty()) {
        resp.ok = false;
        FinishAgentJob(w, AgentJobState::Failed, resp,
                       "agent bridge not configured - set CG_AGENT_BRIDGE to scripts/agentBridge.ts");
        return;
    }

    // Dump the frame for critique/autograde, polling up to ~4s for it to be published.
    if (needFrame) {
        std::shared_ptr<const PreviewFrame> frame;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < deadline) {
            if (wantSource) { std::lock_guard<std::mutex> lk(w->beforeMutex); frame = w->pendingBefore; }
            else            { std::lock_guard<std::mutex> lk(w->previewMutex); frame = w->pendingPreview; }
            if (frame && frame->valid()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        if (!frame || !WriteFrameDump(framePath, *frame)) {
            FinishAgentJob(w, AgentJobState::Failed, resp,
                           "no source frame available - scrub the timeline once, then retry");
            return;
        }
        req.framePath = framePath;
    }

    const std::string tag = std::to_string(w->key);
    const std::string reqPath = AgentTempPath("cg-agent-req-" + tag + ".txt");
    const std::string respPath = AgentTempPath("cg-agent-resp-" + tag + ".txt");
    {
        std::ofstream ro(WidenUtf8(reqPath).c_str(), std::ios::binary);
        if (!ro) { FinishAgentJob(w, AgentJobState::Failed, resp, "could not write the agent request"); return; }
        ro << formatAgentRequest(req);
    }

    if (!SpawnBridge(w, bridge, reqPath, respPath, key, err)) {
        FinishAgentJob(w, AgentJobState::Failed, resp, err);
        return;
    }
    std::ifstream ri(WidenUtf8(respPath).c_str(), std::ios::binary);
    if (!ri) { FinishAgentJob(w, AgentJobState::Failed, resp, "the agent bridge produced no response"); return; }
    std::ostringstream ss; ss << ri.rdbuf();
    ri.close();
    resp = parseAgentResponse(ss.str());
    // Clean up the transient request/response/frame files (the reference sidecar the
    // effect reads is written to outPath and intentionally left in place).
    ::DeleteFileW(WidenUtf8(reqPath).c_str());
    ::DeleteFileW(WidenUtf8(respPath).c_str());
    if (needFrame && !framePath.empty()) ::DeleteFileW(WidenUtf8(framePath).c_str());
    FinishAgentJob(w, AgentJobState::Done, resp, resp.ok ? "" : resp.message);
}

// Join any previous worker so we never leak/overlap threads, then reset it.
void JoinAgentThread(WindowImpl* w) {
    if (w->agentThread.joinable()) w->agentThread.join();
}

// Launch a critique / auto-grade job. Captures inputs on the UI thread (frame, params, key),
// then hands off to a worker. `wantSource` picks the decoded-source (before) frame for
// auto-grade vs the graded preview for critique.
void LaunchFrameAgent(WindowImpl* w, AgentCommand command, const ParamSnapshot& ui, bool wantSource) {
    if (w->agentBusy.exchange(true)) return;  // one job at a time
    JoinAgentThread(w);
    {
        std::lock_guard<std::mutex> lk(w->agentMutex);
        w->agentResult = AgentJobResult{};
        w->agentResult.state = AgentJobState::Running;
        w->agentResult.command = command;
    }

    // Auto-grade needs the decoded source frame; ask the idle hook to publish it (it may not be
    // available in After-only mode). The worker polls for it. Critique uses the graded preview.
    if (wantSource) w->agentWantsFrame.store(true);

    AgentRequest req;
    req.command = command;
    // The editor frame agent has no reference IMAGE to pass here (reference matching is the
    // separate sidecar stat-transfer flow via LaunchReferenceAgent), so never label the job
    // shot-match - that would send the critic a shot-match prompt with no reference.
    req.mode = "correction";
    req.profile = cg::core::footageKeyForIndex(ui.footageProfile);
    req.theme = themeKeyForPopup(ui.theme);
    req.hasStrength = true; req.strength = ui.strength;
    req.hasSkinProtection = true; req.skinProtection = ui.skinProtection;
    req.hasChromaGain = true; req.chromaGain = ui.chromaGain;
    if (command == AgentCommand::Autograde) { req.hasRounds = true; req.rounds = 4; }
    const std::string key = w->agentKeyBuf;
    const std::string framePath = AgentTempPath("cg-agent-frame-" + std::to_string(w->key) + ".bin");

    w->agentThread = std::thread(AgentWorker, w, req, key, framePath, /*needFrame=*/true, wantSource);
}

// Launch the reference-measurement job: measure a picked still into the sidecar the effect's
// LoadReferenceStats already reads, then (on success) the panel switches Theme -> Reference.
void LaunchReferenceAgent(WindowImpl* w, const ParamSnapshot& ui, const std::string& imagePath,
                          const std::string& sidecarPath) {
    if (w->agentBusy.exchange(true)) return;
    JoinAgentThread(w);
    {
        std::lock_guard<std::mutex> lk(w->agentMutex);
        w->agentResult = AgentJobResult{};
        w->agentResult.state = AgentJobState::Running;
        w->agentResult.command = AgentCommand::Reference;
    }
    w->agentRefSidecar = sidecarPath;  // where the effect will read the measured stats from
    AgentRequest req;
    req.command = AgentCommand::Reference;
    req.profile = cg::core::footageKeyForIndex(ui.footageProfile);
    req.referencePath = imagePath;
    req.outPath = sidecarPath;
    w->agentThread = std::thread(AgentWorker, w, req, std::string(), std::string(),
                                 /*needFrame=*/false, /*wantSource=*/false);
}

// Launch the batch consistency job over the picked clips (no model call).
void LaunchBatchAgent(WindowImpl* w, const ParamSnapshot& ui, const std::vector<std::string>& clips) {
    if (w->agentBusy.exchange(true)) return;
    JoinAgentThread(w);
    {
        std::lock_guard<std::mutex> lk(w->agentMutex);
        w->agentResult = AgentJobResult{};
        w->agentResult.state = AgentJobState::Running;
        w->agentResult.command = AgentCommand::Batch;
    }
    AgentRequest req;
    req.command = AgentCommand::Batch;
    req.profile = cg::core::footageKeyForIndex(ui.footageProfile);
    // Batch needs a stat-match theme; None/Reference have none, so fall back to teal-orange.
    std::string tk = themeKeyForPopup(ui.theme);
    req.theme = tk.empty() ? "teal-orange" : tk;
    req.hasStrength = true; req.strength = ui.strength;
    req.clipPaths = clips;
    w->agentThread = std::thread(AgentWorker, w, req, std::string(), std::string(),
                                 /*needFrame=*/false, /*wantSource=*/false);
}

// Apply an accepted auto-grade result by translating it into ParamEdits and pushing them onto
// the same edit queue every control uses, so the idle hook writes them to the recipe.
void ApplyAgentAutograde(WindowImpl* w, const ParamSnapshot& ui, const AgentResponse& resp) {
    std::vector<ParamEdit> edits = translateAgentApply(resp.apply, ui);
    for (const auto& e : edits) w->edits.push(e);
}

// A Win32 multi-select open dialog. Returns picked native paths (empty on cancel). `image`
// restricts the filter to TIFF/PNG (what the TS decoders support).
std::vector<std::string> PickFiles(WindowImpl* w, bool multi) {
    wchar_t buf[8192] = {0};
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = w->hwnd.load();
    ofn.lpstrFilter = L"Images (TIFF, PNG)\0*.tif;*.tiff;*.png\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 8192;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (multi) ofn.Flags |= OFN_ALLOWMULTISELECT;
    std::vector<std::string> out;
    if (!::GetOpenFileNameW(&ofn)) return out;
    // Multi-select returns dir\0file1\0file2\0\0; single returns the full path.
    std::wstring dir = buf;
    const wchar_t* p = buf + dir.size() + 1;
    if (!multi || *p == L'\0') {
        out.push_back(NarrowUtf8(dir));
        return out;
    }
    while (*p) {
        std::wstring name = p;
        out.push_back(NarrowUtf8(dir + L"\\" + name));
        p += name.size() + 1;
    }
    return out;
}

// --- agent dock (confined indigo world; the single Pro/BYOK seam) -----------

// Draw the collapsed rail: a slim indigo strip with an expand affordance.
void DrawAgentRail(WindowImpl* w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(0, 4));
    // glyph button
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 26.0f) * 0.5f);
    ImVec2 gp = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##agentglyph", ImVec2(26, 26))) w->agentOpen = true;
    dl->AddRectFilled(gp, ImVec2(gp.x + 26, gp.y + 26), COL_IRISRAISE, 8.0f);
    dl->AddCircleFilled(ImVec2(gp.x + 13, gp.y + 13), 6.0f, COL_IRIS, 16);
    // vertical "AGENT" label
    ImGui::Dummy(ImVec2(0, 10));
    const char* letters = "AGENT";
    for (const char* p = letters; *p; ++p) {
        char s[2] = {*p, 0};
        const float lw = ImGui::CalcTextSize(s).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - lw) * 0.5f);
        ImGui::TextColored(V4(COL_IRIS2), "%s", s);
    }
    // expand button at the bottom
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 34.0f);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 26.0f) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Button, V4(COL_IRISRAISE));
    ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_IRIS2));
    if (ImGui::Button("<", ImVec2(26, 26))) w->agentOpen = true;
    ImGui::PopStyleColor(2);
}

// Render the last agent job's status/result, and apply an accepted result exactly once. This
// is the immediate-feedback surface (DoD item 5): every job shows Running / a specific result /
// a specific error, never a silent no-op. Auto-grade edits are pushed onto the shared edit
// queue; a finished Reference measurement flips Theme -> Reference.
void DrawAgentResult(WindowImpl* w, ParamSnapshot& ui) {
    AgentJobResult r;
    { std::lock_guard<std::mutex> lk(w->agentMutex); r = w->agentResult; }

    if (r.state == AgentJobState::Idle) return;
    ImGui::Separator();
    if (r.state == AgentJobState::Running) {
        const int dots = 1 + (static_cast<int>(ImGui::GetTime() * 2.0) % 3);
        ImGui::TextColored(V4(COL_BRASS2), "Working%.*s", dots, "...");
        ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK3));
        ImGui::TextWrapped(r.command == AgentCommand::Autograde
                               ? "Rendering + critiquing rounds; the rules decide accept/reject."
                               : r.command == AgentCommand::Critique ? "Sending the frame to Gemini..."
                               : r.command == AgentCommand::Reference ? "Measuring the reference still..."
                                                                       : "Comparing clips...");
        ImGui::PopStyleColor();
        return;
    }
    if (r.state == AgentJobState::Failed || !r.response.ok) {
        ImGui::TextColored(V4(COL_ERR), "Error");
        ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK2));
        ImGui::TextWrapped("%s", (!r.error.empty() ? r.error : r.response.message).c_str());
        ImGui::PopStyleColor();
        return;
    }

    // Done + ok. Apply an accepted result once.
    const AgentResponse& resp = r.response;
    if (!w->agentApplied) {
        if (r.command == AgentCommand::Autograde && !resp.apply.empty()) {
            ApplyAgentAutograde(w, ui, resp);
        } else if (r.command == AgentCommand::Reference) {
            // Point the effect's reference loader at the freshly-measured sidecar (LoadReferenceStats
            // reads CG_REF_STATS_PATH first), then switch the theme so the next bake matches it.
            if (!w->agentRefSidecar.empty())
                ::SetEnvironmentVariableW(L"CG_REF_STATS_PATH", WidenUtf8(w->agentRefSidecar).c_str());
            ui.theme = kThemeReferenceIndex;
            w->edits.push({EditField::Theme, static_cast<double>(ui.theme)});
        }
        w->agentApplied = true;
    }

    switch (r.command) {
        case AgentCommand::Critique: {
            ImGui::TextColored(V4(COL_OK), "Critique");
            ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK2));
            if (resp.defects.empty()) ImGui::TextWrapped("No defects named - the grade looks clean.");
            for (const auto& d : resp.defects) ImGui::BulletText("%s", d.c_str());
            ImGui::PopStyleColor();
            ImGui::TextColored(V4(COL_INK3), "Model names defects; the rules decide across rounds.");
            break;
        }
        case AgentCommand::Autograde: {
            ImGui::TextColored(V4(COL_OK), resp.accepted ? "Auto-grade applied" : "Auto-grade: baseline kept");
            ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK2));
            ImGui::TextWrapped("%s", resp.message.c_str());
            if (!resp.stopReason.empty()) ImGui::TextColored(V4(COL_INK3), "%s", resp.stopReason.c_str());
            if (!resp.apply.empty()) {
                ImGui::Dummy(ImVec2(0, 2));
                ImGui::TextColored(V4(COL_INK3), "Applied to recipe:");
                for (const auto& a : resp.apply) ImGui::BulletText("%s", a.field.c_str());
            }
            if (!resp.unmapped.empty()) {
                ImGui::TextColored(V4(COL_BRASS2), "Proposed, no editor control:");
                for (const auto& u : resp.unmapped) ImGui::BulletText("%s", u.c_str());
            }
            ImGui::PopStyleColor();
            break;
        }
        case AgentCommand::Reference: {
            ImGui::TextColored(V4(COL_OK), "Reference measured");
            ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK2));
            ImGui::TextWrapped("%s", resp.message.c_str());
            ImGui::TextColored(V4(COL_INK3), "Theme set to Reference Match; your Basics/Curves/Wheels ride on top.");
            ImGui::PopStyleColor();
            break;
        }
        case AgentCommand::Batch: {
            ImGui::TextColored(resp.diverged.empty() ? V4(COL_OK) : V4(COL_BRASS2),
                               resp.diverged.empty() ? "Clips consistent" : "Drift found");
            ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK2));
            ImGui::TextWrapped("%s", resp.message.c_str());
            for (const auto& d : resp.diverged) {
                ImGui::BulletText("%s vs %s", d.clipA.c_str(), d.clipB.c_str());
                ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK3));
                ImGui::TextWrapped("  %s", d.reason.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopStyleColor();
            break;
        }
    }
}

// Draw the expanded agent panel with its BYOK states (setup / ready). Agent EXECUTION runs
// from here (cg-agent-wiring): each action spawns the Node bridge (scripts/agentBridge.ts)
// on a worker thread; DrawAgentResult shows Running/result/error and applies accepted results.
void DrawAgentPanel(WindowImpl* w, ParamSnapshot& ui) {
    // Push the confined indigo world onto the button/frame colours for this panel only.
    ImGui::PushStyleColor(ImGuiCol_Button, V4(COL_IRISRAISE));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V4(COL_IRISLINE));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, V4(COL_IRISDIM));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, V4(COL_IRISRAISE));
    ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK));

    // header
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 hp = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(hp.x + 8, hp.y + ImGui::GetTextLineHeight() * 0.5f + 1), 6.0f, COL_IRIS, 16);
    ImGui::Dummy(ImVec2(20, 0)); ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Agent");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 32.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_IRIS2));
    if (ImGui::Button(">", ImVec2(24, 22))) w->agentOpen = false;
    ImGui::PopStyleColor();
    ImGui::Separator();

    const bool busy = w->agentBusy.load();

    // --- BYOK key row (always present). Critique/Auto-grade need a key; Reference/Batch don't. ---
    if (!w->agentKeySet) {
        ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK2));
        ImGui::TextWrapped("Critique / Auto-grade use your own Gemini API key (free tier = $0). Frames "
                           "go to Google only when you press one of those. Reference / Batch use no key.");
        ImGui::PopStyleColor();
        ImGui::TextColored(V4(COL_INK2), "API key");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##agentkey", w->agentKeyBuf, sizeof(w->agentKeyBuf), ImGuiInputTextFlags_Password);
        const bool hasText = w->agentKeyBuf[0] != 0;
        ImGui::BeginDisabled(!hasText);
        ImGui::PushStyleColor(ImGuiCol_Button, V4(COL_IRISDIM));
        if (ImGui::Button("Save key", ImVec2(-FLT_MIN, 0)) && hasText) w->agentKeySet = true;
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
    } else {
        std::string k = w->agentKeyBuf;
        std::string masked = k.size() > 8 ? (k.substr(0, 4) + std::string("....") + k.substr(k.size() - 4))
                                          : std::string("....");
        ImGui::TextColored(V4(COL_INK2), "Key");
        ImGui::SameLine();
        ImGui::TextColored(V4(COL_IRIS2), "%s", masked.c_str());
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 72.0f);
        if (ImGui::SmallButton("Remove")) { w->agentKeySet = false; w->agentKeyBuf[0] = 0; }
    }
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_IRIS2));
    ImGui::TextWrapped("Button-triggered - never automatic. Model NAMES defects, rules decide.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 2));

    // inner tabs (each label unique, no ID collision with the action buttons below).
    const char* tabs[3] = {"Grade", "Reference", "Batch"};
    for (int i = 0; i < 3; ++i) {
        const bool on = w->agentTab == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, V4(COL_IRISPANEL));
        if (ImGui::Button(tabs[i])) w->agentTab = i;
        if (on) ImGui::PopStyleColor();
        if (i < 2) ImGui::SameLine(0, 4);
    }
    ImGui::Separator();

    ImGui::BeginDisabled(busy);
    if (w->agentTab == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK3));
        ImGui::TextWrapped("Critique names defects on the current frame. Auto-grade tunes the grade over "
                           "a few rounds (rules accept/reject) and applies the result to your recipe.");
        ImGui::PopStyleColor();
        ImGui::BeginDisabled(!w->agentKeySet);
        if (ImGui::Button("Critique frame", ImVec2(-FLT_MIN, 0))) {
            w->agentApplied = false;
            LaunchFrameAgent(w, AgentCommand::Critique, ui, /*wantSource=*/false);
        }
        if (ImGui::Button("Auto-grade", ImVec2(-FLT_MIN, 0))) {
            w->agentApplied = false;
            LaunchFrameAgent(w, AgentCommand::Autograde, ui, /*wantSource=*/true);
        }
        ImGui::EndDisabled();
        if (!w->agentKeySet) ImGui::TextColored(V4(COL_INK3), "Enter a key above to enable these.");
    } else if (w->agentTab == 1) {
        ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK3));
        ImGui::TextWrapped("Pick a reference still (TIFF/PNG); it is measured on the TS side and the clip "
                           "is matched to it - no sidecar to hand-produce. No model call.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Pick reference image...", ImVec2(-FLT_MIN, 0))) {
            std::vector<std::string> picked = PickFiles(w, /*multi=*/false);
            if (!picked.empty()) {
                w->agentApplied = false;
                const std::string sidecar = AgentTempPath("ColorGrade_Reference.stats");
                LaunchReferenceAgent(w, ui, picked[0], sidecar);
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK3));
        ImGui::TextWrapped("Power-user override: set CG_REF_STATS_PATH or drop ColorGrade_Reference.stats "
                           "next to the plug-in.");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, V4(COL_INK3));
        ImGui::TextWrapped("Pick 2+ same-scene clips (TIFF/PNG); each is graded with the current theme and "
                           "compared for drift. No model call. Native comp harvest is deferred.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Pick clips to compare...", ImVec2(-FLT_MIN, 0))) {
            std::vector<std::string> picked = PickFiles(w, /*multi=*/true);
            if (picked.size() >= 2) {
                w->agentApplied = false;
                LaunchBatchAgent(w, ui, picked);
            }
        }
    }
    ImGui::EndDisabled();

    DrawAgentResult(w, ui);

    ImGui::PopStyleColor(5);
}

void DrawAgentDock(WindowImpl* w, ParamSnapshot& ui) {
    if (!kAgentDockEnabled) {  // Pro/BYOK seam disabled: keep the free core fully usable.
        ImGui::TextDisabled("Agent features off");
        return;
    }
    if (w->agentOpen) DrawAgentPanel(w, ui);
    else DrawAgentRail(w);
}

// --- footer -----------------------------------------------------------------

void DrawFooter(WindowImpl* w) {
    ImGui::Separator();
    ImGui::TextColored(V4(COL_INK3), "Decode-then-grade - V-Log decoded before any look");
    AnalysisStatus st = w->readStatus();
    const char* right = st.state == AnalysisStatus::State::Sampling ? "analyzing clip..."
                        : st.state == AnalysisStatus::State::Analyzed ? "clip analyzed"
                        : "scrub or Analyze to sample the clip";
    ImGui::SameLine();
    const float rw = ImGui::CalcTextSize(right).x;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - rw - ImGui::GetStyle().WindowPadding.x);
    ImGui::TextColored(V4(COL_INK3), "%s", right);
}

// Draw one ImGui frame from a working copy of the snapshot; push any change onto
// the edit queue. Returns nothing - edits flow through w->edits to the effect.
void DrawEditorUI(WindowImpl* w, ParamSnapshot& ui) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("ColorGradeEditor", nullptr, flags);

    DrawTitleStrip(w, ui);

    // Three-column body (mockup): preview + scopes | inspector | collapsible agent dock,
    // with a slim footer. Widths are computed from the live content region every frame so
    // windowed / fullscreen / live-resize all track, and collapsing the agent reflows.
    ImGuiStyle& style = ImGui::GetStyle();
    const float footerH = ImGui::GetTextLineHeight() + 12.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float bodyH = avail.y - footerH - style.ItemSpacing.y;
    const float inspW = 360.0f;
    const float agentW = w->agentOpen ? 300.0f : 48.0f;
    float leftW = avail.x - inspW - agentW - style.ItemSpacing.x * 2.0f;
    if (leftW < 220.0f) leftW = 220.0f;

    // LEFT: preview + scopes
    ImGui::BeginChild("left", ImVec2(leftW, bodyH), true);
    DrawViewerToolbar(w);
    DrawPreviewPane(w);
    if (w->showScopes.load()) DrawScopesStrip(w);
    ImGui::EndChild();
    ImGui::SameLine();

    // MIDDLE: inspector tabs (Correct / Basics / Grade / Curves / Wheels)
    ImGui::PushStyleColor(ImGuiCol_ChildBg, V4(COL_BG2));
    ImGui::BeginChild("insp", ImVec2(inspW, bodyH), true);
    DrawInspectorTabs(w, ui);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::SameLine();

    // RIGHT: agent dock (confined indigo world; collapses to a slim rail)
    ImGui::PushStyleColor(ImGuiCol_ChildBg, V4(w->agentOpen ? COL_IRISPANEL : COL_IRISBG));
    ImGui::PushStyleColor(ImGuiCol_Border, V4(COL_IRISLINE));
    ImGui::BeginChild("agent", ImVec2(agentW, bodyH), true);
    DrawAgentDock(w, ui);
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    DrawFooter(w);

    ImGui::End();
}

void RunWindowThread(WindowImpl* w, ParamSnapshot seed) {
    ImGui_ImplWin32_EnableDpiAwareness();
    EnsureWindowClass();

    HWND hwnd = ::CreateWindowExW(0, kWindowClass, L"Color Grade - Editor",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1240, 720,
                                  nullptr, nullptr, PluginInstance(), nullptr);
    if (!hwnd) { w->finished.store(true); return; }
    // Publish the HWND for the main thread only once it is fully valid.
    w->hwnd.store(hwnd);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));

    if (!CreateDeviceD3D(w)) {
        w->hwnd.store(nullptr);
        ::DestroyWindow(hwnd);
        w->finished.store(true);
        return;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Per-window ImGui context, made current on THIS thread. With the thread-local
    // GImGui override (imconfig.h) each window's thread has its own current context,
    // so multiple open editors never race one shared global context.
    IMGUI_CHECKVERSION();
    w->imguiCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(w->imguiCtx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't litter the user's disk with imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#ifdef _DEBUG
    // Keep ImGui's duplicate-ID detector ON in Debug so any future same-label widget
    // clash surfaces immediately (captain round-1 finding: Split radio vs split slider).
    // Default is already true in this ImGui (1.91.5); set explicitly so it never regresses.
    io.ConfigDebugHighlightIdConflicts = true;
#endif
    ApplyColorGradeStyle();  // cg-ui-polish "Darkroom" style (replaces StyleColorsDark)
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(w->device, w->context);

    // The UI's working copy of the params; starts from the seed the effect passed.
    ParamSnapshot ui = seed;
    { std::lock_guard<std::mutex> lk(w->snapMutex); w->snapshot = seed; }

    // Enter the UI loop only if a teardown wasn't already requested during startup.
    // A close()/shutdownAll() that raced ahead of us set stopRequested; honor it so
    // we fall straight through to teardown instead of spinning a loop no WM_CLOSE
    // can reach (the HWND may not have been published when close ran).
    if (!w->stopRequested.load()) w->running.store(true);
    const float clear[4] = {0.10f, 0.10f, 0.11f, 1.0f};
    while (w->running.load() && !w->stopRequested.load()) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) w->running.store(false);
        }
        if (!w->running.load() || w->stopRequested.load()) break;

        // Adopt a newer snapshot from the effect for any control the user is NOT
        // currently editing (ImGui's active-id guards against clobbering a drag).
        ParamSnapshot latest = w->readSnapshot();
        if (latest.revision > ui.revision && !ImGui::IsAnyItemActive()) ui = latest;

        // Upload the newest live-preview frame (if the effect published one) into the GPU
        // texture, and recompute the scopes from it, before drawing. Only touches D3D on
        // this UI thread. The before frame (Phase 5) uploads the same way.
        if (auto frame = w->takePendingPreview()) {
            if (frame->valid()) {
                UploadFrame(w, w->previewTex, *frame);
                UpdateScopes(w, *frame);  // scopes read the graded output (decode invariant)
            }
        }
        if (auto before = w->takePendingBefore()) {
            if (before->valid()) UploadFrame(w, w->beforeTex, *before);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawEditorUI(w, ui);
        ImGui::Render();

        w->context->OMSetRenderTargets(1, &w->rtv, nullptr);
        w->context->ClearRenderTargetView(w->rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        w->swapChain->Present(1, 0);  // vsync-paced; ~cheap when idle
    }

    ReleaseGpuTex(w->previewTex);
    ReleaseGpuTex(w->beforeTex);
    ReleaseGpuTex(w->waveTex);
    ReleaseGpuTex(w->histTex);
    ReleaseGpuTex(w->vecTex);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(w->imguiCtx);
    w->imguiCtx = nullptr;
    ImGui::SetCurrentContext(nullptr);
    CleanupDeviceD3D(w);
    HWND doomed = w->hwnd.exchange(nullptr);
    if (doomed) ::DestroyWindow(doomed);
    w->finished.store(true);
}

// --- registry ---------------------------------------------------------------

std::mutex                            g_mapMutex;
std::map<InstanceKey, WindowImpl*>    g_windows;

// Join + delete any window whose thread has finished (user closed it). Called
// under g_mapMutex from the frequently-hit effect entry points, so a dismissed
// window is reaped promptly without a dedicated per-instance teardown command.
void ReapFinishedLocked() {
    for (auto it = g_windows.begin(); it != g_windows.end();) {
        WindowImpl* w = it->second;
        // finished => the UI thread has fully torn down (user close, or a device/
        // window setup failure). Either way the HWND is already gone; join + delete.
        if (w->finished.load()) {
            if (w->thread.joinable()) w->thread.join();
            delete w;
            it = g_windows.erase(it);
        } else {
            ++it;
        }
    }
}

void DestroyWindowImplLocked(WindowImpl* w) {
    // stopRequested first: if the UI thread is still starting up (HWND not yet
    // published), it will see this flag at the loop gate and skip the loop entirely
    // instead of clobbering running back to true and spinning forever.
    w->stopRequested.store(true);
    w->running.store(false);
    HWND hwnd = w->hwnd.load();
    if (hwnd) ::PostMessageW(hwnd, WM_CLOSE, 0, 0);  // nudge a running loop out
    if (w->thread.joinable()) w->thread.join();
    // Kill any in-flight agent bridge child so the worker's WaitForSingleObject returns at once,
    // instead of blocking the join (and g_mapMutex) up to the 120s ceiling on a delete mid-job.
    // The worker owns CloseHandle; a stale/closed handle here just makes TerminateProcess a no-op.
    if (HANDLE child = w->agentChildProcess.load()) ::TerminateProcess(child, 1);
    // Join any in-flight agent worker so its detach can't outlive the window (it holds `w`).
    if (w->agentThread.joinable()) w->agentThread.join();
    delete w;
}

}  // namespace

// --- EditorHost -------------------------------------------------------------

EditorHost& EditorHost::instance() {
    static EditorHost host;
    return host;
}

EditorHost::~EditorHost() { shutdownAll(); }

void EditorHost::open(InstanceKey key, const ParamSnapshot& seed) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    ReapFinishedLocked();
    auto it = g_windows.find(key);
    if (it != g_windows.end()) {
        // Already open: bring it to the foreground (single instance per effect).
        HWND hwnd = it->second->hwnd.load();
        if (hwnd) {
            ::ShowWindow(hwnd, SW_RESTORE);
            ::SetForegroundWindow(hwnd);
        }
        return;
    }
    WindowImpl* w = new WindowImpl();
    w->key = key;
    w->thread = std::thread(RunWindowThread, w, seed);
    g_windows[key] = w;
}

void EditorHost::publishSnapshot(InstanceKey key, const ParamSnapshot& snapshot) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    ReapFinishedLocked();
    auto it = g_windows.find(key);
    if (it != g_windows.end()) it->second->writeSnapshot(snapshot);
}

void EditorHost::publishPreviewFrame(InstanceKey key, std::shared_ptr<const PreviewFrame> frame) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    ReapFinishedLocked();
    auto it = g_windows.find(key);
    if (it != g_windows.end()) it->second->writePreview(std::move(frame));
}

void EditorHost::publishBeforeFrame(InstanceKey key, std::shared_ptr<const PreviewFrame> frame) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    ReapFinishedLocked();
    auto it = g_windows.find(key);
    if (it != g_windows.end()) it->second->writeBefore(std::move(frame));
}

void EditorHost::publishAnalysisStatus(InstanceKey key, const AnalysisStatus& status) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    ReapFinishedLocked();
    auto it = g_windows.find(key);
    if (it != g_windows.end()) it->second->writeStatus(status);
}

std::vector<ParamEdit> EditorHost::drainEdits(InstanceKey key) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    ReapFinishedLocked();
    auto it = g_windows.find(key);
    if (it != g_windows.end()) return it->second->edits.drain();
    return {};
}

bool EditorHost::wantsBeforeFrame(InstanceKey key) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    auto it = g_windows.find(key);
    if (it == g_windows.end()) return false;
    // The before frame is the ORIGINAL decoded source, which auto-grade needs to re-render
    // candidate params - so a pending agent job forces the checkout even in After-only mode.
    return it->second->compareMode.load() != 0 || it->second->agentWantsFrame.load();
}

bool EditorHost::consumeAnalyzeRequest(InstanceKey key) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    auto it = g_windows.find(key);
    return it != g_windows.end() && it->second->analyzeRequested.exchange(false);
}

std::vector<InstanceKey> EditorHost::openKeys() {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    ReapFinishedLocked();
    std::vector<InstanceKey> keys;
    keys.reserve(g_windows.size());
    for (auto& kv : g_windows) keys.push_back(kv.first);
    return keys;
}

bool EditorHost::hasPendingEdits(InstanceKey key) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    auto it = g_windows.find(key);
    return it != g_windows.end() && !it->second->edits.empty();
}

bool EditorHost::consumeCloseRequest(InstanceKey key) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    auto it = g_windows.find(key);
    if (it == g_windows.end()) return false;
    return it->second->closeRequested.exchange(false);
}

bool EditorHost::isOpen(InstanceKey key) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    ReapFinishedLocked();
    return g_windows.find(key) != g_windows.end();
}

void EditorHost::close(InstanceKey key) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    auto it = g_windows.find(key);
    if (it == g_windows.end()) return;
    DestroyWindowImplLocked(it->second);
    g_windows.erase(it);
}

void EditorHost::shutdownAll() {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    for (auto& kv : g_windows) DestroyWindowImplLocked(kv.second);
    g_windows.clear();
}

}  // namespace editor
}  // namespace cg

#else  // ---- non-Windows: no-op stubs so the interface still links ----------

namespace cg {
namespace editor {

EditorHost& EditorHost::instance() { static EditorHost h; return h; }
EditorHost::~EditorHost() {}
void EditorHost::open(InstanceKey, const ParamSnapshot&) {}
void EditorHost::publishSnapshot(InstanceKey, const ParamSnapshot&) {}
void EditorHost::publishPreviewFrame(InstanceKey, std::shared_ptr<const PreviewFrame>) {}
void EditorHost::publishBeforeFrame(InstanceKey, std::shared_ptr<const PreviewFrame>) {}
void EditorHost::publishAnalysisStatus(InstanceKey, const AnalysisStatus&) {}
std::vector<ParamEdit> EditorHost::drainEdits(InstanceKey) { return {}; }
bool EditorHost::wantsBeforeFrame(InstanceKey) { return false; }
bool EditorHost::consumeAnalyzeRequest(InstanceKey) { return false; }
std::vector<InstanceKey> EditorHost::openKeys() { return {}; }
bool EditorHost::hasPendingEdits(InstanceKey) { return false; }
bool EditorHost::consumeCloseRequest(InstanceKey) { return false; }
bool EditorHost::isOpen(InstanceKey) { return false; }
void EditorHost::close(InstanceKey) {}
void EditorHost::shutdownAll() {}

}  // namespace editor
}  // namespace cg

#endif  // _WIN32
