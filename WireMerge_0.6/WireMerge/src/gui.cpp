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

// Explicit ANSI throughout this file -- see prior note history for why
// (avoids UNICODE-macro-dependent mismatches between narrow/wide Win32 calls).
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

    switch (msg) {
        case WM_SIZE:
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

// ---------------------------------------------------------------------------
// Theme (v0.6 UI overhaul, item 2.7 + intro note 1)
//
// Direction: keep the same TYPE of theme as before (dark base, blue
// accents) but lean into a deliberately plain/"HTML-looking, unfinished"
// look rather than a polished/corporate one -- flat fills (no gradients),
// visible thin borders on everything (like unstyled HTML form controls),
// and only slight corner rounding (a hint of softness, not pill-shaped
// buttons or heavily rounded cards, which reads as "corporate SaaS app").
// ---------------------------------------------------------------------------
void Gui::ApplyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // Spacing: previous "everything is cramped" fix, kept -- still a
    // broad/temporary measure pending further design passes.
    style.ItemSpacing = ImVec2(10, 10);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.FramePadding = ImVec2(8, 5);
    style.WindowPadding = ImVec2(14, 14);
    style.IndentSpacing = 24.0f;

    // Slight rounding only -- explicitly not corporate/pill-shaped.
    style.WindowRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;

    // Visible thin borders everywhere -- this is the main thing that reads
    // as "plain/unstyled HTML form" rather than a modern flat-borderless
    // app look.
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    ImVec4* c = style.Colors;

    // Flat dark background, slightly blue-tinted rather than neutral gray.
    c[ImGuiCol_WindowBg]  = ImVec4(0.10f, 0.11f, 0.14f, 1.00f);
    c[ImGuiCol_ChildBg]   = ImVec4(0.12f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_PopupBg]   = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);

    // Borders: visible, plain mid-gray-blue, not high-contrast/glowy.
    c[ImGuiCol_Border]    = ImVec4(0.30f, 0.33f, 0.40f, 0.60f);

    // Blue accent family for interactive elements -- the "current blue"
    // continuity note. Flat fills, no gradient, distinct hover/active
    // steps rather than a smooth animated feel.
    ImVec4 blue        = ImVec4(0.20f, 0.45f, 0.85f, 1.00f);
    ImVec4 blueHover   = ImVec4(0.30f, 0.55f, 0.95f, 1.00f);
    ImVec4 blueActive  = ImVec4(0.15f, 0.35f, 0.70f, 1.00f);
    ImVec4 blueSubtle  = ImVec4(0.20f, 0.45f, 0.85f, 0.35f);

    c[ImGuiCol_Button]         = blue;
    c[ImGuiCol_ButtonHovered]  = blueHover;
    c[ImGuiCol_ButtonActive]   = blueActive;
    c[ImGuiCol_Header]         = blueSubtle;
    c[ImGuiCol_HeaderHovered]  = ImVec4(blue.x, blue.y, blue.z, 0.55f);
    c[ImGuiCol_HeaderActive]   = ImVec4(blue.x, blue.y, blue.z, 0.75f);
    c[ImGuiCol_FrameBg]        = ImVec4(0.16f, 0.17f, 0.21f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgActive]  = ImVec4(0.22f, 0.25f, 0.32f, 1.00f);
    c[ImGuiCol_CheckMark]      = blueHover;
    c[ImGuiCol_SliderGrab]     = blue;
    c[ImGuiCol_SliderGrabActive] = blueHover;
    c[ImGuiCol_Separator]      = c[ImGuiCol_Border];
    c[ImGuiCol_SeparatorHovered] = blue;
    c[ImGuiCol_SeparatorActive]  = blueHover;
    c[ImGuiCol_Tab]            = ImVec4(0.14f, 0.15f, 0.19f, 1.00f);
    c[ImGuiCol_TabHovered]     = blueHover;
    // ImGuiCol_TabSelected replaces the older ImGuiCol_TabActive name as
    // of imgui v1.90.9 (this project's pinned version) -- see CMakeLists'
    // pinned commit; using the old name here would silently compile
    // against the deprecated redirect rather than the real enumerator.
    c[ImGuiCol_TabSelected]    = blue;
}

bool Gui::Initialize() {
    g_activeGui = this;

    WNDCLASSEXA wc = {
        sizeof(WNDCLASSEXA), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr,
        "WireMergeWindowClass", nullptr
    };
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(0, "WireMergeWindowClass", kWireMergeVersion,
                                 WS_OVERLAPPEDWINDOW, 100, 100, 940, 720,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        WM_LOG_ERROR("Failed to create Win32 window.");
        return false;
    }
    hwnd_ = hwnd;

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

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyTheme();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(Dev(d3dDevice_), Ctx(d3dContext_));

    usb_.StartHotplugMonitor([this](UsbEvent ev, const UsbDeviceInfo& info) {
        std::lock_guard<std::mutex> lock(usbQueueMutex_);
        usbEventQueue_.push_back({ev, info});
    });

    layout_ = TilingLayout::BuildDefaultLayout();

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

void Gui::LogUnderrunSummaryBeforeRemoval(SourceId id, const std::string& sourceName) {
    // Must be called BEFORE Mixer::RemoveSource -- the ring buffer (and
    // its underrun counter) is destroyed along with the source, so this
    // is the last point the total is readable. Item 3.3.
    uint64_t underrunFrames = mixer_.GetUnderrunFrames(id);
    double underrunMs = static_cast<double>(underrunFrames) / 48000.0 * 1000.0;
    PushLogLine("Removed '" + sourceName + "' -- total time spent in underrun (audible "
                "silence gaps) this session: ~" + std::to_string(static_cast<long long>(underrunMs)) + "ms");
}

// ---------------------------------------------------------------------------
// Toolbar (item 2.6): Exit relocated OUT of the tiled panes into a fixed
// top strip alongside branding, so it's clearly app-level chrome rather
// than looking like it belongs to any one pane's content.
// ---------------------------------------------------------------------------
void Gui::RenderToolbar(float& outContentY) {
    ImGuiIO& io = ImGui::GetIO();
    float toolbarHeight = 40.0f;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, toolbarHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##toolbar", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(kWireMergeVersion);

    // Right-aligned Exit -- toolbar-appropriate size (not the previous
    // full-width alarming red button), still a distinct color so it
    // reads as a deliberate/different action from ordinary controls, but
    // scaled and placed like normal toolbar chrome instead of shouting.
    const char* exitLabel = "Exit";
    float exitWidth = ImGui::CalcTextSize(exitLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
    ImGui::SameLine(io.DisplaySize.x - exitWidth - 14.0f);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.50f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.22f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.78f, 0.14f, 0.14f, 1.0f));
    if (ImGui::Button(exitLabel)) {
        RequestExit();
    }
    ImGui::PopStyleColor(3);

    ImGui::End();
    ImGui::PopStyleVar(2);

    outContentY = toolbarHeight;
}

std::string Gui::PaneDisplayName(const std::string& paneId) const {
    if (paneId == "output") return "Output Device";
    if (paneId == "inputs") return "Inputs";
    if (paneId == "sources") return "Active Sources";
    if (paneId == "log") return "Log";
    return paneId;
}

void Gui::RenderPane(const std::string& paneId, const PaneRenderContext& ctx) {
    if (paneId == "output") RenderOutputContent(ctx);
    else if (paneId == "inputs") RenderInputsContent(ctx);
    else if (paneId == "sources") RenderSourcesContent(ctx);
    else if (paneId == "log") RenderLogContent(ctx);
}

void Gui::RenderOutputContent(const PaneRenderContext& /*ctx*/) {
    ImGui::SetNextItemWidth(200.0f);
    static float masterVolume = 1.0f;
    if (ImGui::SliderFloat("Master Volume", &masterVolume, 0.0f, 1.5f, "%.2f")) {
        mixer_.SetMasterVolume(masterVolume);
    }
    ImGui::Separator();

    auto outputs = audio_.ListOutputDevices();
    std::string preview = selectedOutputDevice_ >= 0 ? "Selected" : "Choose output...";
    for (auto& d : outputs) {
        if (d.index == selectedOutputDevice_) preview = d.name;
    }

    // Item 2.3/2.4 fix: previously the combo used its default (non-hidden)
    // inline label drawn to the right of the widget, WHILE ALSO being
    // stretched to -1 (fill available width) -- leaving zero room for
    // that label, which is what clipped a letter off "Output"/"Input"
    // right next to the dropdown. Fix: hide the inline label ("##Output")
    // and draw it as its own line above instead, which also can't break
    // regardless of how narrow the pane gets resized to.
    ImGui::TextUnformatted("Output");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##Output", preview.c_str())) {
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
}

void Gui::RenderInputsContent_PcSubsection() {
    static int selectedInput = -1;
    auto inputs = audio_.ListInputDevices();
    std::string inPreview = "Choose input...";
    for (auto& d : inputs) if (d.index == selectedInput) inPreview = d.name;

    // Same label-cutoff fix as the Output combo -- see that function's
    // comment for the full explanation.
    ImGui::TextUnformatted("Input");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##Input", inPreview.c_str())) {
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
        if (id != 0) {
            std::string name = "input source";
            for (auto& d : inputs) if (d.index == selectedInput) name = d.name;
            PushLogLine("Regulated input added: " + name + ".");
        } else {
            PushLogLine("Failed to add regulated input -- check log file.");
        }
    }
    ImGui::EndDisabled();

    if (ImGui::Button("Rescan Devices")) {
        PushLogLine("Regulated input devices rescanned.");
    }
}

void Gui::RenderInputsContent_AndroidSubsection() {
    if (!adb_.IsAvailable()) {
        // Kept short and factual -- matches the trimmed style of the PC
        // subsection rather than the previous multi-sentence explainer.
        ImGui::TextWrapped("Not set up: adb.exe / sndcpy.apk missing from "
                            "'tools' (see Log / README).");
        return;
    }

    // Item 2.5: cut down to length/style matching the PC subsection's
    // single short line, instead of the previous multi-sentence explainer
    // -- the fuller explanation now lives in the README rather than the
    // GUI itself.
    ImGui::TextWrapped("Captures phone app audio (e.g. Spotify) over USB. "
                        "Requires a one-time on-device permission per capture.");

    static std::string selectedSerial;

    constexpr double kRescanIntervalMs = 2000.0;
    static std::vector<AdbDeviceInfo> cachedDevices;
    static double lastScanTime = -kRescanIntervalMs;

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
        ImGui::TextDisabled("No devices detected.");
    }

    for (auto& d : cachedDevices) {
        bool isSelected = (d.serial == selectedSerial);
        std::string label = d.serial + " [" + d.state + "]";
        if (ImGui::RadioButton(label.c_str(), isSelected)) {
            selectedSerial = d.serial;
        }
    }

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
        adb_.StartCaptureAsync(mixer_, selectedSerial);
        PushLogLine("Starting Android capture for " + selectedSerial + "...");
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
                            "Starting on %s -- check phone for prompt.", selectedSerial.c_str());
    }
}

void Gui::RenderInputsContent(const PaneRenderContext& /*ctx*/) {
    if (ImGui::CollapsingHeader("Regulated Inputs to PC", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        RenderInputsContent_PcSubsection();
        ImGui::Unindent();
    }

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 12));

    if (ImGui::CollapsingHeader("Devices (Android)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        RenderInputsContent_AndroidSubsection();
        ImGui::Unindent();
    }
}

void Gui::RenderSourcesContent(const PaneRenderContext& /*ctx*/) {
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
        if (ImGui::SliderFloat("Trim", &volume, 0.0f, 2.0f, "%.2f")) {
            mixer_.SetVolume(s.id, volume);
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            // 3.3: log the underrun summary BEFORE removing -- the ring
            // buffer (and its counter) goes away with the source.
            LogUnderrunSummaryBeforeRemoval(s.id, s.name);
            audio_.CloseInputSource(s.id);
            mixer_.RemoveSource(s.id);
        }

        uint64_t underrunFrames = mixer_.GetUnderrunFrames(s.id);
        double underrunMs = static_cast<double>(underrunFrames) / 48000.0 * 1000.0;
        ImVec4 color = underrunMs < 50.0 ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f)
                      : underrunMs < 500.0 ? ImVec4(0.9f, 0.7f, 0.2f, 1.0f)
                      : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
        ImGui::TextColored(color, "Underruns: ~%.0fms of silence total", underrunMs);

        ImGui::Separator();
        ImGui::PopID();
    }
}

void Gui::RenderLogContent(const PaneRenderContext& /*ctx*/) {
    for (auto& line : logLines_) {
        ImGui::TextWrapped("%s", line.c_str());
    }
    if (!logLines_.empty()) ImGui::SetScrollHereY(1.0f);
}

void Gui::HandleResize(unsigned int width, unsigned int height) {
    if (!d3dDevice_ || !d3dContext_ || !swapChain_) return;
    if (width == 0 || height == 0) return;

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
    if (hwnd_ && IsIconic(static_cast<HWND>(hwnd_))) {
        Sleep(50);
        return;
    }

    DrainUsbEventQueue();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();

    float contentY = 0.0f;
    RenderToolbar(contentY);

    // The tiling layout fills everything below the toolbar, and is
    // recomputed from the CURRENT window size every frame -- this is what
    // makes panes automatically resize/adapt when the main window itself
    // is resized (item 2.1), with no special-casing needed: there's no
    // stored pixel geometry for panes, only ratios within the tree.
    ImGui::SetNextWindowPos(ImVec2(0, contentY));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y - contentY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##layout_root", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                  ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                  ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (layout_) {
        TilingLayout::Render(*layout_, 0, contentY, io.DisplaySize.x, io.DisplaySize.y - contentY,
                              [this](const std::string& paneId, const PaneRenderContext& ctx) {
                                  RenderPane(paneId, ctx);
                              },
                              [this](const std::string& paneId) {
                                  return PaneDisplayName(paneId);
                              });
    }

    ImGui::End();
    ImGui::PopStyleVar(3);

    ImGui::Render();

    const float clearColor[4] = {0.08f, 0.08f, 0.10f, 1.0f};
    Ctx(d3dContext_)->OMSetRenderTargets(1, reinterpret_cast<ID3D11RenderTargetView* const*>(&renderTargetView_), nullptr);
    Ctx(d3dContext_)->ClearRenderTargetView(RTV(renderTargetView_), clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    Swap(swapChain_)->Present(1, 0);
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
