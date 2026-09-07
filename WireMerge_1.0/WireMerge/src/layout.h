#pragma once
#include <string>
#include <memory>
#include <functional>

namespace wm {

enum class SplitOrientation { Horizontal, Vertical };

struct LayoutNode;
using LayoutNodePtr = std::unique_ptr<LayoutNode>;

struct LayoutNode {
    bool isLeaf = true;

    std::string paneId;

    SplitOrientation orientation = SplitOrientation::Horizontal;
    float ratio = 0.5f;
    LayoutNodePtr first;
    LayoutNodePtr second;

    float splitterAlpha = 0.0f;
};

struct PaneRenderContext {
    float x, y, width, height;
};

PaneRenderContext DrawSubtlePanelFrame(const std::string& label, float x, float y,
                                        float width, float height, float margin);

class TilingLayout {
public:
    static LayoutNodePtr BuildDefaultLayout();

    static LayoutNode* FindPaneParent(LayoutNode& root, const std::string& paneId);

    static void Render(LayoutNode& root, float x, float y, float width, float height,
                        const std::function<void(const std::string& paneId, const PaneRenderContext&)>& renderFn,
                        const std::function<std::string(const std::string& paneId)>& displayNameFn);

private:
    static void RenderNode(LayoutNode& node, float x, float y, float width, float height,
                            const std::function<void(const std::string&, const PaneRenderContext&)>& renderFn,
                            const std::function<std::string(const std::string&)>& displayNameFn);
};

}
