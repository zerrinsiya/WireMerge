#pragma once
#include <string>
#include <memory>
#include <functional>

// ---------------------------------------------------------------------------
// layout.h
//
// A small tiling layout engine, replacing ImGui's default floating windows
// for WireMerge's main panes (Output / Inputs / Active Sources / Log).
//
// Why this exists instead of ImGui::Begin() windows with position hints:
// floating ImGui windows have their own independent drag (via title bar)
// and resize (via corner grips) behavior, and nothing pushes neighboring
// windows around when one is resized -- that's fundamentally a different
// interaction model than what was asked for here (fixed slots, splitter-
// style resize where a neighbor grows/shrinks to match, and swapping two
// panes' positions by dragging one onto another). Rather than fight
// ImGui's window system into faking that, this implements an actual small
// tiling tree, the same structural idea used by tools like i3 or VS
// Code's panel splits:
//
//   - A Node is either a Split (horizontal or vertical, holding two
//     children and a ratio 0..1) or a Leaf (holding one pane's string ID).
//   - Each frame, the tree is walked top-down starting from the window's
//     current content rect, recursively dividing it according to each
//     split's orientation and ratio -- this is what makes panes resize to
//     fit the main window automatically (2.1) with no special-casing.
//   - Between two children of a split, a thin draggable strip lets the
//     user drag the divider, adjusting that split's ratio live (2.2,
//     confirmed VS-Code-style: growing one pane shrinks its sibling).
//   - Each leaf renders a small header bar (drag source AND drop target)
//     above its content; dragging one leaf's header onto another leaf
//     swaps which pane ID occupies which leaf (2.2's second interaction).
//     Content itself renders inside a plain ImGui::BeginChild with no
//     border-drag/resize-grip behavior of its own -- all resizing goes
//     through the splitter strips, not the child window.
// ---------------------------------------------------------------------------

namespace wm {

enum class SplitOrientation { Horizontal, Vertical };

struct LayoutNode;
using LayoutNodePtr = std::unique_ptr<LayoutNode>;

struct LayoutNode {
    bool isLeaf = true;

    // --- Leaf fields ---
    std::string paneId; // matches a key passed into TilingLayout::RenderPane

    // --- Split fields ---
    SplitOrientation orientation = SplitOrientation::Horizontal;
    float ratio = 0.5f; // 0..1, how much of the split the first child gets
    LayoutNodePtr first;
    LayoutNodePtr second;
};

struct PaneRenderContext {
    float x, y, width, height; // content rect, already excludes this pane's header bar
};

class TilingLayout {
public:
    // Builds the fixed initial 2x2 arrangement: (Output | Inputs) on top,
    // (Active Sources | Log) on bottom, both splits at ratio 0.5. This is
    // the starting layout; splitter drags and pane swaps mutate it live
    // from there for the rest of the session (not persisted across
    // restarts -- see gui.cpp's TODO note on config persistence).
    static LayoutNodePtr BuildDefaultLayout();

    // Call once per frame with the available content region (usually the
    // full window client area). Walks the tree, draws splitter strips,
    // and for each leaf calls renderFn(paneId, rect) so the caller can
    // render that pane's actual content (device dropdowns, buttons, etc)
    // -- this keeps layout math completely separate from panel content,
    // which lives in gui.cpp as before.
    //
    // displayNameFn maps a pane ID to the human-readable header text
    // shown in that leaf's drag handle (e.g. "output" -> "Output Device").
    static void Render(LayoutNode& root, float x, float y, float width, float height,
                        const std::function<void(const std::string& paneId, const PaneRenderContext&)>& renderFn,
                        const std::function<std::string(const std::string& paneId)>& displayNameFn);

private:
    // Returns true if a swap was performed this frame (root may have been
    // mutated -- specifically, two leaves' paneId fields swapped).
    static bool RenderNode(LayoutNode& node, float x, float y, float width, float height,
                            const std::function<void(const std::string&, const PaneRenderContext&)>& renderFn,
                            const std::function<std::string(const std::string&)>& displayNameFn);
};

} // namespace wm
