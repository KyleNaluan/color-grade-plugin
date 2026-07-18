# ADR: Editor-window UI toolkit - native (Dear ImGui) vs embedded webview

- **Status:** ACCEPTED - captain approved native Dear ImGui + Win32/D3D11 at the
  Phase 3 checkpoint; the full Correct/Grade control set was then built on it (below).
- **Phase:** Native re-platform Phase 3 (editor window shell + effect<->window bridge).
- **Decision owner:** captain (this is the deliberate product-direction call Phase 3 owns).
- **Author:** crewmate (native-phase3 lane).

This ADR records the toolkit evaluation, the recommendation, and the de-risking
spike that backs it. It exists so the captain can approve a direction **before** the
full Correct/Grade control set is built on either technology (building that set is
gated on this decision).

---

## Context

Phase 1-2 landed the effect spine: a SmartFX effect (`ColorGradeFX.aex`) with the
full C++ core, a param surface (Theme / Strength / Skin / Chroma / LUT-Source), and
an arb-data grade recipe (`native/ColorGradeFX/`). Phase 3 adds the **editor window**:
a button in Effect Controls opens a native OS window that hosts the Correct/Grade
controls (and, in Phases 4-5, a **live clip preview** and **scopes**). The AE SDK
gives no large editor surface inside Effect Controls, so - exactly like Magic Bullet
Looks / FilmConvert Nitrate - the plugin opens **its own OS window** and draws it
itself (`native-scope-m2/report.md`, Area 3).

The open technology question this phase owns: **what draws that window's UI?**

The window is not a form-filling panel. What it must do well is the thing Phases 4-5
add: show a **GPU frame** (the corrected clip, checked out via
`AEGP_RenderAndCheckoutLayerFrame`) live in the center, refreshing as the user
scrubs/grades, with **waveform / vectorscope / histogram** scopes drawn from that
same frame. The controls are the cheap part; the live GPU preview + scopes are the
expensive part, and they should drive the toolkit choice.

## Decision drivers (the axes the brief names)

1. **Dependency weight / packaging** - Phase 6 ships a *free, public, signed `.aex`
   drop-in*: ideally a single file users drop into `.../MediaCore/`. Runtime
   dependencies and shipped DLL sprawl are a direct cost here.
2. **Windows-first, macOS-eventually** - v1 is Windows; the Mac `.plugin` follows.
   A single UI codebase across both is worth a lot.
3. **Dev velocity for Phase 5's scopes/preview UI** - how cheaply can we get a GPU
   texture (the checked-out frame) on screen and draw custom real-time scopes?
4. **Input latency** - the grade loop (drag a knob -> preview updates) must feel
   immediate.

## Options considered

| Option | What it is |
|---|---|
| **A. Dear ImGui + D3D11 (Win) / Metal (Mac)** | Immediate-mode GUI compiled into the plugin; we own the HWND/swapchain. |
| B. Qt (or wxWidgets) | Retained-mode cross-platform toolkit, dynamically linked. |
| C. Raw Win32 + Cocoa | OS APIs directly, no toolkit. |
| **D. Embedded webview + Preact** | WebView2 (Win) / WKWebView (Mac) hosting the existing `src/panel` Preact UI. |

### Evaluation

**1. Dependency weight / packaging**
- **A (ImGui): best.** ~6 vendored `.cpp` files (MIT), compiled straight into the
  `.aex`. Zero external runtime dependency - D3D11/DXGI/d3dcompiler/dwmapi are all
  OS-provided. The drop-in stays a single file. Binary grows ~1-2 MB.
- B (Qt): worst. Multi-hundred-MB SDK; ships `Qt6Core/Gui/Widgets` DLLs + a
  `platforms/` plugin folder next to the `.aex`; LGPL relink/source-offer duties.
  A "single-file drop-in" becomes "an `.aex` + a DLL bundle" - fragile for a free
  public release.
- C (raw): zero dependency, like A, but pays it in hand-rolled code (see axis 3).
- **D (webview): asymmetric and backwards for Windows-first.** WKWebView is
  OS-provided on Mac (cheap), but on **Windows** WebView2 needs the Evergreen
  runtime (bundled on current Win11, **not guaranteed on Win10**) or shipping the
  Fixed-Version runtime (~150-180 MB) beside the `.aex`. Either a runtime dependency
  or a large packaging bloat - on the platform we ship *first*. Plus the Preact
  bundle must be embedded/served.

**2. Windows-first, macOS-eventually**
- **A: best single-source story.** One UI codebase; swap only the rendering +
  platform backend (D3D11+Win32 -> Metal+Cocoa, both are stock ImGui backends). The
  control code is identical across platforms.
- B: genuinely one codebase, but the packaging cost (axis 1) applies on both.
- C: **two** full UI codebases (Win32 + Cocoa). Worst.
- D: the HTML/Preact UI is shared, but the **native shell + bridge is doubled**
  (WebView2 vs WKWebView are different host APIs with different frame-delivery
  paths). Middle.

**3. Dev velocity for scopes/preview (the decisive axis)**
- **A: purpose-built for this.** The live preview is a GPU texture drawn with
  `ImGui::Image` - and we already have the frame on the GPU from the CUDA/DirectX
  render path. Scopes are custom immediate-mode draws (`ImDrawList`, or a small
  compute shader we already have the toolchain for). Near-zero impedance between "I
  have a GPU texture" and "show it." This is the standard home for real-time
  GPU-drawn color tools.
- B: can host a D3D/GL surface (QRhi / QOpenGLWidget), but compositing our GPU
  texture into a Qt scene and drawing scopes via QPainter/QRhi carries real
  impedance. Workable, heavier.
- C: full control, but we hand-roll the swapchain **and** every widget, layout,
  DPI, and theming. Most code, slowest.
- **D: the hard case.** Getting a low-latency GPU frame into an HTML surface means
  either (a) read the frame back to CPU and re-upload it as an `ImageBitmap`/canvas
  texture every refresh (latency + bandwidth), or (b) a WebView2 shared-texture /
  composition path that is complex and Windows-specific. The scoping report flags
  "getting a low-latency GPU frame into an HTML surface is the hard part" as a top
  risk. Reusing Preact controls is nice, but webview makes exactly the
  expensive part (preview + scopes) hardest.

**4. Input latency**
- **A / C: lowest.** Direct OS events -> immediate redraw; a knob drag updates the
  preview in the same frame.
- B: retained-mode, still native events; fine, slightly more overhead.
- **D: highest.** Input -> webview event loop -> IPC to native (update param +
  trigger checkout) -> frame back across the bridge to the canvas. Multiple hops on
  the interactive path.

### The Preact-reuse argument (webview's one real win)

The scoping report calls `src/panel` "throwaway as code; the UI *design* and
orchestration *logic* are the spec." Webview salvages the *controls code*. But:
- The controls are a modest slice (`src/panel` is 670 LOC total, much of it CEP
  analyze->bake->apply orchestration that the native param model *replaces*, not
  reuses).
- The genuinely reusable asset is the **design/UX spec**, which transfers to any
  toolkit (the ImGui spike already reproduces the Correct/Grade tab layout).
- Against that modest reuse, webview takes the **worst** outcome on the two axes
  that dominate Phases 4-5 (preview/scopes velocity, input latency) **and** the
  worst Windows packaging story.

## Decision (recommended)

**Adopt Option A: Dear ImGui, compiled into the plugin, D3D11 backend on Windows
(Metal on macOS when the Mac target opens).**

Rationale: it wins dependency-weight/packaging (zero runtime dep, single-file
drop-in), wins the dev-velocity axis that actually matters (GPU-texture preview +
immediate-mode scopes are native to it), wins input latency, and gives one
cross-platform UI codebase. Its only real cost - losing Preact reuse - is small and
the design spec carries over regardless.

Rejected: **Qt** (packaging weight + LGPL DLL sprawl for a free drop-in); **raw
Win32/Cocoa** (two hand-rolled UI codebases); **webview** (worst preview latency and
Windows packaging for a marginal controls-reuse win).

**Keep in the back pocket:** revisit webview only if priorities shift from the live
grade/scope tool toward a marketing-grade polished control panel or JS-based theme
authoring - not the case for the v1 grade tool.

## Spike evidence (de-risking Option A)

Built in this branch, all **compile-verified in the `.aex` across the four build
configs** (Debug/Release x CPU/`--gpu`) via `native/scripts/build.sh`:

- **`native/third_party/imgui/`** - Dear ImGui v1.91.5 vendored (core + Win32/DX11
  backends, MIT). Compiles into the `.aex` with the project's flags (`NOMINMAX`,
  MultiByte, `stdcpp17`, `/MD[d]`) with no CRT/dependency conflict. Confirms the
  **packaging** claim: the drop-in stays one file; D3D11/DXGI/d3dcompiler/dwmapi
  auto-link from OS SDK (backend `#pragma comment(lib,...)`), XInput loads
  dynamically (no link dep).
- **`native/ColorGradeFX/editor/EditorWindow.{h,cpp}`** - the window host: a
  single-instance-per-effect registry, each window on its own UI thread (D3D11
  device + swapchain + ImGui loop, WARP fallback), lifecycle (open/focus, close-box
  teardown, `shutdownAll` at GLOBAL_SETDOWN, per-instance close at SEQUENCE_SETDOWN).
  Renders the Correct/Grade tab layout with the live-preview + scopes area stubbed -
  proving the **UI velocity** claim (the control layout is ~40 lines of immediate-mode
  code) without building the full set.
- **`native/ColorGradeFX/editor/EditorBridge.h`** - the pure effect<->window seam
  (thread-safe coalescing edit queue, percent<->fraction mapping, `applyEdit`),
  **headless-tested** by `npm run native:editor-test`
  (`native/tests/editor/bridge_test.cpp`): FIFO + coalescing, concurrent
  push/drain loses no edit, clamps hold, and a drained edit round-trips back into the
  snapshot. Proves the **bridge logic** without a running AE.
- **Effect wiring** (`ColorGrade.cpp`): an "Open Editor" **button param** opens the
  window; `PreRender` publishes the current params so the window mirrors Effect
  Controls; `UserChangedParam` drains editor edits back onto the params.

What the spike deliberately did **not** do at the checkpoint (gated on the decision):
the full control set, the live preview + scopes (Phases 4-5), and the production
continuous write driver (below).

### Post-approval: the full Correct/Grade control set (built on ImGui)

After the captain approved ImGui, the full control set landed on it:

- **Correct tab:** a **Footage** profile popup (Rec.709 / V-Log) - a real, persisted
  effect param (`CG_FOOTAGE_PROFILE`). It drives a native **decode stage**: the Auto
  bake composes `grade(decode(x))` into one LUT when the clip is a log profile, so a
  V-Log clip is decoded to Rec.709 *before* the grade, baked into a single LUT (CPU
  and GPU apply paths stay identical - no per-pixel decode in the kernel). This
  composition is proven bit-exact against the TS oracle by a new `gradedecode` case in
  `npm run native:core-parity`. (Analyze/scopes stay Phase 5; the raw Embedded/
  External LUT modes are unaffected by the decode stage, by design.)
- **Grade tab:** Theme popup, Strength / Skin Protection / Chroma Gain sliders, LUT
  Source popup - all round-trip through the bridge to their params.
- **Every control** reads (via `publishSnapshot`) and writes (via `drainEdits` ->
  `CHANGED_VALUE`) the effect's params. The headless bridge test covers the new
  `FootageProfile` field; all four build configs (Debug/Release x CPU/`--gpu`) compile
  clean.

## The effect <-> window bridge (design)

Two directions over the pure `EditorBridge.h` seam:

- **effect -> window** (`publishSnapshot`): a `ParamSnapshot` of the current param
  values, published from `PreRender`. A locked value copy, so it is safe from any
  render thread; a monotonic `revision` lets the window ignore a stale re-publish and
  not stomp a control the user is mid-drag on.
- **window -> effect** (`drainEdits`): the window pushes `ParamEdit`s on its UI
  thread; they are drained on AE's main thread and written back onto the effect's
  params (with `PF_ChangeFlag_CHANGED_VALUE`, so AE re-renders and Effect Controls
  updates). The queue **coalesces per field** so a fast drag is one write per changed
  field, not one per pixel of travel.

**Production write path (post-decision, captain-verified in AE):** a **companion
AEGP** plugin owns an `AEGP_RegisterIdleHook`, draining the queue on AE's main thread
and applying each edit via `AEGP_StreamSuite`/`AEGP_DynamicStreamSuite` set-value,
wrapped in **one** `AEGP_StartUndoGroup`/`EndUndoGroup` per gesture so a drag is a
single undo step. The effect reaches AEGP via `AEGP_PFInterfaceSuite`. The spike's
in-command drain (any param-change flushes the queue) stands in for the idle-hook
driver until the companion AEGP lands.

**Undo grouping behavior to document/verify:** editor writes should group as one
undo step per gesture (via the undo group above). Writes coming through
`CHANGED_VALUE` in the spike path currently ride AE's normal param-undo; the
companion-AEGP path makes the grouping explicit. This is on the AE-verification list.

**Instance key:** the spike keys the registry on `in_data->effect_ref` (stable for an
applied effect's lifetime within a session). Production hardening: store a generated
uid in **sequence data** so the key survives an `effect_ref` reuse across
delete/re-add and project reload.

## Consequences

- **Positive:** single-file signed `.aex` stays intact; one UI codebase Win+Mac; the
  preview/scopes work in Phases 4-5 sits in the toolkit built for it; lowest-latency
  grade loop; no JS VM / webview runtime inside an AE render process.
- **Negative:** `src/panel` Preact controls are not reused as code (the design is).
  Immediate-mode UI is less "designer-friendly" than HTML/CSS for heavy visual
  polish - acceptable for a pro color tool, revisit if that changes.
- **Follow-ups:** companion AEGP + idle-hook write driver; sequence-data instance
  uid; Metal/Cocoa backend for the Mac target; the full Correct/Grade control set +
  live preview + scopes (Phases 4-5).

## AE verification checklist (captain-assisted)

Runtime cannot be automated from WSL; verify in AE 2025 once the toolkit is approved
and the build is loaded:

1. **Button opens window:** Effect Controls shows **Open Editor…**; clicking it opens
   the editor window; AE stays responsive (window is on its own thread).
2. **Single instance:** clicking the button again focuses the existing window (no
   second window).
3. **Controls round-trip:** editing Footage (Correct) or
   Theme/Strength/Skin/Chroma/LUT-Source (Grade) in the window updates the
   corresponding Effect Controls param and re-renders the comp; changing a param in
   Effect Controls updates the window's control. Setting Footage = V-Log on a V-Log
   clip visibly corrects it (decode + grade) in the Auto LUT-Source mode.
4. **No dialogs / no hangs:** no modal/error dialogs; the window never blocks AE.
5. **Lifecycle:** closing the window's close-box leaves no orphan window and AE
   stays healthy; deleting the effect / closing the comp closes the window; quitting
   AE / closing the project leaves no orphan window.
6. **Undo:** an editor edit is undoable; document how it groups (one step per gesture
   once the companion-AEGP undo group lands).
