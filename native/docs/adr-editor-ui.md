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
  effect param (`CG_FOOTAGE_PROFILE`). It drives a native **decode stage** that applies
  in **every** LUT Source mode (captain directive - never leave V-Log footage
  undecoded): pipeline order is always decode-then-LUT. Auto composes `grade(decode(x))`
  into one LUT (continuous, no resample); Embedded/External resample their raw LUT
  through the decode (`rawLut(decode(x))`) - both keep the per-pixel apply a single
  trilinear sample so CPU and GPU stay identical. Rec.709 decodes to itself (no-op).
  Both compositions are proven bit-exact / within 3.3e-7 of the TS oracle by the
  `gradedecode` and `lutdecode` cases in `npm run native:core-parity`. (Analyze/scopes
  stay Phase 5.)
- **Grade tab:** Theme popup, Strength / Skin Protection / Chroma Gain sliders, LUT
  Source popup - all round-trip through the bridge to their params.
- **Every control** reads (via `publishSnapshot`) and writes (via `drainEdits` ->
  `CHANGED_VALUE`) the effect's params. The headless bridge test covers the new
  `FootageProfile` field; all four build configs (Debug/Release x CPU/`--gpu`) compile
  clean.

## The effect <-> window bridge (design)

Two directions over the pure `EditorBridge.h` seam:

- **effect -> window** (`publishSnapshot`): a `ParamSnapshot` of the current param
  values. Published two ways: opportunistically from `PreRender` (a locked value copy,
  safe from any render thread), and - the reliable path - from the **idle hook**, which
  reads the effect's current param streams via AEGP (`AEGP_GetNewStreamValue`) and
  publishes on change so an Effect-Controls scrub/type tracks in the window sub-second.
  The idle-hook poll is what makes EC->window work: `PreRender`'s publish can't be
  relied on because `in_data->sequence_data` (and thus the window key) is unreliable on
  render threads. A monotonic `revision` + the window's mid-drag guard
  (`IsAnyItemActive`) keep a stale publish or a poll from stomping a control the user is
  actively dragging.
- **window -> effect** (`drainEdits`): the window pushes `ParamEdit`s on its UI
  thread; they are drained on AE's main thread and written back onto the effect's
  params (with `PF_ChangeFlag_CHANGED_VALUE`, so AE re-renders and Effect Controls
  updates). The queue **coalesces per field** so a fast drag is one write per changed
  field, not one per pixel of travel.

**Write driver (implemented; captain-verified in AE):** the effect registers a global
**AEGP idle hook** via PICA - `AEGP_RegisterSuite5::AEGP_RegisterIdleHook`, no separate
AEGP plugin/PiPL needed - after `AEGP_RegisterWithAEGP` gives it a plugin id. On AE's
main thread the hook drains each open window's queue and writes each value onto the
effect's param streams (`AEGP_StreamSuite5::AEGP_GetNewEffectStreamByIndex` +
`AEGP_SetStreamValue`), wrapped in **one** `AEGP_StartUndoGroup`/`EndUndoGroup` per
tick so a gesture is a single undo step. It reaches the right instance through a
per-key **`AEGP_EffectRefH`** captured on a main-thread command (button-open) via
`AEGP_PFInterfaceSuite1::AEGP_GetNewEffectForEffect`, disposed on close/setdown. The
hook **cheap-noops** when no window has pending edits, so it never burdens AE. The
in-`UserChangedParam` `CHANGED_VALUE` drain remains as a secondary flush. All AEGP
calls are wrapped in `catch(...)`, so on a host without AEGP (e.g. Premiere) the driver
is simply absent rather than fatal.

**Instance key (fixed):** `in_data->effect_ref` is **not stable across command types**
(it differs between `USER_CHANGED_PARAM` and `SMART_PRE_RENDER`), which is why the
first cut's publish/drain targeted the wrong/no window and the round-trip was dead in
live AE. The key is now a uid stored in **sequence data** (flat POD, reseeded on
SETUP/RESETUP so a duplicated/reloaded effect never shares a key), consistent across
every command for one instance.

## Phase 4: live clip preview (built on this toolkit)

The editor window now shows a **live preview of the actual clip frame**, centered and
letterboxed, updating as the captain scrubs the timeline or changes any effect param.
This is the payoff the ImGui choice was made for (axis 3): a checked-out GPU-renderable
frame drawn with near-zero impedance.

**Frame source - the decode invariant for free.** The effect's global AEGP idle hook
(the same one that drives the window<->effect bridge) checks out the layer frame
**downstream of this effect** via
`AEGP_LayerRenderOptionsSuite2::AEGP_NewFromDownstreamOfEffect` +
`AEGP_RenderSuite5::AEGP_RenderAndCheckoutLayerFrame`. "Downstream of effect" = the
layer output *including* our effect, so the checked-out pixels are already decoded +
graded. V-Log is therefore **never left undecoded** in the preview under any LUT Source -
the invariant holds by construction, with no second decode path to keep in sync. The
options handle is seeded to the layer's current time, which we read back
(`AEGP_GetTime`) as the scrub position - no separate comp-time lookup needed.
`AEGP_NewFromDownstreamOfEffect` is **UI-thread only**; the idle hook satisfies that.

**Display.** The 8-bit ARGB receipt world is copied (ARGB->RGBA, decimated so the long
side is <= 960 px) into a CPU `PreviewFrame`, published to the window, and uploaded on
the window's own UI thread into a per-window D3D11 texture
(`DXGI_FORMAT_R8G8B8A8_UNORM`), drawn with `ImDrawList::AddImage` centered + letterboxed
(`letterboxFit`). AE worlds are premultiplied and a footage-clip preview is opaque, so
the copy forces alpha = 255 (partial-alpha layers look slightly off - accepted for v1).

**Refresh + caching = interactivity.** The idle hook computes a `PreviewKey` = (frame
time + a fingerprint of the grade-affecting params, read from the same stream poll that
feeds the controls). A pure state machine (`decidePreviewAction`) decides: window already
current (do nothing), a **cached** CPU frame matches (publish it - the interactive
scrub-back path, no AE render), or a fresh render is needed. A bounded per-instance LRU
cache (16 frames) of decoded frames keeps scrubbing interactive; the render is
**synchronous** on the idle thread, which is sanctioned here precisely because the cache
makes repeats free and only genuinely new time/param states pay a render. **Every**
checkout is checked back in unconditionally (a `ScopedCheckin` RAII guard that fires even
if the pixel copy throws) - a leaked receipt destabilizes AE, so this is load-bearing.

**Testable seam.** All the risky logic - cache keying/eviction, the scrub/param-change
decision, the letterbox fit math, and the once-and-always check-in guarantee - lives in
the pure, AE-free `editor/PreviewCache.h` and is proven headlessly by
`npm run native:preview-test` (`native/tests/editor/preview_test.cpp`, g++/clang, NOT in
CI). The AE-side integration (the AEGP checkout + D3D upload) is captain-verified.

**Known limitations / deferred.** (a) Sync UI-thread checkout is flagged in the SDK as
deprecated for *passive* redraws in favor of `AEGP_RenderAndCheckoutLayerFrame_Async`;
async (with in-flight request cancellation on fast scrub) is the documented future
refinement if interactivity needs it - the state machine is already shaped for one
in-flight request. (b) The cache fingerprint keys on time + *our* params; an unrelated
change (another effect, a source swap without a time change) can leave the preview stale
until the next scrub/param nudge. (c) Scopes + before/after are Phase 5.

## Phase 5: in-effect analysis + live scopes + before/after (built on this toolkit)

Phase 5 moves clip **analysis into the effect** (no panel-era plumbing) and adds two pro
feedback surfaces to the editor window - **live scopes** and a **before/after** compare.

**In-effect multi-frame analysis.** The Auto grade adapts to the real clip by measuring
its source stats in-effect. The idle hook (main thread - `AEGP_NewFromUpstreamOfEffect` is
UI-thread-only, exactly like the Phase 4 downstream checkout) samples several frames evenly
across the layer's duration UPSTREAM of this effect (the RAW footage), decodes each to
Rec.709 via the footage profile - the **same** `decodePixelToRec709` the Auto bake composes,
so measured stats and applied grade agree and **V-Log is never analysed as raw log** - and
runs the ported `cg::core::computeStats` over the union. Work is **incremental: one upstream
checkout per idle tick**, so a multi-frame job never stalls AE and never runs on the editor
window's D3D thread. Every checkout is checked back in via `ScopedCheckin` (Phase 4's leak
discipline). The measured stats live in a process-global keyed by instance
(`g_analyzedStats`, mutex-guarded: idle hook writes, render threads read) and are injected
over the recipe's `sourceStats` at render (`InjectAnalyzedStats`), only while the footage
profile still matches what was analysed. Completion calls **`PF_TouchActiveItem`** (PF
AdvItemSuite) to force the active comp to re-render so the new grade + preview appear.
Persisting the analysed stats into the arb-data (project save/reload) is deferred with the
rest of grade-recipe persistence (issues #11/#12) - re-analysis re-runs cheaply on open.

**Rebake on knob change / debounce.** Analysis re-runs only when an *analysis-relevant*
input changes - the footage profile or the clip span (the grade knobs change the LUT, not
the measured source, so a slider drag never triggers a checkout). A change is **debounced**
(`AnalysisDebounce`, N stable idle ticks) so toggling the footage popup mid-interaction
doesn't thrash multi-frame checkouts, and a small per-instance **results cache** (fingerprint
-> stats) makes re-selecting a previously-analysed profile instant. An explicit **Analyze**
button forces a fresh run.

**Live scopes.** Waveform, histogram, and (cheap) vectorscope are computed on the window's
UI thread from the **same graded preview frame** the editor already shows (downstream =
decoded+graded, so the decode invariant holds for scopes too), synthesised into small RGBA8
images (`Scopes.h`) and drawn as GPU textures with `ImDrawList::AddImage` - the same GPU path
as the preview, so "GPU-drawn" falls out for free. They refresh whenever a new preview frame
arrives.

**Before/after.** A toolbar toggles **After / Before / Split**. "Before" is the decoded
ORIGINAL (the upstream frame decoded to Rec.709 - **never raw log** when Footage=V-Log),
checked out + decoded on the idle hook only when the compare mode needs it
(`wantsBeforeFrame`), cached by (time + footage). Split view draws the before frame clipped
left of a draggable divider and the after frame right of it, both letterboxed to the shared
clip rect (`splitViewGeometry`); the divider position is a UI slider.

**Testable seams.** All the risky pure logic - the frame-sampling schedule, the incremental
job state machine, the analysis fingerprint + debounce (`editor/Analysis.h`), the scope
binning + image synthesis (`editor/Scopes.h`), and the split-view geometry
(`editor/PreviewCache.h`) - is host-agnostic and proven headlessly by
`npm run native:analysis-test` and `npm run native:scopes-test` (g++/clang, NOT in CI), plus
the existing preview/bridge tests. The AEGP checkout + `PF_TouchActiveItem` + D3D uploads are
captain-verified in AE. All four build configs (Debug/Release x CPU/`--gpu`) compile clean.

**Known limitations / deferred.** (a) `PF_TouchActiveItem` re-renders the *active* item, so
analysis-completion refresh assumes the analysed comp is frontmost (the normal grading flow);
(b) analysed stats are not yet persisted to the project (deferred with grade-recipe
persistence); (c) the multi-instance limitation from Phase 4 (two CG effects on one layer)
carries over; (d) sync upstream checkout, like the Phase 4 preview, could move to the async
render variant if analysis latency ever needs it.

**Captain round-1 verification (functional PASS)** landed two fixes, both AE-verified next
round: (1) the Split radio and the split-position slider shared the label "Split" and so
shared an ImGui ID (debug overlay) - the slider now uses a unique `##splitpos` id, and
`io.ConfigDebugHighlightIdConflicts` is kept ON in Debug so any future clash surfaces; (2) the
scopes drew at their source size centered in wide boxes (dead margins) with the vectorscope
clipped off the window edge - the strip now lays out three EQUAL columns from the live content
region every frame, waveform/histogram fill their box interior, the vectorscope stays 1:1
square + centered, and each plot is hard-clipped to its box (correct windowed / fullscreen /
live-resize). **Deferred - dedicated UI-polish pass:** the captain wants the whole editor
restyled (prettier/sleeker) once all feature phases land (with/after Phase 6); do NOT restyle
piecemeal before then - this is a tracked follow-up, not a Phase 5 gap.

## Phase 6a: manual grade suite - Basics (built on this toolkit)

Phase 6a adds a **manual primary-correction stage** to the engine, ahead of the theme
stages, on the decoded gamma-Rec.709 signal (the V-Log decode invariant holds by
construction - manual only ever sees decoded footage). Design proposal + captain
decisions: `firstmate/data/grade-suite-design/{report.md,decisions.md}`.

- **Engine (oracle-first, then ported bit-exact):** `ManualGrade` (exposure, contrast +
  pivot, highlights/shadows/whites/blacks via the feathered `bandWeights`, temperature/tint
  as LAB a/b bias, saturation as LAB chroma multiply, vibrance reusing the engine falloff)
  and a `lookMix` blend live in `src/core/engine/engine.ts` `buildTransform`, ported to
  `native/ColorGradeFX/core/Engine.h`. Each control is neutral-gated so a neutral value is
  exact identity. A new **"None / Manual" theme** (`matchStats: false`) turns off the
  stat-match look so manual grading is the whole grade with no stat-match staleness; with a
  neutral manual grade it takes an identity fast path (clean identity LUT). Gated bit-exact
  by `npm run native:core-parity` (manual/lookMix/None cases added; ~3e-7).
- **Keyframeable params (decision D1):** `CG_EXPOSURE`, `CG_LOOK_MIX`, `CG_TEMPERATURE` are
  appended AFTER `CG_RECIPE` (append-only - AE stores param values by index). Their live
  values override the recipe's stored exposure/temperature and drive Look Mix at bake time
  (`BakeAutoLut`). Everything else is recipe-backed editor state. Strength dilutes the whole
  grade incl. manual (D3); skin protection applies as-is.
- **Recipe migration (the 6a landmine):** `RecipeData` grew a `matchStats` flag + the manual
  block + `lookMix`, all APPENDED after the v2 fields, and `RECIPE_VERSION` bumped 2->3. The
  arb-data UNFLATTEN handler now MIGRATES instead of reseeding: `migrateRecipeInto`
  (`core/Recipe.h`, the single source of truth, self-tested by the parity `migrate` case)
  copies a v2 blob's prefix over v3 defaults and re-stamps the version, so old saved grades
  survive; only a foreign/corrupt blob reseeds.
- **Editor Basics tab + bridge:** the window's **Basics** tab has all the sliders. The three
  keyframeable scalars round-trip as scalar streams (existing AEGP one_d path); the 9
  recipe-backed controls round-trip through the `CG_RECIPE` **arb** stream - the idle hook
  read-modify-writes it (`AEGP_GetNewStreamValue` -> `HandleSuite1->host_lock_handle` mutate
  -> `AEGP_SetStreamValue`), and reads it back in the poll (`ReadRecipeViaAegp`). The pure
  `ManualState` + `EditQueue` coalescing (now copies the whole edit) are headless-tested by
  `npm run native:editor-test`. `previewParamFingerprint` folds the 3 scalars + a hash of the
  whole recipe blob so a manual edit never serves a stale preview (`native:preview-test`).
- **Grade LUT grid stays 33 (measured, not bumped):** a 65-point grade bake is ~8x slower
  (~80-95ms vs ~10-13ms; measured with an aggressive manual grade), which is not affordable
  for the keyframeable params that re-bake per animated frame. 33-point banding on extreme
  manual grades (~3% worst case on a smooth ramp) matches the regime the shipping themes
  already bake at. An adaptive grid (65 only for static aggressive grades) is a follow-up.
- **Out of scope (separately tracked):** curves UI (6b), wheels (6c), secondary hue curves
  (6d), the AI agent loop, the UI-polish overhaul.

## Consequences

- **Positive:** single-file signed `.aex` stays intact; one UI codebase Win+Mac; the
  preview/scopes work in Phases 4-5 sits in the toolkit built for it; lowest-latency
  grade loop; no JS VM / webview runtime inside an AE render process.
- **Negative:** `src/panel` Preact controls are not reused as code (the design is).
  Immediate-mode UI is less "designer-friendly" than HTML/CSS for heavy visual
  polish - acceptable for a pro color tool, revisit if that changes.
- **Follow-ups:** Metal/Cocoa backend for the Mac target; scopes + before/after
  (Phase 5); async checkout if scrub interactivity needs it. (The idle-hook write
  driver, sequence-data uid, and the full Correct/Grade control set landed in Phase 3;
  the live clip preview landed in Phase 4 - see the Phase 4 section above.)

## Known limitations

- **SEQUENCE_SETDOWN closes the editor window.** On `PF_Cmd_SEQUENCE_SETDOWN` the
  effect closes the open editor window and disposes its `AEGP_EffectRefH`. AE issues
  `SEQUENCE_SETDOWN` not only on true instance removal (delete effect, close comp) but
  also around sequence-data **flatten/reload** cycles. For the current flows this is
  captain-verified fine - ordinary flatten / RAM-preview cycles do **not** dismiss the
  window. But it is a latent edge: if a future change makes setdown fire during an
  ordinary flatten/RAM-preview cycle for a still-live instance, the open editor would
  be dismissed out from under the user. Revisit (e.g. defer teardown, or re-open on the
  matching RESETUP) if that surfaces.

- **Effect deletion is detected by the idle hook re-enumerating the layer, not by
  SEQUENCE_SETDOWN and not by touching the effect ref.** Phase 4 captain verification
  found that deleting the effect while its editor window is open does **not** reliably
  reach `SEQUENCE_SETDOWN` in time (the window stayed open), leaving the idle hook to use
  a now-stale effect ref. Any call against that stale ref crashes AE with an uncatchable
  modal: `AEGP_GetNewStreamValue` on the collapsed stream raised *"...NO_DATA streams"
  (5027::247)* (round 1), and even `AEGP_GetNewEffectStreamByIndex` / `AEGP_GetStreamType`
  by index into the collapsed stream group raised *"invalid index in indexed group"* then
  a hard crash (round 2). **Conclusion: never touch ANY stream of a possibly-stale ref.**
  Final design: the idle hook keeps **no AEGP handles at all** - effect ref, layer handle,
  AND comp handle all go stale across delete/undo. At button-open it captures only **stable
  ids**: the parent comp's **project-item id** (`AEGP_GetItemFromComp` + `AEGP_GetItemID`),
  the layer's **layer-ID** (`AEGP_GetLayerID`), and our **installed-effect key**. Each tick
  `ResolveLiveEffect` re-resolves everything from those ids and returns a **tri-state**:
  (1) **comp membership** - re-resolve the current comp handle by walking the *live project's*
  item list (`AEGP_GetFirstProjItem`/`AEGP_GetNextProjItem`) for our item id
  (`ResolveCompByItemID`); (2) **layer membership** - `AEGP_GetLayerFromLayerID` within that
  comp; (3) **effect match** - enumerate that layer's effects by installed key. -> **Alive**
  (use the fresh ref for read/write/render, disposed at end of pass); live comp+layer but no
  effect match -> **ConfirmedGone** (effect deleted: close promptly); comp or layer not a live
  member, or partial context -> **CannotVerify**.

  **Why membership, not call-failure (rounds 4-5):** deleting a layer *or a whole comp* moves
  it into AE's *undo buffer* as a "zombie" - a cached `AEGP_LayerH`/`AEGP_CompH` keeps
  resolving against it, so a "failed-enumeration" counter never fires and the window lingers.
  The fix is to re-resolve from the *live* project/comp tree by id: a deleted comp is not in
  the project item list, a deleted layer (or one whose source footage was removed) is not in
  the comp's layer list, and an **undo** restores the id and hands back the *current* handle -
  so the write path re-derives the live effect and never writes onto a stale ref (that stale
  write was the intermittent "internal verification" modal on edit-after-undo). A single
  `CannotVerify` never force-closes (transient guard); a persistent run closes at
  `CG_VERIFY_FAIL_LIMIT` (~0.8 s). While not `Alive`, the hook also **drops any edits the
  orphaned window queued** (writes happen only in the `Alive` branch, on the fresh ref). A
  defensive `NO_DATA` type-check remains in `ReadStreamOneD`.

  **Exactly one window per effect:** the registry is keyed on the sequence-data uid, but a
  delete+undo can reseed that uid (SEQUENCE_RESETUP), which would let a fresh Open Editor
  spawn a second window while a stale orphan lingers. So Open Editor first calls
  `CloseDuplicateWindowsForEffect`: it closes any window whose captured (comp-item id, layer
  id) matches the effect being opened but under a *different* key, then opens/adopts the
  current key - a fresh open replaces the orphan rather than fighting it.

  **Multi-instance limitation:** two CG effects on one layer -> `ResolveLiveEffect` matches the
  *first* by installed key (no per-instance id available from the idle hook); acceptable and
  non-crashing; revisit if per-instance targeting is needed.

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
7. **Live preview (Phase 4):** the window shows the actual clip frame, centered +
   letterboxed. Scrubbing the timeline updates it; changing any grade param (in the
   window or Effect Controls) re-renders it; scrubbing back to a visited time/param
   state serves instantly from the cache. On a V-Log clip in any LUT Source mode the
   preview is decoded (not washed-out log).
8. **Deletion lifecycle (Phase 4):** with a window open, deleting the effect closes the
   window promptly (no modal, no crash); undo restores the effect and re-opening works;
   deleting the layer or comp, and swapping/removing the source footage, also close the
   window without a stale-ref modal.
9. **In-effect analysis (Phase 5):** on an ungraded clip in Auto LUT-Source, the Correct
   tab shows "Analyzing n/N..." then "Analyzed" shortly after opening; the graded look
   visibly shifts to adapt to the clip (vs the neutral placeholder). Clicking **Analyze**
   re-runs it. Switching Footage Rec.709 <-> V-Log re-analyses (debounced, no thrash on a
   fast toggle); switching back shows "Analyzed (cached)" instantly. AE stays responsive
   throughout (no multi-second stall) - analysis is one checkout per idle tick.
10. **Analysis is post-decode (Phase 5):** on a V-Log clip, the analysed/adapted grade is
    sensible (measured on decoded Rec.709), never driven by raw-log stats.
11. **Live scopes (Phase 5):** waveform / histogram / vectorscope appear below the preview
    and update as you scrub or change a grade param; toggling **Scopes** hides/shows them.
    A neutral clip centres the vectorscope; pushing chroma spreads it outward.
12. **Before/after (Phase 5):** the **After / Before / Split** toggle works; Before shows
    the decoded original (on V-Log, decoded - not washed-out log), After the graded result,
    Split shows both across a draggable divider, all correctly letterboxed. The comp/preview
    updates after analysis completes (active comp re-renders).

### Phase 6a (manual grade - Basics)

Rebuild from THIS worktree with AE closed, then load the fresh `.aex` (all 4 configs build
clean; CUDA is the engaged GPU path on the dev box). New Effect Controls params appear at the
end: **Exposure**, **Look Mix**, **Temperature**. The editor gains a **Basics** tab and the
Theme popup a **None (Manual)** entry.

13. **Each control moves the preview:** with LUT Source = Auto, in the Basics tab, dragging
    each of Exposure / Contrast / Contrast Pivot / Highlights / Shadows / Whites / Blacks /
    Temperature / Tint / Saturation / Vibrance / Look Mix visibly changes the preview AND the
    comp, live (no need to touch Effect Controls). The 9 recipe-backed sliders round-trip via
    the arb stream; Exposure/Look Mix/Temperature also move their Effect Controls sliders.
14. **Neutral = identity:** with theme **None (Manual)** and every Basics control at neutral
    (Exposure 0, Contrast 0, region sliders 0, Temperature/Tint 0, Saturation 100%, Vibrance
    0, Look Mix 100%), the output is visually identical to the effect disabled (an identity
    grade). Nudging any single control away from neutral and back returns to identity.
15. **Exposure keyframes ramp:** keyframe CG_EXPOSURE from -2 to +2 stops across the clip;
    playback shows a smooth exposure ramp (the grade re-bakes per animated frame; 33-point
    bake keeps this real-time). Confirm no banding regression on a smooth gradient/sky.
16. **Strength dilutes manual (D3):** with a strong manual grade on None (Manual), lowering
    the Grade tab **Strength** toward 0 fades the whole manual grade back toward the original
    footage (manual is not Strength-immune).
17. **Old-version grades survive load (the migration):** open a project SAVED WITH THE
    PRE-6a build (v2 recipe) that has a non-default grade (e.g. a theme + adjusted
    Strength/Chroma). It must load with that grade intact (NOT reset to default), and the new
    Basics controls default to neutral. (If no pre-6a project exists, this is covered by the
    `migrate` parity self-test; note that in the report.)
18. **V-Log decode invariant:** on a V-Log clip with Footage = V-Log, manual controls act on
    the decoded (corrected) image - e.g. Saturation/Temperature look natural, never like
    they are operating on raw washed-out log.
