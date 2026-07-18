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

// Forward-decl of the backend's Win32 message handler (defined in imgui_impl_win32.cpp).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

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

// One window instance: its own thread, D3D11 device/swapchain, ImGui context, and
// the bridge state shared with the effect (snapshot in, edits out).
struct WindowImpl {
    InstanceKey key = 0;

    std::thread          thread;
    std::atomic<bool>    running{false};    // false asks the UI loop to exit
    std::atomic<bool>    finished{false};   // true once the thread fully tore down
    std::atomic<bool>    closeRequested{false};  // user clicked the window's close box

    // Bridge: effect -> window (snapshot) guarded; window -> effect (queue) is
    // itself thread-safe. seededTheme etc. let the UI init from the effect's state.
    std::mutex     snapMutex;
    ParamSnapshot  snapshot;   // latest published effect params
    EditQueue      edits;      // pending user edits for the effect to drain

    HWND                    hwnd = nullptr;
    ID3D11Device*           device = nullptr;
    ID3D11DeviceContext*    context = nullptr;
    IDXGISwapChain*         swapChain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;

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
    sd.OutputWindow = w->hwnd;
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
            ImGui::BeginDisabled();
            ImGui::Button("Analyze frame");  // in-effect analysis: Phase 5
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(analysis: Phase 5)");
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

    // Live preview placeholder (Phase 4: AEGP_RenderAndCheckoutLayerFrame -> GPU texture).
    ImGui::BeginChild("preview", ImVec2(0, 0), true);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(avail.x, avail.y - 28.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin();
    ImVec2 p1 = ImGui::GetItemRectMax();
    dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 28, 255));
    const char* label = "Live preview (Phase 4)  -  scopes / before-after (Phase 5)";
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f),
                IM_COL32(150, 150, 160, 255), label);
    ImGui::EndChild();

    ImGui::End();
}

void RunWindowThread(WindowImpl* w, ParamSnapshot seed) {
    ImGui_ImplWin32_EnableDpiAwareness();
    EnsureWindowClass();

    w->hwnd = ::CreateWindowExW(0, kWindowClass, L"Color Grade - Editor",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 560,
                                nullptr, nullptr, PluginInstance(), nullptr);
    if (!w->hwnd) { w->finished.store(true); return; }
    ::SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));

    if (!CreateDeviceD3D(w)) {
        ::DestroyWindow(w->hwnd);
        w->hwnd = nullptr;
        w->finished.store(true);
        return;
    }

    ::ShowWindow(w->hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(w->hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't litter the user's disk with imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(w->hwnd);
    ImGui_ImplDX11_Init(w->device, w->context);

    // The UI's working copy of the params; starts from the seed the effect passed.
    ParamSnapshot ui = seed;
    { std::lock_guard<std::mutex> lk(w->snapMutex); w->snapshot = seed; }

    w->running.store(true);
    const float clear[4] = {0.10f, 0.10f, 0.11f, 1.0f};
    while (w->running.load()) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) w->running.store(false);
        }
        if (!w->running.load()) break;

        // Adopt a newer snapshot from the effect for any control the user is NOT
        // currently editing (ImGui's active-id guards against clobbering a drag).
        ParamSnapshot latest = w->readSnapshot();
        if (latest.revision > ui.revision && !ImGui::IsAnyItemActive()) ui = latest;

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

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D(w);
    if (w->hwnd) { ::DestroyWindow(w->hwnd); w->hwnd = nullptr; }
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
    w->running.store(false);
    if (w->hwnd) ::PostMessageW(w->hwnd, WM_CLOSE, 0, 0);  // nudge the loop out
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
        if (it->second->hwnd) {
            ::ShowWindow(it->second->hwnd, SW_RESTORE);
            ::SetForegroundWindow(it->second->hwnd);
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

std::vector<ParamEdit> EditorHost::drainEdits(InstanceKey key) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    ReapFinishedLocked();
    auto it = g_windows.find(key);
    if (it != g_windows.end()) return it->second->edits.drain();
    return {};
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
std::vector<ParamEdit> EditorHost::drainEdits(InstanceKey) { return {}; }
std::vector<InstanceKey> EditorHost::openKeys() { return {}; }
bool EditorHost::hasPendingEdits(InstanceKey) { return false; }
bool EditorHost::consumeCloseRequest(InstanceKey) { return false; }
bool EditorHost::isOpen(InstanceKey) { return false; }
void EditorHost::close(InstanceKey) {}
void EditorHost::shutdownAll() {}

}  // namespace editor
}  // namespace cg

#endif  // _WIN32
