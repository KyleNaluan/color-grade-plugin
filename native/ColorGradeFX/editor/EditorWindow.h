/*
 * EditorWindow.h - the native editor-window host the effect talks to.
 *
 * This is the Phase 3 SPIKE of the leading UI-toolkit candidate (Dear ImGui + a
 * Win32/D3D11 window; see native/docs/adr-editor-ui.md). It owns:
 *   - a single-instance-per-effect registry (keyed by the effect instance's stable
 *     id from sequence data), so one effect never spawns two windows,
 *   - each window on its OWN UI thread (D3D11 device + swapchain + ImGui loop), so
 *     AE's threads are never blocked by the editor,
 *   - the bridge endpoints: publishSnapshot (effect -> window, so controls mirror
 *     Effect Controls) and drainEdits (window -> effect, applied on AE's main
 *     thread via the AEGP StreamSuite), reusing the pure EditorBridge.h seam.
 *
 * Deliberately AE-SDK-free (only <cstdint>/STL + Win32 in the .cpp): the effect
 * includes this, not the reverse, keeping the host coupling one-directional. The
 * whole plugin is Windows-only today; the macOS port swaps the .cpp backend
 * (Cocoa + Metal) behind this same interface (ADR "cross-platform" section).
 */
#pragma once
#ifndef CG_EDITOR_WINDOW_H
#define CG_EDITOR_WINDOW_H

#include <cstdint>
#include <memory>
#include <vector>

#include "EditorBridge.h"

namespace cg {
namespace editor {

// Opaque per-effect-instance key (the uid stored in the effect's sequence data).
using InstanceKey = uint64_t;

class EditorWindow;  // impl detail (EditorWindow.cpp)

// Process-wide registry + lifecycle owner. All methods are safe to call from AE's
// threads; each is a no-op on non-Windows builds (the .cpp compiles empty there).
class EditorHost {
public:
    static EditorHost& instance();

    // Open the editor for `key`, seeded with the effect's current params. If a
    // window is already open for `key`, brings it to the foreground instead of
    // spawning a second one (single instance per effect).
    void open(InstanceKey key, const ParamSnapshot& seed);

    // Effect -> window: publish the effect's latest param values so the window's
    // controls reflect Effect Controls. Ignored if no window is open for `key`.
    void publishSnapshot(InstanceKey key, const ParamSnapshot& snapshot);

    // Window -> effect: take the edits the user made since the last drain (empty if
    // none / no window). The caller applies them to the effect's params.
    std::vector<ParamEdit> drainEdits(InstanceKey key);

    // The keys of all currently-open windows (for the idle-hook driver to iterate).
    std::vector<InstanceKey> openKeys();

    // True if the window for `key` has edits waiting - the idle hook's cheap fast-path
    // check so it does no AEGP work when nothing changed.
    bool hasPendingEdits(InstanceKey key);

    // Did the user request the window be closed (clicked its close box)? Lets the
    // effect release a window that the user dismissed, without a per-instance hook.
    bool consumeCloseRequest(InstanceKey key);

    bool isOpen(InstanceKey key);

    // Close the window for `key` (effect instance removed / sequence setdown).
    void close(InstanceKey key);

    // Close every open window. Called at GLOBAL_SETDOWN so AE process exit / project
    // close never leaves an orphan editor window behind.
    void shutdownAll();

private:
    EditorHost() = default;
    ~EditorHost();
    EditorHost(const EditorHost&) = delete;
    EditorHost& operator=(const EditorHost&) = delete;
};

}  // namespace editor
}  // namespace cg

#endif  // CG_EDITOR_WINDOW_H
