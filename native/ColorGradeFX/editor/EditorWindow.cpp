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
#include <d3d11.h>

#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/backends/imgui_impl_win32.h"
#include "../../third_party/imgui/backends/imgui_impl_dx11.h"

#include "Scopes.h"  // Phase 5: waveform / histogram / vectorscope image synthesis

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

// --- viewer: toolbar + preview + scopes (Phase 4/5) -------------------------

// Compare-mode + scopes toolbar above the preview. Writes the window's UI-state atomics
// (compareMode, showScopes) + the split position; the effect reads wantsBeforeFrame().
void DrawViewerToolbar(WindowImpl* w) {
    int mode = w->compareMode.load();
    ImGui::TextUnformatted("View:");
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
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Split", &w->splitFraction, 0.0f, 1.0f, "%.2f");
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

    auto tag = [&](const char* s, float x0, float x1, float y) {
        ImVec2 ts = ImGui::CalcTextSize(s);
        dl->AddText(ImVec2((x0 + x1 - ts.x) * 0.5f, y + 4.0f), IM_COL32(210, 210, 220, 255), s);
    };

    if (mode == 1) {  // BeforeOnly (fall back to after if the before frame isn't in yet)
        const GpuTex& t = haveBefore ? w->beforeTex : w->previewTex;
        DrawLetterboxed(dl, t, p0, p1, 0, 0, false);
        tag(haveBefore ? "Before (original)" : "Before (pending)", p0.x, p1.x, p0.y);
    } else if (mode == 2 && haveBefore && haveAfter) {  // Split
        // Both frames share the clip dimensions -> one letterbox rect; the divider splits it.
        SplitGeometry g = splitViewGeometry(p1.x - p0.x, p1.y - p0.y, w->previewTex.w,
                                            w->previewTex.h, w->splitFraction);
        const float dstX0 = p0.x + g.dst.x;
        const float dstX1 = p0.x + g.dst.x + g.dst.w;
        const float splitX = p0.x + g.splitX;
        DrawLetterboxed(dl, w->beforeTex, p0, p1, dstX0, splitX, true);   // before: left
        DrawLetterboxed(dl, w->previewTex, p0, p1, splitX, dstX1, true);  // after: right
        dl->AddLine(ImVec2(splitX, p0.y + g.dst.y), ImVec2(splitX, p0.y + g.dst.y + g.dst.h),
                    IM_COL32(255, 255, 255, 200), 1.5f);
        tag("Before", dstX0, splitX, p0.y);
        tag("After", splitX, dstX1, p0.y);
    } else {  // AfterOnly (default), or Split before the before frame arrives
        DrawLetterboxed(dl, w->previewTex, p0, p1, 0, 0, false);
    }
}

// The scopes strip below the preview: waveform | histogram | vectorscope, each a GPU
// texture drawn with AddImage (the same GPU path as the preview).
void DrawScopesStrip(WindowImpl* w) {
    ImGui::Spacing();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float h = 140.0f;
    const float gap = 8.0f;
    // Vectorscope is square (h x h); the remaining width splits between waveform + histogram.
    const float squareW = h;
    float wide = (avail.x - squareW - gap * 2.0f) * 0.5f;
    if (wide < 40.0f) wide = 40.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 base = ImGui::GetCursorScreenPos();

    auto panel = [&](const GpuTex& t, const char* label, float x, float wpx) {
        ImVec2 a(x, base.y), b(x + wpx, base.y + h);
        dl->AddRectFilled(a, b, IM_COL32(16, 16, 20, 255));
        dl->AddRect(a, b, IM_COL32(70, 70, 80, 255));
        if (t.ready()) {
            FitRect fit = letterboxFit(wpx, h, t.w, t.h);
            ImVec2 mn(a.x + fit.x, a.y + fit.y), mx(mn.x + fit.w, mn.y + fit.h);
            dl->AddImage(reinterpret_cast<ImTextureID>(t.srv), mn, mx);
        }
        dl->AddText(ImVec2(a.x + 4, a.y + 2), IM_COL32(180, 180, 190, 255), label);
    };
    panel(w->waveTex, "Waveform", base.x, wide);
    panel(w->histTex, "Histogram", base.x + wide + gap, wide);
    panel(w->vecTex, "Vectorscope", base.x + wide * 2 + gap * 2, squareW);
    ImGui::Dummy(ImVec2(avail.x, h));
}

// --- the per-window UI content (the editor's controls) ----------------------

const char* const kFootageNames[] = {"Rec.709 (standard)", "V-Log"};
const char* const kThemeNames[] = {"Teal-Orange", "Warm-Film", "Cool-Noir"};
const char* const kLutSourceNames[] = {"Auto (Theme + Analysis)", "Embedded (Teal-Orange)",
                                       "External .cube file"};

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

    ImGui::TextUnformatted("Color Grade");
    ImGui::SameLine();
    ImGui::TextDisabled("(editor - Correct / Grade)");
    ImGui::Separator();

    // Two-column layout: controls left, live preview (Phase 4 placeholder) right.
    const float leftW = 340.0f;
    ImGui::BeginChild("controls", ImVec2(leftW, 0), true);

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Correct")) {
            ImGui::TextWrapped("Footage log format. V-Log decodes to Rec.709 before the "
                               "grade - applied live (this is an effect; no separate "
                               "Apply step).");
            ImGui::Spacing();
            // --- Footage profile: writes EditField::FootageProfile (1-based, CG_FOOT_*) ---
            int footIdx = ui.footageProfile - 1;
            if (footIdx < 0) footIdx = 0;
            if (footIdx > 1) footIdx = 1;
            if (ImGui::Combo("Footage", &footIdx, kFootageNames, 2)) {
                ui.footageProfile = footIdx + 1;
                w->edits.push({EditField::FootageProfile, static_cast<double>(ui.footageProfile)});
            }
            ImGui::Spacing();
            // In-effect multi-frame analysis (Phase 5): "Analyze" forces a re-run; the
            // idle hook also auto-analyses when the footage profile changes. The status
            // reflects the effect's live analysis driver.
            if (ImGui::Button("Analyze clip")) w->analyzeRequested.store(true);
            ImGui::SameLine();
            AnalysisStatus st = w->readStatus();
            if (st.state == AnalysisStatus::State::Sampling) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Analyzing %d/%d...",
                                   st.sampled, st.total);
            } else if (st.state == AnalysisStatus::State::Analyzed) {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
                                   st.fromCache ? "Analyzed (cached)" : "Analyzed");
            } else {
                ImGui::TextDisabled("Not analyzed");
            }
            ImGui::Spacing();
            ImGui::TextWrapped("Analysis samples several frames across the clip and adapts "
                               "the Auto grade to the whole shot.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Grade")) {
            // --- Theme popup: writes EditField::Theme (1-based to match CG_THEME_*) ---
            int themeIdx = ui.theme - 1;
            if (themeIdx < 0) themeIdx = 0;
            if (themeIdx > 2) themeIdx = 2;
            if (ImGui::Combo("Theme", &themeIdx, kThemeNames, 3)) {
                ui.theme = themeIdx + 1;
                w->edits.push({EditField::Theme, static_cast<double>(ui.theme)});
            }

            // --- Strength slider (fraction shown as percent) ---
            float strengthPct = static_cast<float>(fractionToPercent(ui.strength));
            if (ImGui::SliderFloat("Strength", &strengthPct, 0.0f, 100.0f, "%.0f%%")) {
                ui.strength = clamp01(percentToFraction(strengthPct));
                w->edits.push({EditField::Strength, ui.strength});
            }

            float skinPct = static_cast<float>(fractionToPercent(ui.skinProtection));
            if (ImGui::SliderFloat("Skin Protection", &skinPct, 0.0f, 100.0f, "%.0f%%")) {
                ui.skinProtection = clamp01(percentToFraction(skinPct));
                w->edits.push({EditField::SkinProtection, ui.skinProtection});
            }

            float chromaPct = static_cast<float>(fractionToPercent(ui.chromaGain));
            if (ImGui::SliderFloat("Chroma Gain", &chromaPct, 0.0f, 200.0f, "%.0f%%")) {
                ui.chromaGain = clampChromaFraction(percentToFraction(chromaPct));
                w->edits.push({EditField::ChromaGain, ui.chromaGain});
            }

            // --- LUT source popup ---
            int srcIdx = ui.lutSource - 1;
            if (srcIdx < 0) srcIdx = 0;
            if (srcIdx > 2) srcIdx = 2;
            if (ImGui::Combo("LUT Source", &srcIdx, kLutSourceNames, 3)) {
                ui.lutSource = srcIdx + 1;
                w->edits.push({EditField::LutSource, static_cast<double>(ui.lutSource)});
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right pane: a compare-mode + scopes toolbar, then the live preview, then the scopes.
    ImGui::BeginChild("viewer", ImVec2(0, 0), true);
    DrawViewerToolbar(w);
    DrawPreviewPane(w);
    if (w->showScopes.load()) DrawScopesStrip(w);
    ImGui::EndChild();

    ImGui::End();
}

void RunWindowThread(WindowImpl* w, ParamSnapshot seed) {
    ImGui_ImplWin32_EnableDpiAwareness();
    EnsureWindowClass();

    HWND hwnd = ::CreateWindowExW(0, kWindowClass, L"Color Grade - Editor",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 560,
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
    ImGui::StyleColorsDark();
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
    return it != g_windows.end() && it->second->compareMode.load() != 0;
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
