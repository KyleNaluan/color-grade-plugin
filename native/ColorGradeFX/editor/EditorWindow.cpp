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

#include <cmath>

#include <vector>

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
        // Unique ID: the label must differ from the "Split" radio above, or ImGui flags an
        // ID conflict. "##splitpos" hides the label text (the adjacent radio names it) while
        // giving the slider its own id. The leading text keeps context visible.
        ImGui::TextUnformatted("pos");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("##splitpos", &w->splitFraction, 0.0f, 1.0f, "%.2f");
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
            ImGui::TextWrapped("Camera log format. A log profile decodes to Rec.709 before "
                               "the grade - applied live (this is an effect; no separate "
                               "Apply step). Standard (Rec.709) = no decode.");
            ImGui::Spacing();
            // --- Footage profile as a Camera->Profile cascade (decision doc): both combos
            //     derive from core/FootageCatalog.h and resolve to the flat 1-based popup
            //     index the effect stores. Picking a camera auto-selects its Standard so the
            //     Profile dropdown is never empty; the whole cascade writes the same
            //     EditField::FootageProfile the AE flat popup does. ---
            cg::core::FootageCascadePos pos = cg::core::footageCascadePosForFlat(ui.footageProfile);
            std::vector<std::string> cams = cg::core::footageCameras();
            std::vector<const char*> camPtrs;
            camPtrs.reserve(cams.size());
            for (const auto& c : cams) camPtrs.push_back(c.c_str());
            int camIdx = pos.cameraIndex;
            if (ImGui::Combo("Camera", &camIdx, camPtrs.data(), static_cast<int>(camPtrs.size()))) {
                // New camera -> auto-select that camera's Standard (Rec.709) profile.
                cg::core::FootageCameraProfiles p = cg::core::footageProfilesForCamera(cams[camIdx]);
                ui.footageProfile = p.flatIndices.front();
                w->edits.push({EditField::FootageProfile, static_cast<double>(ui.footageProfile)});
            }
            // Profile dropdown, filtered to the selected camera (Standard first).
            cg::core::FootageCameraProfiles profs = cg::core::footageProfilesForCamera(cams[camIdx]);
            std::vector<const char*> profPtrs;
            profPtrs.reserve(profs.labels.size());
            for (const auto& l : profs.labels) profPtrs.push_back(l.c_str());
            int optIdx = 0;
            for (size_t i = 0; i < profs.flatIndices.size(); ++i) {
                if (profs.flatIndices[i] == ui.footageProfile) {
                    optIdx = static_cast<int>(i);
                    break;
                }
            }
            if (ImGui::Combo("Profile", &optIdx, profPtrs.data(), static_cast<int>(profPtrs.size()))) {
                ui.footageProfile = profs.flatIndices[static_cast<size_t>(optIdx)];
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
        if (ImGui::BeginTabItem("Basics")) {
            // Manual primary correction (Phase 6a). Exposure/Look Mix/Temperature are
            // keyframeable PF scalar params (written as scalar edits); the rest are the
            // recipe-backed ManualState (pushed as one coalesced Manual edit). Applied
            // ahead of the theme look; neutral = no change. Requires LUT Source = Auto.
            auto pushManual = [w](ParamSnapshot& s) {
                ParamEdit e;
                e.field = EditField::Manual;
                e.manual = s.manual;
                w->edits.push(e);
            };
            ImGui::TextWrapped("Manual primary correction, applied before the theme look. "
                               "Exposure, Look Mix and Temperature are keyframeable in Effect "
                               "Controls. Uses LUT Source = Auto.");
            ImGui::Spacing();

            float exposure = static_cast<float>(ui.exposure);
            if (ImGui::SliderFloat("Exposure", &exposure, -5.0f, 5.0f, "%.2f stops")) {
                ui.exposure = exposure;
                w->edits.push({EditField::Exposure, ui.exposure});
            }
            float contrast = static_cast<float>(ui.manual.contrast);
            if (ImGui::SliderFloat("Contrast", &contrast, -100.0f, 100.0f, "%.0f")) {
                ui.manual.contrast = contrast;
                pushManual(ui);
            }
            float pivot = static_cast<float>(ui.manual.pivot);
            if (ImGui::SliderFloat("Contrast Pivot", &pivot, 0.05f, 0.95f, "%.3f")) {
                ui.manual.pivot = pivot;
                pushManual(ui);
            }
            ImGui::Separator();
            float highlights = static_cast<float>(ui.manual.highlights);
            if (ImGui::SliderFloat("Highlights", &highlights, -100.0f, 100.0f, "%.0f")) {
                ui.manual.highlights = highlights;
                pushManual(ui);
            }
            float shadows = static_cast<float>(ui.manual.shadows);
            if (ImGui::SliderFloat("Shadows", &shadows, -100.0f, 100.0f, "%.0f")) {
                ui.manual.shadows = shadows;
                pushManual(ui);
            }
            float whites = static_cast<float>(ui.manual.whites);
            if (ImGui::SliderFloat("Whites", &whites, -100.0f, 100.0f, "%.0f")) {
                ui.manual.whites = whites;
                pushManual(ui);
            }
            float blacks = static_cast<float>(ui.manual.blacks);
            if (ImGui::SliderFloat("Blacks", &blacks, -100.0f, 100.0f, "%.0f")) {
                ui.manual.blacks = blacks;
                pushManual(ui);
            }
            ImGui::Separator();
            float temperature = static_cast<float>(ui.temperature);
            if (ImGui::SliderFloat("Temperature", &temperature, -100.0f, 100.0f, "%.0f")) {
                ui.temperature = temperature;
                w->edits.push({EditField::Temperature, ui.temperature});
            }
            float tint = static_cast<float>(ui.manual.tint);
            if (ImGui::SliderFloat("Tint", &tint, -100.0f, 100.0f, "%.0f")) {
                ui.manual.tint = tint;
                pushManual(ui);
            }
            float satPct = static_cast<float>(ui.manual.saturation * 100.0);
            if (ImGui::SliderFloat("Saturation", &satPct, 0.0f, 200.0f, "%.0f%%")) {
                ui.manual.saturation = satPct / 100.0;
                pushManual(ui);
            }
            float vibrance = static_cast<float>(ui.manual.vibrance);
            if (ImGui::SliderFloat("Vibrance", &vibrance, -100.0f, 100.0f, "%.0f")) {
                ui.manual.vibrance = vibrance;
                pushManual(ui);
            }
            ImGui::Separator();
            float lookMixPct = static_cast<float>(ui.lookMix * 100.0);
            if (ImGui::SliderFloat("Look Mix", &lookMixPct, 0.0f, 100.0f, "%.0f%%")) {
                ui.lookMix = clamp01(lookMixPct / 100.0);
                w->edits.push({EditField::LookMix, ui.lookMix});
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Grade")) {
            // --- Theme popup: writes EditField::Theme (1-based to match CG_THEME_*) ---
            int themeIdx = ui.theme - 1;
            if (themeIdx < 0) themeIdx = 0;
            if (themeIdx > kThemeCount - 1) themeIdx = kThemeCount - 1;
            if (ImGui::Combo("Theme", &themeIdx, kThemeNames, kThemeCount)) {
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
        if (ImGui::BeginTabItem("Curves")) {
            // Master + per-channel monotone tone curves (Phase 6b), recipe-backed and
            // baked with the theme look. Wired into the existing engine curve fields;
            // no engine math is added. Requires LUT Source = Auto.
            ImGui::TextWrapped("Tone curves. Left-drag a point to move, left-click to add, "
                               "right-click to remove. Reset clears the curve. Uses LUT "
                               "Source = Auto.");
            ImGui::Spacing();
            const float plot = 148.0f;
            struct CurveEntry {
                const char* label;
                cg::editor::CurveState* c;
                int idx;
                ImU32 col;
            };
            const CurveEntry curves[4] = {
                {"Master", &ui.curves.master, 0, IM_COL32(235, 235, 240, 255)},
                {"Red", &ui.curves.r, 1, IM_COL32(235, 90, 90, 255)},
                {"Green", &ui.curves.g, 2, IM_COL32(90, 210, 110, 255)},
                {"Blue", &ui.curves.b, 3, IM_COL32(105, 150, 240, 255)},
            };
            bool curvesChanged = false;
            for (int i = 0; i < 4; ++i) {
                ImGui::BeginGroup();
                ImGui::TextUnformatted(curves[i].label);
                std::string plotId = std::string("##curve") + std::to_string(i);
                if (DrawCurveWidget(plotId.c_str(), *curves[i].c, w->curveDrag[curves[i].idx],
                                    curves[i].col, plot)) {
                    curvesChanged = true;
                }
                std::string resetId = std::string("Reset##curve") + std::to_string(i);
                if (ImGui::SmallButton(resetId.c_str())) {
                    *curves[i].c = cg::editor::CurveState{};
                    w->curveDrag[curves[i].idx] = -1;
                    curvesChanged = true;
                }
                ImGui::EndGroup();
                if (i % 2 == 0) ImGui::SameLine();
            }
            if (curvesChanged) {
                ParamEdit e;
                e.field = EditField::Curves;
                e.curves = ui.curves;
                w->edits.push(e);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Wheels")) {
            // DaVinci Lift/Gamma/Gain wheels (primary) + Adobe-style 3-way (secondary).
            // LGG is a new engine stage; the 3-way reuses the LGG masters for luminance
            // and the band LAB tint fields for color - no extra engine math. Uses Auto.
            bool wheelsChanged = false;
            int mode = ui.wheels.mode;
            ImGui::TextUnformatted("Wheels:");
            ImGui::SameLine();
            if (ImGui::RadioButton("Lift/Gamma/Gain", mode == 0)) mode = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("3-Way", mode == 1)) mode = 1;
            if (mode != ui.wheels.mode) {
                ui.wheels.mode = mode;
                wheelsChanged = true;
            }
            ImGui::Separator();

            const float radius = 46.0f;
            if (mode == 0) {
                ImGui::TextWrapped("Lift moves blacks, Gain moves whites, Gamma bends mids. "
                                   "Drag the disc for color balance; the slider is luminance.");
                ImGui::Spacing();
                struct LggWheel {
                    const char* name;
                    double* triple;
                    double neutral;
                    double colorK;
                    float mMin, mMax;
                    const char* fmt;
                };
                const LggWheel wheels[3] = {
                    {"Lift", ui.wheels.lift, 0.0, 0.20, -0.5f, 0.5f, "%.3f"},
                    {"Gamma", ui.wheels.gamma, 1.0, 0.60, 0.2f, 3.0f, "%.2f"},
                    {"Gain", ui.wheels.gain, 1.0, 0.40, 0.0f, 2.0f, "%.2f"},
                };
                for (int i = 0; i < 3; ++i) {
                    const LggWheel& wl = wheels[i];
                    ImGui::BeginGroup();
                    ImGui::TextUnformatted(wl.name);
                    std::string discId = std::string("##lgg") + std::to_string(i);
                    if (DrawWheelDisc(discId.c_str(), wl.triple, wl.colorK, radius))
                        wheelsChanged = true;
                    ImGui::SetNextItemWidth(radius * 2.0f);
                    float m = static_cast<float>(WheelMaster(wl.triple));
                    std::string mId = std::string("##lggm") + std::to_string(i);
                    if (ImGui::SliderFloat(mId.c_str(), &m, wl.mMin, wl.mMax, wl.fmt)) {
                        WheelSetMaster(wl.triple, m);
                        wheelsChanged = true;
                    }
                    std::string rId = std::string("Reset##lgg") + std::to_string(i);
                    if (ImGui::SmallButton(rId.c_str())) {
                        wl.triple[0] = wl.triple[1] = wl.triple[2] = wl.neutral;
                        wheelsChanged = true;
                    }
                    ImGui::EndGroup();
                    if (i < 2) ImGui::SameLine();
                }
            } else {
                ImGui::TextWrapped("Adobe-style 3-way: the disc tints each tonal band; the "
                                   "slider pushes that band's luminance. Reuses the band tints "
                                   "and the Lift/Gamma/Gain luminance.");
                ImGui::Spacing();
                // Each band: color disc -> LAB tint field; luminance slider -> LGG master.
                struct ThreeWay {
                    const char* name;
                    double* ab;         // the band tint [a,b]
                    bool* has;          // the tint presence flag
                    double* triple;     // the LGG triple carrying this band's luminance
                    double neutral;     // luminance neutral for that triple
                    float mMin, mMax;
                    const char* fmt;
                };
                ThreeWay bands[3] = {
                    {"Shadows", ui.wheels.shadowTint, &ui.wheels.hasShadowTint, ui.wheels.lift,
                     0.0, -0.5f, 0.5f, "%.3f"},
                    {"Midtones", ui.wheels.midTint, &ui.wheels.hasMidTint, ui.wheels.gamma,
                     1.0, 0.2f, 3.0f, "%.2f"},
                    {"Highlights", ui.wheels.highTint, &ui.wheels.hasHighTint, ui.wheels.gain,
                     1.0, 0.0f, 2.0f, "%.2f"},
                };
                for (int i = 0; i < 3; ++i) {
                    ThreeWay& bd = bands[i];
                    ImGui::BeginGroup();
                    ImGui::TextUnformatted(bd.name);
                    std::string discId = std::string("##tw") + std::to_string(i);
                    if (DrawTintDisc(discId.c_str(), bd.ab, 25.0, radius)) {
                        *bd.has = (bd.ab[0] != 0.0 || bd.ab[1] != 0.0);
                        wheelsChanged = true;
                    }
                    ImGui::SetNextItemWidth(radius * 2.0f);
                    float m = static_cast<float>(WheelMaster(bd.triple));
                    std::string mId = std::string("##twm") + std::to_string(i);
                    if (ImGui::SliderFloat(mId.c_str(), &m, bd.mMin, bd.mMax, bd.fmt)) {
                        WheelSetMaster(bd.triple, m);
                        wheelsChanged = true;
                    }
                    std::string rId = std::string("Reset##tw") + std::to_string(i);
                    if (ImGui::SmallButton(rId.c_str())) {
                        bd.ab[0] = bd.ab[1] = 0.0;
                        *bd.has = false;
                        bd.triple[0] = bd.triple[1] = bd.triple[2] = bd.neutral;
                        wheelsChanged = true;
                    }
                    ImGui::EndGroup();
                    if (i < 2) ImGui::SameLine();
                }
            }
            if (wheelsChanged) {
                ParamEdit e;
                e.field = EditField::Wheels;
                e.wheels = ui.wheels;
                w->edits.push(e);
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
#ifdef _DEBUG
    // Keep ImGui's duplicate-ID detector ON in Debug so any future same-label widget
    // clash surfaces immediately (captain round-1 finding: Split radio vs split slider).
    // Default is already true in this ImGui (1.91.5); set explicitly so it never regresses.
    io.ConfigDebugHighlightIdConflicts = true;
#endif
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
