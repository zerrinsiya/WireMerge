#include "layout.h"
#include "imgui.h"
#include <algorithm>
#include <cstring>

namespace wm {

// Minimum fraction of a split either child is allowed to shrink to --
// without this, dragging a splitter to an extreme could collapse a pane
// to zero/negative size.
static constexpr float kMinRatio = 0.12f;
static constexpr float kMaxRatio = 0.88f;
static constexpr float kSplitterThickness = 6.0f;
static constexpr float kHeaderHeight = 26.0f;

LayoutNodePtr TilingLayout::BuildDefaultLayout() {
    auto output = std::make_unique<LayoutNode>();
    output->isLeaf = true;
    output->paneId = "output";

    auto sources = std::make_unique<LayoutNode>();
    sources->isLeaf = true;
    sources->paneId = "sources";

    auto leftColumn = std::make_unique<LayoutNode>();
    leftColumn->isLeaf = false;
    leftColumn->orientation = SplitOrientation::Vertical; // stacked top/bottom
    leftColumn->ratio = 0.45f;
    leftColumn->first = std::move(output);
    leftColumn->second = std::move(sources);

    auto inputs = std::make_unique<LayoutNode>();
    inputs->isLeaf = true;
    inputs->paneId = "inputs";

    auto log = std::make_unique<LayoutNode>();
    log->isLeaf = true;
    log->paneId = "log";

    auto rightColumn = std::make_unique<LayoutNode>();
    rightColumn->isLeaf = false;
    rightColumn->orientation = SplitOrientation::Vertical;
    rightColumn->ratio = 0.65f;
    rightColumn->first = std::move(inputs);
    rightColumn->second = std::move(log);

    auto root = std::make_unique<LayoutNode>();
    root->isLeaf = false;
    root->orientation = SplitOrientation::Horizontal; // side by side left/right
    root->ratio = 0.5f;
    root->first = std::move(leftColumn);
    root->second = std::move(rightColumn);

    return root;
}

void TilingLayout::Render(LayoutNode& root, float x, float y, float width, float height,
                           const std::function<void(const std::string&, const PaneRenderContext&)>& renderFn,
                           const std::function<std::string(const std::string&)>& displayNameFn) {
    RenderNode(root, x, y, width, height, renderFn, displayNameFn);
}

bool TilingLayout::RenderNode(LayoutNode& node, float x, float y, float width, float height,
                               const std::function<void(const std::string&, const PaneRenderContext&)>& renderFn,
                               const std::function<std::string(const std::string&)>& displayNameFn) {
    if (node.isLeaf) {
        // Unique ImGui ID scope per leaf (by pane ID, not by tree position)
        // so widget state doesn't get confused if two leaves swap paneIds
        // -- IDs follow the pane's identity, not its current slot.
        ImGui::PushID(node.paneId.c_str());

        ImGui::SetCursorScreenPos(ImVec2(x, y));

        // --- Header bar: drag source AND drop target for pane swapping ---
        std::string headerLabel = displayNameFn(node.paneId);
        ImGui::Selectable(headerLabel.c_str(), false, ImGuiSelectableFlags_None,
                           ImVec2(width, kHeaderHeight));

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            LayoutNode* selfPtr = &node;
            ImGui::SetDragDropPayload("WM_PANE_SWAP", &selfPtr, sizeof(LayoutNode*));
            ImGui::TextUnformatted(headerLabel.c_str());
            ImGui::EndDragDropSource();
        }

        bool swapped = false;
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("WM_PANE_SWAP")) {
                LayoutNode* source = *static_cast<LayoutNode**>(payload->Data);
                if (source && source != &node) {
                    std::swap(source->paneId, node.paneId);
                    swapped = true;
                }
            }
            ImGui::EndDragDropTarget();
        }

        // --- Content area ---
        ImGui::SetCursorScreenPos(ImVec2(x, y + kHeaderHeight));
        std::string childId = std::string("##content_") + node.paneId;
        float contentHeight = std::max(0.0f, height - kHeaderHeight);
        if (ImGui::BeginChild(childId.c_str(), ImVec2(width, contentHeight),
                               ImGuiChildFlags_Border)) {
            PaneRenderContext ctx{x, y + kHeaderHeight, width, contentHeight};
            // If a swap just happened, node.paneId now refers to a
            // DIFFERENT pane than what was dragged FROM this slot -- skip
            // rendering stale content this one frame rather than call
            // renderFn with a paneId that doesn't match what the header
            // just displayed; next frame picks up the new pane cleanly.
            if (!swapped) {
                renderFn(node.paneId, ctx);
            }
        }
        ImGui::EndChild();

        ImGui::PopID();
        return swapped;
    }

    // --- Split node: divide the rect, render both children, draw a
    // draggable divider between them ---
    bool horizontal = (node.orientation == SplitOrientation::Horizontal);
    float totalLength = horizontal ? width : height;
    float firstLength = std::max(0.0f, totalLength * node.ratio - kSplitterThickness * 0.5f);
    float secondLength = std::max(0.0f, totalLength - firstLength - kSplitterThickness);

    float firstX = x, firstY = y, firstW = width, firstH = height;
    float secondX = x, secondY = y, secondW = width, secondH = height;
    float splitterX, splitterY, splitterW, splitterH;

    if (horizontal) {
        firstW = firstLength;
        secondX = x + firstLength + kSplitterThickness;
        secondW = secondLength;
        splitterX = x + firstLength;
        splitterY = y;
        splitterW = kSplitterThickness;
        splitterH = height;
    } else {
        firstH = firstLength;
        secondY = y + firstLength + kSplitterThickness;
        secondH = secondLength;
        splitterX = x;
        splitterY = y + firstLength;
        splitterW = width;
        splitterH = kSplitterThickness;
    }

    bool swappedInFirst = RenderNode(*node.first, firstX, firstY, firstW, firstH, renderFn, displayNameFn);

    // --- Splitter drag handle ---
    ImGui::PushID(&node); // split nodes have no paneId, so identity is the node's address
    ImGui::SetCursorScreenPos(ImVec2(splitterX, splitterY));
    ImGui::InvisibleButton("##splitter", ImVec2(splitterW, splitterH));

    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(horizontal ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive()) {
        float delta = horizontal ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
        if (totalLength > 1.0f) {
            node.ratio += delta / totalLength;
            node.ratio = std::clamp(node.ratio, kMinRatio, kMaxRatio);
        }
    }

    // Subtle visual so the splitter is discoverable without looking like a
    // hard corporate divider line -- drawn only while hovered/active,
    // invisible otherwise so it doesn't clutter the flat/HTML-ish look.
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImU32 col = ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
        drawList->AddRectFilled(ImVec2(splitterX, splitterY),
                                 ImVec2(splitterX + splitterW, splitterY + splitterH), col, 3.0f);
    }
    ImGui::PopID();

    bool swappedInSecond = RenderNode(*node.second, secondX, secondY, secondW, secondH, renderFn, displayNameFn);

    return swappedInFirst || swappedInSecond;
}

} // namespace wm
