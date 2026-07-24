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
//   - Each leaf renders a small header bar above its content, labeling
//     the pane. Content itself renders inside a plain ImGui::BeginChild
//     with no border-drag/resize-grip behavior of its own -- all resizing
//     goes through the splitter strips, not the child window. (Pane
//     swap-by-drag was considered and explicitly dropped in the v0.6 UI
//     spec -- header bars are labels only now, not drag sources/targets.)
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

// Draws a subtle bordered/backed "card" frame at (x,y,width,height): a
// background fill, a thin border, and a label at the top -- with a real
// inset margin between that outer border and where content actually
// starts (this is the "subtle window" treatment requested repeatedly:
// every pane, and now also each Inputs subsection, should look like its
// own small bordered card with breathing room inside it, not content
// sitting flush against a border). Returns the inner content rect; the
// caller is responsible for BeginChild/EndChild (with NO border of its
// own -- this function already drew the one border, a second one would
// produce the "box inside a box" look reported earlier).
PaneRenderContext DrawSubtlePanelFrame(const std::string& label, float x, float y,
                                        float width, float height, float margin);

class TilingLayout {
public:
    // Builds the fixed initial 2x2 arrangement: (Output | Inputs) on top,
    // (Active Sources | Log) on bottom, both splits at ratio 0.5. This is
    // the starting layout; splitter drags and pane swaps mutate it live
    // from there for the rest of the session (not persisted across
    // restarts -- see gui.cpp's TODO note on config persistence).
    static LayoutNodePtr BuildDefaultLayout();

    // Finds the split node whose first or second child is the leaf with
    // the given paneId, or nullptr if not found / paneId is the root.
    // Used by item-5-style live corrections that need to nudge a
    // specific split's ratio (e.g. growing the Sources/Inputs split so
    // Active Sources doesn't need its own scrollbar) without the caller
    // needing to know the tree shape.
    static LayoutNode* FindPaneParent(LayoutNode& root, const std::string& paneId);

    // Call once per frame with the available content region (usually the
    // full window client area). Walks the tree, draws splitter strips,
    // and for each leaf calls renderFn(paneId, rect) so the caller can
    // render that pane's actual content (device dropdowns, buttons, etc)
    // -- this keeps layout math completely separate from panel content,
    // which lives in gui.cpp as before.
    //
    // displayNameFn maps a pane ID to the human-readable header text
    // shown in that leaf's header bar (e.g. "output" -> "Output Device").
    static void Render(LayoutNode& root, float x, float y, float width, float height,
                        const std::function<void(const std::string& paneId, const PaneRenderContext&)>& renderFn,
                        const std::function<std::string(const std::string& paneId)>& displayNameFn);

private:
    static void RenderNode(LayoutNode& node, float x, float y, float width, float height,
                            const std::function<void(const std::string&, const PaneRenderContext&)>& renderFn,
                            const std::function<std::string(const std::string&)>& displayNameFn);
};

} // namespace wm
