#pragma once
#include "audio_handler.h"
#include "usb_handler.h"
#include "adb_handler.h"
#include "mixer.h"
#include <deque>
#include <mutex>

// ---------------------------------------------------------------------------
// gui.h
//
// Dear ImGui frontend (Win32 + DirectX 11 backend -- the standard lightweight
// combo for a small native Windows tool; avoids pulling in GLFW/OpenGL just
// for a control panel).
//
// Responsibilities:
//   - Own the Win32 window + DX11 device/swapchain.
//   - Run the main loop (message pump + ImGui frame + present).
//   - Let the user pick an output device, add input sources from the
//     enumerated PortAudio input device list, toggle sources on/off, and
//     adjust the optional per-source trim volume.
//   - Show live USB connect/disconnect notifications (via UsbHandler's
//     hotplug callback, marshaled onto the UI thread through a queue).
// ---------------------------------------------------------------------------

namespace wm {

class Gui {
public:
    Gui(AudioHandler& audio, UsbHandler& usb, AdbHandler& adb, Mixer& mixer);
    ~Gui();

    // Creates the window + DX11 context + ImGui context. Returns false on failure.
    bool Initialize();

    // Blocks, running the message pump + render loop until the window is
    // closed. Returns when the app should exit.
    void Run();

    void Shutdown();

    // Sets the flag that Run()'s message loop checks each iteration,
    // causing a clean exit on the *next* loop check -- same effect as the
    // user clicking the window's close button (both ultimately just stop
    // Run() from continuing; actual teardown happens in main.cpp's
    // gui.Shutdown()/adb.Shutdown()/audio.Shutdown()/usb.Shutdown()
    // sequence either way, so there's only one cleanup path, not two).
    void RequestExit() { running_ = false; }

    // Recreates the D3D11 render target view against the swap chain's
    // actual current buffer size. Called from WM_SIZE (see gui.cpp's
    // WndProc) -- without this, the render target silently goes stale
    // relative to the real window/swapchain dimensions on any resize,
    // including minimize/restore, which is what caused visible corruption
    // on minimize before this existed.
    void HandleResize(unsigned int width, unsigned int height);

private:
    void RenderFrame();
    void RenderOutputPanel();
    void RenderInputsPanel();
    void RenderInputsPanel_PcSubsection();
    void RenderInputsPanel_AndroidSubsection();
    void RenderSourcesPanel();
    void RenderLogPanel();
    void DrainUsbEventQueue();
    void PushLogLine(const std::string& line);

    AudioHandler& audio_;
    UsbHandler& usb_;
    AdbHandler& adb_;
    Mixer& mixer_;

    // Platform/DX handles are kept as void* here and cast in gui.cpp so
    // this header doesn't force <d3d11.h>/<windows.h> on every includer.
    void* hwnd_ = nullptr;
    void* d3dDevice_ = nullptr;
    void* d3dContext_ = nullptr;
    void* swapChain_ = nullptr;
    void* renderTargetView_ = nullptr;

    bool running_ = false;
    int selectedOutputDevice_ = -1;
    bool outputOpen_ = false;

    std::mutex usbQueueMutex_;
    std::deque<std::pair<UsbEvent, UsbDeviceInfo>> usbEventQueue_;

    std::deque<std::string> logLines_;
};

} // namespace wm
