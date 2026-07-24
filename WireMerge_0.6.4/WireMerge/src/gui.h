#pragma once
#include "audio_handler.h"
#include "usb_handler.h"
#include "adb_handler.h"
#include "mixer.h"
#include "layout.h"
#include <deque>
#include <mutex>

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

    std::mutex usbQueueMutex_;
    std::deque<std::pair<UsbEvent, UsbDeviceInfo>> usbEventQueue_;

    std::deque<std::string> logLines_;
    size_t lastSeenLogLineCount_ = 0; // for RenderLogContent's auto-scroll-on-new-lines fix
    bool wasAtBottomLastFrame_ = true; // whether user was scrolled to bottom; gates auto-follow
};

} // namespace wm

