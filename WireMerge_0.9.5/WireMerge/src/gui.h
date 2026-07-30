#pragma once
#include "audio_handler.h"
#include "usb_handler.h"
#include "adb_handler.h"
#include "mixer.h"
#include "layout.h"
#include <deque>
#include <vector>
#include <mutex>
#include <cstdint>

struct ImDrawList;
struct ImFont;
struct ImVec2;
using ImU32 = unsigned int; //must match Dear ImGui's own typedef

namespace wm {

class Gui {
public:
    Gui(AudioHandler& audio, UsbHandler& usb, AdbHandler& adb, Mixer& mixer);
    ~Gui();

    bool Initialize();
    void Run();
    void Shutdown();

    void RequestExit() { running_ = false; }
    void HandleResize(unsigned int width, unsigned int height);

private:
    void RenderFrame();
    void RenderToolbar(float& outContentY);
    void RenderPane(const std::string& paneId, const PaneRenderContext& ctx);
    std::string PaneDisplayName(const std::string& paneId) const;

    void RenderOutputContent(const PaneRenderContext& ctx);
    void RenderInputsContent(const PaneRenderContext& ctx);
    void RenderDevicesContent(const PaneRenderContext& ctx);
    void RenderSourcesContent(const PaneRenderContext& ctx);
    void RenderLogContent(const PaneRenderContext& ctx);

    void ApplyTheme();
    void DrainUsbEventQueue();
    void PushLogLine(const std::string& line);

    void LogUnderrunSummaryBeforeRemoval(SourceId id, const std::string& sourceName);

    AudioHandler& audio_;
    UsbHandler& usb_;
    AdbHandler& adb_;
    Mixer& mixer_;

    void* hwnd_ = nullptr;
    void* d3dDevice_ = nullptr;
    void* d3dContext_ = nullptr;
    void* swapChain_ = nullptr;
    void* renderTargetView_ = nullptr;

    bool running_ = false;
    int selectedOutputDevice_ = -1;
    bool outputOpen_ = false;

    LayoutNodePtr layout_;
    LayoutNode* sourcesSplitNode_ = nullptr;
    bool sourcesRatioSettled_ = false;

    bool showLicensesWindow_ = false;
    bool showTestersWindow_ = false;

    struct FooterOccluderRect { float minX, minY, maxX, maxY; };
    std::vector<FooterOccluderRect> footerOccluderRects_;
    void RegisterFooterOccluder();
    void DrawFooterTextClipped(ImDrawList* dl, ImFont* font, float fontSize,
                                ImVec2 pos, ImVec2 size, ImU32 color, const char* text);

    void RenderToolsDownloadPrompt();
    bool toolsPromptShown_ = false;
    bool showToolsDownloadPrompt_ = false;
    bool loggedInitialToolsCheck_ = false;
    bool wasDownloadInProgress_ = false;

    void RenderPerformanceWindow();
    bool showPerfWindow_ = false;
    uint64_t perfPrevKernelTime100ns_ = 0;
    uint64_t perfPrevUserTime100ns_ = 0;
    double perfPrevWallMs_ = 0.0;
    double perfLastSampleMs_ = -100000.0;
    double perfCpuPercentCur_ = 0.0;
    double perfCpuPercentAvg_ = 0.0;
    double perfWorkingSetMB_ = 0.0;
    double perfPeakWorkingSetMB_ = 0.0;
    double perfPrivateBytesMB_ = 0.0;
    uint64_t perfPageFaultCount_ = 0;
    uint32_t perfHandleCount_ = 0;
    uint32_t perfGdiObjectCount_ = 0;
    uint32_t perfUserObjectCount_ = 0;
    double perfUptimeSeconds_ = 0.0;
    double perfFrameTimeMsAvg_ = 0.0;

    std::mutex usbQueueMutex_;
    std::deque<std::pair<UsbEvent, UsbDeviceInfo>> usbEventQueue_;

    std::deque<std::string> logLines_;
    size_t totalLogLinesPushed_ = 0;
    size_t lastSeenLogLineCount_ = 0;
    bool wasAtBottomLastFrame_ = true;
};

}
