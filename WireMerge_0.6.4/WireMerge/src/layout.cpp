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
static constexpr float kPaneGap = 16.0f; // gap between adjacent panes -- unrelated to splitter thickness, left unchanged
static constexpr float kSplitterThickness = 10.0f; // the splitter's own hitbox/highlight thickness -- thinner than the gap, centered within it

PaneRenderContext DrawSubtlePanelFrame(const std::string& label, float x, float y,
                                        float width, float height, float margin) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 p0(x, y);
    ImVec2 p1(x + width, y + height);

    // Subtle background + border -- slightly lighter than the app
    // background (ChildBg) with a thin, muted border (Border), giving the
    // "its own small window" look without being visually loud.
    drawList->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_ChildBg), 4.0f);
    drawList->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), 4.0f, 0, 1.0f);

    // Title: scaled up slightly (15/13 ratio, per feedback -- "just
    // enough to differentiate") so it visibly reads as a title rather
    // than blending in with body text.
    constexpr float kTitleFontScale = 15.0f / 13.0f;
    ImGui::SetCursorScreenPos(ImVec2(x + margin, y + margin));
    ImGui::SetWindowFontScale(kTitleFontScale);
    ImGui::TextUnformatted(label.c_str());
    float labelHeight = ImGui::GetTextLineHeight();
    ImGui::SetWindowFontScale(1.0f);

    // Divider line beneath the title -- the "# creates a title divider,
    // like markdown" ask. Full margin's worth of gap between the title
    // text and the divider (previously only a fraction of the margin,
    // which read as cramped).
    float dividerY = y + margin + labelHeight + margin;
    drawList->AddLine(ImVec2(x + margin, dividerY), ImVec2(x + width - margin, dividerY),
                       ImGui::GetColorU32(ImGuiCol_Border), 1.0f);

    // Inner content rect: full margin's worth of gap after the divider
    // too (previously half), matching the same spacing used everywhere
    // else around this pane.
    float contentY = dividerY + margin;
    PaneRenderContext inner{
        x + margin,
        contentY,
        std::max(0.0f, width - margin * 2.0f),
        std::max(0.0f, (y + height) - contentY - margin)
    };
    return inner;
}

LayoutNodePtr TilingLayout::BuildDefaultLayout() {
    // Reverted per feedback: Inputs is one pane again, containing both the
    // "Regulated Inputs to PC" and "Devices (Android)" subsections (the
    // brief 3-separate-panes structure from the v0.6.2 spec attempt was
    // explicitly taken back). Left ~47%, right ~53%.
    auto output = std::make_unique<LayoutNode>();
    output->isLeaf = true;
    output->paneId = "output";

    auto log = std::make_unique<LayoutNode>();
    log->isLeaf = true;
    log->paneId = "log";

    auto leftColumn = std::make_unique<LayoutNode>();
    leftColumn->isLeaf = false;
    leftColumn->orientation = SplitOrientation::Vertical;
    leftColumn->ratio = 0.50f; // 50/50 split, per feedback
    leftColumn->first = std::move(output);
    leftColumn->second = std::move(log);

    auto sources = std::make_unique<LayoutNode>();
    sources->isLeaf = true;
    sources->paneId = "sources";

    auto inputs = std::make_unique<LayoutNode>();
    inputs->isLeaf = true;
    inputs->paneId = "inputs";

    auto rightColumn = std::make_unique<LayoutNode>();
    rightColumn->isLeaf = false;
    rightColumn->orientation = SplitOrientation::Vertical;
    rightColumn->ratio = 0.37f; // 37/63 split, per feedback
    rightColumn->first = std::move(sources);
    rightColumn->second = std::move(inputs);

    auto root = std::make_unique<LayoutNode>();
    root->isLeaf = false;
    root->orientation = SplitOrientation::Horizontal;
    root->ratio = 0.47f;
    root->first = std::move(leftColumn);
    root->second = std::move(rightColumn);

    return root;
}

LayoutNode* TilingLayout::FindPaneParent(LayoutNode& root, const std::string& paneId) {
    if (root.isLeaf) return nullptr;
    if ((root.first && root.first->isLeaf && root.first->paneId == paneId) ||
        (root.second && root.second->isLeaf && root.second->paneId == paneId)) {
        return &root;
    }
    if (root.first) {
        if (LayoutNode* found = FindPaneParent(*root.first, paneId)) return found;
    }
    if (root.second) {
        if (LayoutNode* found = FindPaneParent(*root.second, paneId)) return found;
    }
    return nullptr;
}

void TilingLayout::Render(LayoutNode& root, float x, float y, float width, float height,
                           const std::function<void(const std::string&, const PaneRenderContext&)>& renderFn,
                           const std::function<std::string(const std::string&)>& displayNameFn) {
    RenderNode(root, x, y, width, height, renderFn, displayNameFn);
}

void TilingLayout::RenderNode(LayoutNode& node, float x, float y, float width, float height,
                               const std::function<void(const std::string&, const PaneRenderContext&)>& renderFn,
                               const std::function<std::string(const std::string&)>& displayNameFn) {
    if (node.isLeaf) {
        ImGui::PushID(node.paneId.c_str());

        std::string headerLabel = displayNameFn(node.paneId);
        constexpr float kPanelMargin = 14.0f; // item 5: more padding around pane titles, per feedback
        PaneRenderContext inner = DrawSubtlePanelFrame(headerLabel, x, y, width, height, kPanelMargin);

        ImGui::SetCursorScreenPos(ImVec2(inner.x, inner.y));
        std::string childId = std::string("##content_") + node.paneId;
        // No border on the child itself -- DrawSubtlePanelFrame already
        // drew the ONE border for this pane. A second border here is
        // exactly what produced the "box inside a box" look reported
        // earlier.
        if (ImGui::BeginChild(childId.c_str(), ImVec2(inner.width, inner.height),
                               ImGuiChildFlags_None)) {
            renderFn(node.paneId, inner);
        }
        ImGui::EndChild();

        ImGui::PopID();
        return;
    }

    // --- Split node: divide the rect, render both children, draw a
    // draggable divider between them ---
    bool horizontal = (node.orientation == SplitOrientation::Horizontal);
    float totalLength = horizontal ? width : height;
    float firstLength = std::max(0.0f, totalLength * node.ratio - kPaneGap * 0.5f);
    float secondLength = std::max(0.0f, totalLength - firstLength - kPaneGap);
    float splitterInset = (kPaneGap - kSplitterThickness) * 0.5f; // centers the thinner hitbox within the gap

    float firstX = x, firstY = y, firstW = width, firstH = height;
    float secondX = x, secondY = y, secondW = width, secondH = height;
    float splitterX, splitterY, splitterW, splitterH;

    if (horizontal) {
        firstW = firstLength;
        secondX = x + firstLength + kPaneGap;
        secondW = secondLength;
        splitterX = x + firstLength + splitterInset;
        splitterY = y;
        splitterW = kSplitterThickness;
        splitterH = height;
    } else {
        firstH = firstLength;
        secondY = y + firstLength + kPaneGap;
        secondH = secondLength;
        splitterX = x;
        splitterY = y + firstLength + splitterInset;
        splitterW = width;
        splitterH = kSplitterThickness;
    }

    RenderNode(*node.first, firstX, firstY, firstW, firstH, renderFn, displayNameFn);

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
    // Thinner than the full hitbox (half its size) and inset/centered
    // within it, rather than filling the whole hitbox edge-to-edge -- the
    // hitbox itself stays the full size for easy grabbing, only the
    // VISIBLE highlight shrinks and gets breathing room around it.
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImU32 col = ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);

        float visibleThickness = (horizontal ? splitterW : splitterH) * 0.5f;
        float totalThickness = horizontal ? splitterW : splitterH;
        float thicknessInset = (totalThickness - visibleThickness) * 0.5f;

        // Shorten the highlight along its LENGTH too -- previously it
        // spanned the entire pane boundary (splitterH for a horizontal
        // splitter, splitterW for a vertical one), which read as "very
        // basic"/a full divider line. Now a short segment centered along
        // that boundary, capped so it never exceeds the actual available
        // length on very small panes.
        float boundaryLength = horizontal ? splitterH : splitterW;
        float visibleLength = std::min(128.0f, boundaryLength); // doubled from 64px, per feedback
        float lengthInset = (boundaryLength - visibleLength) * 0.5f;

        ImVec2 hiMin, hiMax;
        if (horizontal) {
            hiMin = ImVec2(splitterX + thicknessInset, splitterY + lengthInset);
            hiMax = ImVec2(splitterX + thicknessInset + visibleThickness, splitterY + lengthInset + visibleLength);
        } else {
            hiMin = ImVec2(splitterX + lengthInset, splitterY + thicknessInset);
            hiMax = ImVec2(splitterX + lengthInset + visibleLength, splitterY + thicknessInset + visibleThickness);
        }
        drawList->AddRectFilled(hiMin, hiMax, col, 2.0f);
    }
    ImGui::PopID();

    RenderNode(*node.second, secondX, secondY, secondW, secondH, renderFn, displayNameFn);
}

} // namespace wm
