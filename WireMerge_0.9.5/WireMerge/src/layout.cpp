#include "layout.h"
#include "imgui.h"
#include <algorithm>
#include <cstring>

namespace wm {

static constexpr float kMinRatio = 0.12f;
static constexpr float kMaxRatio = 0.88f;
static constexpr float kPaneGap = 16.0f;
static constexpr float kSplitterThickness = 10.0f;

PaneRenderContext DrawSubtlePanelFrame(const std::string& label, float x, float y,
                                        float width, float height, float margin) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 p0(x, y);
    ImVec2 p1(x + width, y + height);

    drawList->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_ChildBg), 4.0f);
    drawList->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), 4.0f, 0, 1.0f);

    constexpr float kTitleFontScale = 15.0f / 13.0f;
    ImGui::SetCursorScreenPos(ImVec2(x + margin, y + margin));
    ImGui::SetWindowFontScale(kTitleFontScale);
    ImGui::TextUnformatted(label.c_str());
    float labelHeight = ImGui::GetTextLineHeight();
    ImGui::SetWindowFontScale(1.0f);

    float dividerY = y + margin + labelHeight + margin;
    drawList->AddLine(ImVec2(x + margin, dividerY), ImVec2(x + width - margin, dividerY),
                       ImGui::GetColorU32(ImGuiCol_Border), 1.0f);

    float contentY = dividerY + margin;
    PaneRenderContext inner{
        x + margin,
        contentY,
        std::max(1.0f, width - margin * 2.0f),
        std::max(1.0f, (y + height) - contentY - margin)
    };
    return inner;
}

LayoutNodePtr TilingLayout::BuildDefaultLayout() {
    auto output = std::make_unique<LayoutNode>();
    output->isLeaf = true;
    output->paneId = "output";

    auto log = std::make_unique<LayoutNode>();
    log->isLeaf = true;
    log->paneId = "log";

    auto leftColumn = std::make_unique<LayoutNode>();
    leftColumn->isLeaf = false;
    leftColumn->orientation = SplitOrientation::Vertical;
    leftColumn->ratio = 0.50f;
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
    rightColumn->ratio = 0.37f;
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
        constexpr float kPanelMargin = 14.0f;
        PaneRenderContext inner = DrawSubtlePanelFrame(headerLabel, x, y, width, height, kPanelMargin);

        ImGui::SetCursorScreenPos(ImVec2(inner.x, inner.y));
        std::string childId = std::string("##content_") + node.paneId;
        if (ImGui::BeginChild(childId.c_str(), ImVec2(inner.width, inner.height),
                               ImGuiChildFlags_None)) {
            renderFn(node.paneId, inner);
        }
        ImGui::EndChild();

        ImGui::PopID();
        return;
    }

    bool horizontal = (node.orientation == SplitOrientation::Horizontal);
    float totalLength = horizontal ? width : height;
    float firstLength = std::max(0.0f, totalLength * node.ratio - kPaneGap * 0.5f);
    float secondLength = std::max(0.0f, totalLength - firstLength - kPaneGap);
    float splitterInset = (kPaneGap - kSplitterThickness) * 0.5f;

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

    ImGui::PushID(&node);
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

    {
        bool hoveredOrActive = ImGui::IsItemHovered() || ImGui::IsItemActive();
        float target = hoveredOrActive ? 1.0f : 0.0f;
        float dt = ImGui::GetIO().DeltaTime;
        constexpr float kFadeSpeed = 9.0f;
        if (node.splitterAlpha < target) {
            node.splitterAlpha = std::min(target, node.splitterAlpha + kFadeSpeed * dt);
        } else if (node.splitterAlpha > target) {
            node.splitterAlpha = std::max(target, node.splitterAlpha - kFadeSpeed * dt);
        }

        if (node.splitterAlpha > 0.001f) {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            ImVec4 restCol = ImGui::GetStyleColorVec4(ImGuiCol_Border);
            ImVec4 hotCol = ImGui::GetStyleColorVec4(ImGui::IsItemActive() ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
            float t = node.splitterAlpha;
            ImVec4 blended(restCol.x + (hotCol.x - restCol.x) * t,
                            restCol.y + (hotCol.y - restCol.y) * t,
                            restCol.z + (hotCol.z - restCol.z) * t,
                            restCol.w + (hotCol.w - restCol.w) * t);
            constexpr float kRestAlpha = 0.25f;
            blended.w = kRestAlpha + (hotCol.w - kRestAlpha) * t;
            ImU32 col = ImGui::GetColorU32(blended);

            float visibleThickness = (horizontal ? splitterW : splitterH) * 0.5f;
            float totalThickness = horizontal ? splitterW : splitterH;
            float thicknessInset = (totalThickness - visibleThickness) * 0.5f;

            float boundaryLength = horizontal ? splitterH : splitterW;
            float visibleLength = std::min(128.0f, boundaryLength);
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
    }
    ImGui::PopID();

    RenderNode(*node.second, secondX, secondY, secondW, secondH, renderFn, displayNameFn);
}

}
