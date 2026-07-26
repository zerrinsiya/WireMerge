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

// ---------------------------------------------------------------------------
// gui.h
//
// Dear ImGui frontend (Win32 + DirectX 11 backend). As of the v0.6 UI
// overhaul, panes are no longer floating ImGui windows -- they're tiled
// leaves in a TilingLayout tree (see layout.h), giving fixed-slot,
// splitter-resizable, swap-by-drag panes instead of independently
// draggable/resizable floating windows.
// ---------------------------------------------------------------------------

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
    void RenderToolbar(float& outContentY); // fixed top strip: branding + Exit -- see 2.6
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

    // Logs a source's accumulated underrun time before it's removed (3.3)
    // -- call this BEFORE Mixer::RemoveSource, since the ring buffer (and
    // its underrun counter) goes away with the source.
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
    // item 5: the Sources/Inputs split node, cached once so
    // RenderSourcesContent can nudge its ratio without re-searching the
    // tree every frame. Grows the ratio in small steps only until Active
    // Sources stops needing its own scrollbar, then stops permanently
    // (sourcesRatioSettled_) so it never fights a later manual splitter
    // drag.
    LayoutNode* sourcesSplitNode_ = nullptr;
    bool sourcesRatioSettled_ = false;

    // Items 6/7: footer popups. AlwaysAutoResize + NoSavedSettings, same
    // reasoning as the Performance window's item 5/8 fix.
    bool showLicensesWindow_ = false;
    bool showTestersWindow_ = false;

    // Item 3: the footer draws on the foreground draw list (see
    // RenderFrame), which by construction ignores normal window z-order
    // and always draws on top of everything -- including floating
    // windows the user has dragged over it. To fix that, every floating
    // window that could plausibly overlap the footer registers its
    // current screen rect here (via RegisterFooterOccluder, called right
    // after its own Begin()) BEFORE the footer draws each frame; the
    // footer then skips any of its own elements that fall inside a
    // registered rect. Cleared and repopulated fresh every frame.
    struct FooterOccluderRect { float minX, minY, maxX, maxY; };
    std::vector<FooterOccluderRect> footerOccluderRects_;
    void RegisterFooterOccluder(); // call right after Begin() for a window that might cover the footer
    bool IsFooterRectOccluded(float minX, float minY, float maxX, float maxY) const;

    // Item 3: download-consent prompt state. toolsPromptShown_ ensures
    // the modal only auto-opens once per session (the first time we
    // learn tools are actually missing); after that, re-showing it is
    // only ever user-initiated via the "Download Tools" button in the
    // Devices panel.
    void RenderToolsDownloadPrompt();
    bool toolsPromptShown_ = false;
    bool showToolsDownloadPrompt_ = false;
    // Item 1: edge-detected so the on-screen Log panel gets a clear
    // entry for each step of the tools lifecycle (initial check
    // finishing, and download finishing) -- WM_LOG_* alone only reaches
    // the file/console log, not this panel; see PushLogLine.
    bool loggedInitialToolsCheck_ = false;
    bool wasDownloadInProgress_ = false;

    // Item 8: lightweight performance panel state. Sampling itself is
    // throttled independently of render rate (see RenderPerformanceWindow)
    // so leaving the panel open doesn't add per-frame OS-call overhead --
    // it only actually queries CPU/memory a couple times a second.
    void RenderPerformanceWindow();
    bool showPerfWindow_ = false;
    uint64_t perfPrevKernelTime100ns_ = 0;
    uint64_t perfPrevUserTime100ns_ = 0;
    double perfPrevWallMs_ = 0.0;
    double perfLastSampleMs_ = -100000.0; // forces an immediate first sample
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
    // Item 3 (this round): must be a monotonically increasing total, NOT
    // logLines_.size(). The deque is capped at 200 with pop_front() -- once
    // that cap is hit, one push + one pop leaves size() unchanged, so a
    // size-based comparison silently stops detecting new lines forever
    // (this was the actual root cause of "doesn't auto-scroll", not the
    // already-fixed hard-pin issue). PushLogLine increments this on every
    // call regardless of trimming.
    size_t totalLogLinesPushed_ = 0;
    size_t lastSeenLogLineCount_ = 0; // for RenderLogContent's auto-scroll-on-new-lines fix
    bool wasAtBottomLastFrame_ = true; // whether user was scrolled to bottom; gates auto-follow
};

} // namespace wm

