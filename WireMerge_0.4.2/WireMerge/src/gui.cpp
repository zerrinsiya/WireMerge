#include "gui.h"
#include "utils.h"

#include <d3d11.h>
#include <windows.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace wm {

// ---------------------------------------------------------------------------
// Small helpers to keep the void*-based header clean of D3D types.
// ---------------------------------------------------------------------------
static ID3D11Device* Dev(void* p) { return static_cast<ID3D11Device*>(p); }
static ID3D11DeviceContext* Ctx(void* p) { return static_cast<ID3D11DeviceContext*>(p); }
static IDXGISwapChain* Swap(void* p) { return static_cast<IDXGISwapChain*>(p); }
static ID3D11RenderTargetView* RTV(void* p) { return static_cast<ID3D11RenderTargetView*>(p); }

static Gui* g_activeGui = nullptr; // for the WndProc trampoline below

// Explicit ANSI (WndProcA-equivalent) throughout this file -- deliberately
// not using the generic TCHAR-based macros (_T, CreateWindow, WNDCLASSEX's
// implicit W/A resolution) to avoid UNICODE-macro-dependent mismatches
// between narrow and wide Win32 calls.
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

    switch (msg) {
        case WM_SIZE:
            // SIZE_MINIMIZED reports a 0x0 client area; skip the resize
            // entirely rather than trying to build a 0-sized render target
            // (RenderFrame separately skips rendering while minimized --
            // see IsIconic check there -- so nothing needs this data until
            // the window is restored, at which point WM_SIZE fires again
            // with the real dimensions).
            if (g_activeGui && wParam != SIZE_MINIMIZED) {
                g_activeGui->HandleResize(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

Gui::Gui(AudioHandler& audio, UsbHandler& usb, AdbHandler& adb, Mixer& mixer)
    : audio_(audio), usb_(usb), adb_(adb), mixer_(mixer) {}

Gui::~Gui() {
    Shutdown();
}

bool Gui::Initialize() {
    // Set before CreateWindowExA -- Windows can dispatch WM_SIZE synchronously
    // as part of window creation itself, and WndProc needs a valid pointer
    // to forward that to (HandleResize itself no-ops safely if the D3D
    // device isn't ready yet, so this is safe even if it fires that early).
    g_activeGui = this;

    WNDCLASSEXA wc = {
        sizeof(WNDCLASSEXA), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
        "WireMergeWindowClass", nullptr
    };
    RegisterClassExA(&wc);

    // Window title includes the version string (utils.h) so builds are
    // identifiable at a glance -- update kWireMergeVersion there on release.
    HWND hwnd = CreateWindowExA(0, "WireMergeWindowClass", kWireMergeVersion,
                                 WS_OVERLAPPEDWINDOW, 100, 100, 940, 720,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        WM_LOG_ERROR("Failed to create Win32 window.");
        return false;
    }
    hwnd_ = hwnd;

    // --- D3D11 device/swapchain setup ---
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
        &swapChain, &device, &featureLevel, &context);

    if (FAILED(hr)) {
        WM_LOG_ERROR("D3D11CreateDeviceAndSwapChain failed. GPU/driver may not support DX11; "
                      "this is unusual for a low-end target so double-check the machine's "
                      "graphics drivers are installed.");
        return false;
    }

    d3dDevice_ = device;
    d3dContext_ = context;
    swapChain_ = swapChain;

    ID3D11Texture2D* backBuffer = nullptr;
    Swap(swapChain_)->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    ID3D11RenderTargetView* rtv = nullptr;
    Dev(d3dDevice_)->CreateRenderTargetView(backBuffer, nullptr, &rtv);
    backBuffer->Release();
    renderTargetView_ = rtv;

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // --- ImGui setup ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // Global spacing bump -- ImGui's defaults are quite tight, which is
    // what made "almost everything" feel cramped. This is a broad,
    // temporary measure (a real visual design pass is planned separately)
    // rather than hand-tuning every individual widget's spacing.
    ImGuiStyle& style = ImGui::GetStyle();
    style.ItemSpacing = ImVec2(10, 10);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.FramePadding = ImVec2(8, 5);
    style.WindowPadding = ImVec2(14, 14);
    style.IndentSpacing = 24.0f;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(Dev(d3dDevice_), Ctx(d3dContext_));

    // Wire up USB hotplug -> thread-safe queue drained on the UI thread.
    usb_.StartHotplugMonitor([this](UsbEvent ev, const UsbDeviceInfo& info) {
        std::lock_guard<std::mutex> lock(usbQueueMutex_);
        usbEventQueue_.push_back({ev, info});
    });

    running_ = true;
    return true;
}

void Gui::PushLogLine(const std::string& line) {
    logLines_.push_back(line);
    if (logLines_.size() > 200) logLines_.pop_front();
}

void Gui::DrainUsbEventQueue() {
    std::lock_guard<std::mutex> lock(usbQueueMutex_);
    while (!usbEventQueue_.empty()) {
        auto [ev, info] = usbEventQueue_.front();
        usbEventQueue_.pop_front();
        std::string name = !info.product.empty() ? info.product : "Unknown USB device";
        if (ev == UsbEvent::Connected) {
            PushLogLine("[USB] Connected: " + name +
                        (info.looksLikeAudioClass ? " (audio class)" : ""));
        } else {
            PushLogLine("[USB] Disconnected: " + name);
        }
    }
}

void Gui::RenderOutputPanel() {
    // First-launch-only default layout so panels don't all stack on top of
    // each other on a fresh run -- ImGuiCond_FirstUseEver means this is
    // ignored once imgui.ini has a remembered position (e.g. after the
    // user has dragged panels around), so it doesn't fight manual layout
    // on subsequent launches.
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 260), ImGuiCond_FirstUseEver);
    ImGui::Begin("Output Device");

    // Styled red so it reads as a distinct/deliberate action, not just
    // another control -- clicking this runs the exact same shutdown
    // sequence as closing the window (see RequestExit()'s doc comment),
    // it's not a separate/lesser cleanup path.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.20f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.10f, 0.10f, 1.0f));
    if (ImGui::Button("Exit WireMerge")) {
        RequestExit();
    }
    ImGui::PopStyleColor(3);
    ImGui::Separator();

    static float masterVolume = 1.0f;
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::SliderFloat("Master Volume", &masterVolume, 0.0f, 1.5f, "%.2f")) {
        mixer_.SetMasterVolume(masterVolume);
    }
    ImGui::Separator();

    auto outputs = audio_.ListOutputDevices();
    std::string preview = selectedOutputDevice_ >= 0 ? "Selected" : "Choose output...";
    for (auto& d : outputs) {
        if (d.index == selectedOutputDevice_) preview = d.name;
    }

    // -1 = stretch to fill the available panel width, rather than ImGui's
    // cramped default combo width -- the dropdown popup's width follows
    // the widget's width, so a narrow widget was cutting off longer
    // device names (e.g. "Speakers (2- USB Audio Device)") in the list.
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("Output", preview.c_str())) {
        for (auto& d : outputs) {
            bool selected = (d.index == selectedOutputDevice_);
            std::string label = d.name + (d.isDefaultOutput ? " (default)" : "");
            if (ImGui::Selectable(label.c_str(), selected)) {
                selectedOutputDevice_ = d.index;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::BeginDisabled(selectedOutputDevice_ < 0 || outputOpen_);
    if (ImGui::Button("Start Output")) {
        if (audio_.OpenOutput(mixer_, selectedOutputDevice_)) {
            outputOpen_ = true;
            PushLogLine("Output started.");
        } else {
            PushLogLine("Failed to start output -- check log file for details.");
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!outputOpen_);
    if (ImGui::Button("Stop Output")) {
        audio_.CloseOutput();
        outputOpen_ = false;
        PushLogLine("Output stopped.");
    }
    ImGui::EndDisabled();

    ImGui::End();
}

void Gui::RenderInputsPanel_PcSubsection() {
    ImGui::TextWrapped("USB microphones, DACs, and audio interfaces -- "
                        "anything Windows already shows as a recording device.");

    static int selectedInput = -1;
    auto inputs = audio_.ListInputDevices();
    std::string inPreview = "Choose input...";
    for (auto& d : inputs) if (d.index == selectedInput) inPreview = d.name;

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("Input", inPreview.c_str())) {
        for (auto& d : inputs) {
            bool selected = (d.index == selectedInput);
            std::string label = d.name + (d.isDefaultInput ? " (default)" : "");
            if (ImGui::Selectable(label.c_str(), selected)) selectedInput = d.index;
        }
        ImGui::EndCombo();
    }

    ImGui::BeginDisabled(selectedInput < 0);
    if (ImGui::Button("Add Source")) {
        SourceId id = audio_.OpenInputSource(mixer_, selectedInput);
        if (id != 0) PushLogLine("Added input source.");
        else PushLogLine("Failed to add input source -- check log file.");
    }
    ImGui::EndDisabled();

    if (ImGui::Button("Rescan Devices")) {
        PushLogLine("Rescanned devices.");
        // ListInputDevices()/ListOutputDevices() re-query PortAudio live
        // and are cheap in-memory lookups (unlike the ADB device list --
        // see the Android subsection below), so nothing else needs to
        // happen here besides the log entry.
    }
}

void Gui::RenderInputsPanel_AndroidSubsection() {
    if (!adb_.IsAvailable()) {
        ImGui::TextWrapped(
            "Not set up: adb.exe and sndcpy.apk weren't found in the 'tools' "
            "folder next to WireMerge.exe. WireMerge already tried a one-time "
            "automatic download on startup (check the Log panel for whether "
            "that succeeded or failed) -- if it failed, likely due to no "
            "internet access, see README.md for manual download links, or "
            "just restart WireMerge once you're back online.");
        return;
    }

    ImGui::TextWrapped(
        "Captures an app's audio (e.g. Spotify) from the phone itself, over "
        "the same USB cable used for charging/data. Requires USB debugging "
        "enabled on the phone (one-time authorization prompt), and each "
        "time capture starts, an on-device prompt to confirm screen/audio "
        "capture (normal Android behavior, not a bug). Not all apps allow "
        "this -- most do, some DRM-guarded streaming apps don't.");
    ImGui::Separator();

    static std::string selectedSerial;

    // adb_.ListDevices() spawns an actual adb.exe subprocess -- calling
    // that unconditionally every render frame was launching a process
    // 30-60+ times per second on the render thread, which was the actual
    // cause of general UI stutter/dragging jank (not a scheduling-priority
    // issue). Cached to a 2s interval, plus an explicit Rescan button.
    constexpr double kRescanIntervalMs = 2000.0;
    static std::vector<AdbDeviceInfo> cachedDevices;
    static double lastScanTime = -kRescanIntervalMs; // force an initial scan on first frame

    double now = ImGui::GetTime() * 1000.0;
    if (now - lastScanTime >= kRescanIntervalMs) {
        cachedDevices = adb_.ListDevices();
        lastScanTime = now;
    }

    if (ImGui::SmallButton("Rescan Now")) {
        cachedDevices = adb_.ListDevices();
        lastScanTime = now;
    }

    if (cachedDevices.empty()) {
        ImGui::TextDisabled("No devices detected. Plug in a phone with USB debugging enabled.");
    }

    for (auto& d : cachedDevices) {
        bool isSelected = (d.serial == selectedSerial);
        std::string label = d.serial + " [" + d.state + "]";
        if (ImGui::RadioButton(label.c_str(), isSelected)) {
            selectedSerial = d.serial;
        }
    }

    // Poll for a just-finished async start every frame, regardless of
    // whether this device is currently selected -- a start kicked off
    // earlier should still get its result consumed and logged even if the
    // user has since clicked a different device's radio button.
    for (auto& d : cachedDevices) {
        SourceId result;
        if (adb_.TryTakeStartResult(d.serial, result)) {
            if (result != 0) {
                PushLogLine("Started Android capture for " + d.serial + " (source added).");
            } else {
                PushLogLine("Failed to start Android capture for " + d.serial +
                            " -- check log file for details.");
            }
        }
    }

    bool starting = !selectedSerial.empty() && adb_.IsStarting(selectedSerial);
    bool canStart = !selectedSerial.empty() && !starting;

    ImGui::BeginDisabled(!canStart);
    if (ImGui::Button("Start Android Capture")) {
        // Async: the install/forward/launch sequence involves several
        // adb.exe round-trips and can take up to ~10s (an APK install
        // alone can be several seconds) -- running that synchronously on
        // this button's call stack previously froze the whole window
        // (Windows marks a window "not responding" once its message loop
        // stops pumping, which is exactly what a blocked render thread
        // does). Now runs on a background thread; see the "starting..."
        // status text below for the in-progress state.
        adb_.StartCaptureAsync(mixer_, selectedSerial);
        PushLogLine("Starting Android capture for " + selectedSerial +
                    "... this can take up to ~10s (installing/launching on "
                    "the phone). The window will stay responsive.");
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(selectedSerial.empty() || starting);
    if (ImGui::Button("Stop Android Capture")) {
        adb_.StopCapture(selectedSerial);
        PushLogLine("Stopped Android capture for " + selectedSerial);
    }
    ImGui::EndDisabled();

    if (starting) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f),
                            "Starting capture on %s... check the phone screen "
                            "for a permission prompt.", selectedSerial.c_str());
    }
}

void Gui::RenderInputsPanel() {
    ImGui::SetNextWindowPos(ImVec2(440, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440, 380), ImGuiCond_FirstUseEver);
    ImGui::Begin("Inputs");

    // Two clearly separated subsections in one window rather than two
    // separate top-level windows -- Android capture is the app's main
    // feature, not a secondary/bolted-on one, so it belongs alongside
    // regular PC inputs as another input source type, not off in its own
    // disconnected panel.
    if (ImGui::CollapsingHeader("Regulated Inputs to PC", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        RenderInputsPanel_PcSubsection();
        ImGui::Unindent();
    }

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 12));

    if (ImGui::CollapsingHeader("Devices (Android)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        RenderInputsPanel_AndroidSubsection();
        ImGui::Unindent();
    }

    ImGui::End();
}

void Gui::RenderSourcesPanel() {
    ImGui::SetNextWindowPos(ImVec2(20, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 320), ImGuiCond_FirstUseEver);
    ImGui::Begin("Active Sources");

    auto sources = mixer_.ListSources();
    if (sources.empty()) {
        ImGui::TextDisabled("No sources added yet.");
    }

    for (auto& s : sources) {
        ImGui::PushID(static_cast<int>(s.id));

        bool enabled = s.enabled;
        if (ImGui::Checkbox("##enabled", &enabled)) {
            mixer_.SetEnabled(s.id, enabled);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(s.name.c_str());

        float volume = s.volume;
        ImGui::SetNextItemWidth(180.0f);
        // Kept as an optional trim only -- the external device (phone/TV)
        // is expected to be the primary volume control, per the project's
        // core design goal. 1.0 = unity/pass-through.
        if (ImGui::SliderFloat("Trim", &volume, 0.0f, 2.0f, "%.2f")) {
            mixer_.SetVolume(s.id, volume);
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            audio_.CloseInputSource(s.id);
            mixer_.RemoveSource(s.id);
        }

        // Live underrun indicator -- rising quickly means this source's
        // buffer is running dry (audible gaps/stutter). Color-coded so
        // it's visible at a glance without reading exact numbers.
        uint64_t underrunFrames = mixer_.GetUnderrunFrames(s.id);
        double underrunMs = static_cast<double>(underrunFrames) / 48000.0 * 1000.0; // approximation; exact rate varies per source
        ImVec4 color = underrunMs < 50.0 ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f)
                      : underrunMs < 500.0 ? ImVec4(0.9f, 0.7f, 0.2f, 1.0f)
                      : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
        ImGui::TextColored(color, "Underruns: ~%.0fms of silence total", underrunMs);

        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::End();
}

void Gui::RenderLogPanel() {
    ImGui::SetNextWindowPos(ImVec2(440, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440, 200), ImGuiCond_FirstUseEver);
    ImGui::Begin("Log");
    // TextUnformatted never wraps regardless of window width -- that's
    // why log lines were clipping off-screen instead of wrapping.
    // TextWrapped wraps to the current content region width automatically.
    for (auto& line : logLines_) {
        ImGui::TextWrapped("%s", line.c_str());
    }
    if (!logLines_.empty()) ImGui::SetScrollHereY(1.0f);
    ImGui::End();
}

void Gui::HandleResize(unsigned int width, unsigned int height) {
    // No-op if called before D3D is set up (e.g. an early WM_SIZE during
    // CreateWindowExA, before the swapchain exists yet) or with a
    // degenerate size.
    if (!d3dDevice_ || !d3dContext_ || !swapChain_) return;
    if (width == 0 || height == 0) return;

    // Must release the old render target view before ResizeBuffers --
    // DXGI refuses to resize while anything still references the old
    // back buffer.
    if (renderTargetView_) {
        RTV(renderTargetView_)->Release();
        renderTargetView_ = nullptr;
    }

    HRESULT hr = Swap(swapChain_)->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        WM_LOG_ERROR("Gui::HandleResize: ResizeBuffers failed (hr=0x" +
                      std::to_string(static_cast<unsigned long>(hr)) + ")");
        return;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    Swap(swapChain_)->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    ID3D11RenderTargetView* rtv = nullptr;
    Dev(d3dDevice_)->CreateRenderTargetView(backBuffer, nullptr, &rtv);
    backBuffer->Release();
    renderTargetView_ = rtv;
}

void Gui::RenderFrame() {
    // Skip rendering entirely while minimized: the client area is 0x0, the
    // swapchain/render target aren't meaningfully sized, and calling
    // Present() in this state is what caused visible corruption on
    // minimize/restore before this guard existed. A short sleep avoids
    // busy-spinning Run()'s loop the whole time the window is minimized.
    if (hwnd_ && IsIconic(static_cast<HWND>(hwnd_))) {
        Sleep(50);
        return;
    }

    DrainUsbEventQueue();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    RenderOutputPanel();
    RenderInputsPanel();
    RenderSourcesPanel();
    RenderLogPanel();

    ImGui::Render();

    const float clearColor[4] = {0.08f, 0.08f, 0.10f, 1.0f};
    Ctx(d3dContext_)->OMSetRenderTargets(1, reinterpret_cast<ID3D11RenderTargetView* const*>(&renderTargetView_), nullptr);
    Ctx(d3dContext_)->ClearRenderTargetView(RTV(renderTargetView_), clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    Swap(swapChain_)->Present(1, 0); // vsync on -- keeps CPU usage low, matches "lightweight" goal
}

void Gui::Run() {
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (running_ && msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }
        RenderFrame();
    }
}

void Gui::Shutdown() {
    if (!running_ && !hwnd_) return;

    usb_.StopHotplugMonitor();

    if (d3dDevice_ || d3dContext_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    if (renderTargetView_) { RTV(renderTargetView_)->Release(); renderTargetView_ = nullptr; }
    if (swapChain_) { Swap(swapChain_)->Release(); swapChain_ = nullptr; }
    if (d3dContext_) { Ctx(d3dContext_)->Release(); d3dContext_ = nullptr; }
    if (d3dDevice_) { Dev(d3dDevice_)->Release(); d3dDevice_ = nullptr; }

    if (hwnd_) {
        DestroyWindow(static_cast<HWND>(hwnd_));
        UnregisterClassA("WireMergeWindowClass", GetModuleHandleA(nullptr));
        hwnd_ = nullptr;
    }

    running_ = false;
}

} // namespace wm
