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
                                 WS_OVERLAPPEDWINDOW, 100, 100, 900, 600,
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

void Gui::RenderDevicesPanel() {
    // First-launch-only default layout so the four panels don't all stack
    // on top of each other on a fresh run -- ImGuiCond_FirstUseEver means
    // this is ignored once imgui.ini has a remembered position (e.g. after
    // the user has dragged panels around), so it doesn't fight manual
    // layout on subsequent launches.
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 300), ImGuiCond_FirstUseEver);
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

    ImGui::Separator();
    ImGui::TextWrapped("Add an input source below (this is where your phone/TV shows up "
                        "once Windows has enumerated it as a recording device).");

    static int selectedInput = -1;
    auto inputs = audio_.ListInputDevices();
    std::string inPreview = "Choose input...";
    for (auto& d : inputs) if (d.index == selectedInput) inPreview = d.name;

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
        // ListInputDevices()/ListOutputDevices() re-query PortAudio live,
        // so nothing else needs to happen here besides the log entry --
        // Pa_GetDeviceCount() reflects hotplug changes automatically on
        // most WASAPI backends.
    }

    ImGui::End();
}

void Gui::RenderSourcesPanel() {
    ImGui::SetNextWindowPos(ImVec2(460, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 300), ImGuiCond_FirstUseEver);
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
        // buffer is running dry (audible gaps/stutter). This is the actual
        // diagnostic signal for the stutter reports: previously an
        // underrun failed completely silently, so there was no way to
        // see when/how badly it was happening. Color-coded so it's
        // visible at a glance without reading exact numbers.
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

void Gui::RenderAndroidPanel() {
    ImGui::SetNextWindowPos(ImVec2(20, 340), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 220), ImGuiCond_FirstUseEver);
    ImGui::Begin("Android (ADB)");

    if (!adb_.IsAvailable()) {
        ImGui::TextWrapped(
            "Not set up: adb.exe and sndcpy.apk weren't found in the 'tools' "
            "folder next to WireMerge.exe. WireMerge already tried a one-time "
            "automatic download on startup (check the Log panel below for "
            "whether that succeeded or failed) -- if it failed, likely due to "
            "no internet access, see README.md for manual download links, or "
            "just restart WireMerge once you're back online.");
        ImGui::End();
        return;
    }

    ImGui::TextWrapped(
        "Captures an app's audio (e.g. Spotify) from the phone itself, over "
        "the same USB cable used for charging/data -- this is different "
        "from the USB mic/DAC input above. Requires USB debugging enabled "
        "on the phone; you'll get a one-time authorization prompt on the "
        "device, and each time capture starts, an on-device prompt to "
        "confirm screen/audio capture (this is normal Android behavior, "
        "not a bug). Not all apps allow this -- most do, some DRM-guarded "
        "streaming apps don't.");
    ImGui::Separator();

    static std::string selectedSerial;
    auto devices = adb_.ListDevices();

    if (devices.empty()) {
        ImGui::TextDisabled("No devices detected. Plug in a phone with USB debugging enabled.");
    }

    for (auto& d : devices) {
        bool isSelected = (d.serial == selectedSerial);
        std::string label = d.serial + " [" + d.state + "]";
        if (ImGui::RadioButton(label.c_str(), isSelected)) {
            selectedSerial = d.serial;
        }
    }

    bool canStart = !selectedSerial.empty();
    ImGui::BeginDisabled(!canStart);
    if (ImGui::Button("Start Android Capture")) {
        SourceId id = adb_.StartCapture(mixer_, selectedSerial);
        if (id != 0) {
            PushLogLine("Started Android capture for " + selectedSerial +
                        " -- check the phone screen for a capture-permission prompt.");
        } else {
            PushLogLine("Failed to start Android capture -- check log file for details.");
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!canStart);
    if (ImGui::Button("Stop Android Capture")) {
        adb_.StopCapture(selectedSerial);
        PushLogLine("Stopped Android capture for " + selectedSerial);
    }
    ImGui::EndDisabled();

    ImGui::End();
}

void Gui::RenderLogPanel() {
    ImGui::SetNextWindowPos(ImVec2(460, 340), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 220), ImGuiCond_FirstUseEver);
    ImGui::Begin("Log");
    for (auto& line : logLines_) {
        ImGui::TextUnformatted(line.c_str());
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

    RenderDevicesPanel();
    RenderSourcesPanel();
    RenderAndroidPanel();
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
