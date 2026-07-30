#include "gui.h"
#include "utils.h"
#include <algorithm>

#include <d3d11.h>
#include <windows.h>
#include <psapi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace wm {

static void DrawSkeletalDropdownArrow() {
    ImVec2 itemMin = ImGui::GetItemRectMin();
    ImVec2 itemMax = ImGui::GetItemRectMax();
    float cy = (itemMin.y + itemMax.y) * 0.5f;
    float cx = itemMax.x - 18.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 backdropCol = ImGui::GetColorU32(ImGuiCol_FrameBg);
    dl->AddRectFilled(ImVec2(itemMax.x - 30.0f, itemMin.y + 1.0f),
                       ImVec2(itemMax.x - 1.0f, itemMax.y - 1.0f), backdropCol);

    float arrowHalfWidth = 4.0f;
    ImU32 arrowCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dl->AddLine(ImVec2(cx - arrowHalfWidth, cy - 2.0f), ImVec2(cx, cy + 2.0f), arrowCol, 1.5f);
    dl->AddLine(ImVec2(cx, cy + 2.0f), ImVec2(cx + arrowHalfWidth, cy - 2.0f), arrowCol, 1.5f);
}

static void DrawSkeletalChevron(ImVec2 center, bool expanded) {
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float s = 4.0f;
    if (expanded) {
        dl->AddLine(ImVec2(center.x - s, center.y - s * 0.5f), ImVec2(center.x, center.y + s * 0.5f), col, 1.5f);
        dl->AddLine(ImVec2(center.x, center.y + s * 0.5f), ImVec2(center.x + s, center.y - s * 0.5f), col, 1.5f);
    } else {
        dl->AddLine(ImVec2(center.x - s * 0.5f, center.y - s), ImVec2(center.x + s * 0.5f, center.y), col, 1.5f);
        dl->AddLine(ImVec2(center.x + s * 0.5f, center.y), ImVec2(center.x - s * 0.5f, center.y + s), col, 1.5f);
    }
}

static float SubsectionTitleFontSize() {
    return ImGui::GetFontSize() + 1.0f;
}

static float SubsectionHeaderRowHeight() {
    constexpr float kHeaderPadY = 3.0f;
    return SubsectionTitleFontSize() + kHeaderPadY * 2.0f;
}

static bool RenderSubsectionHeader(const char* label, bool& expanded) {
    constexpr float kChevronIndent = 8.0f;
    constexpr float kTextIndent = 18.0f;
    float rowHeight = SubsectionHeaderRowHeight();

    ImGui::PushID(label);
    ImVec2 rowMin = ImGui::GetCursorScreenPos();
    float rowWidth = ImGui::GetContentRegionAvail().x;
    ImVec2 rowMax(rowMin.x + rowWidth, rowMin.y + rowHeight);

    ImGui::InvisibleButton("##subsection_header", ImVec2(rowWidth, rowHeight));
    bool clicked = ImGui::IsItemClicked();
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    if (clicked) expanded = !expanded;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered || active) {
        ImU32 highlightCol = ImGui::GetColorU32(active ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered);
        dl->AddRectFilled(rowMin, rowMax, highlightCol, ImGui::GetStyle().FrameRounding);
    }

    float chevronX = rowMin.x + kChevronIndent;
    float rowCenterY = rowMin.y + rowHeight * 0.5f;
    DrawSkeletalChevron(ImVec2(chevronX, rowCenterY), expanded);

    float titleFontSize = SubsectionTitleFontSize();
    ImVec2 textPos(rowMin.x + kTextIndent, rowMin.y + (rowHeight - titleFontSize) * 0.5f);
    dl->AddText(ImGui::GetFont(), titleFontSize, textPos, ImGui::GetColorU32(ImGuiCol_Text), label);

    ImGui::PopID();
    return expanded;
}

static ID3D11Device* Dev(void* p) { return static_cast<ID3D11Device*>(p); }
static ID3D11DeviceContext* Ctx(void* p) { return static_cast<ID3D11DeviceContext*>(p); }
static IDXGISwapChain* Swap(void* p) { return static_cast<IDXGISwapChain*>(p); }
static ID3D11RenderTargetView* RTV(void* p) { return static_cast<ID3D11RenderTargetView*>(p); }

static Gui* g_activeGui = nullptr;

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

void Gui::ApplyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    style.ItemSpacing = ImVec2(10, 10);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.FramePadding = ImVec2(12, 8);
    style.WindowPadding = ImVec2(16, 16);
    style.IndentSpacing = 24.0f;

    style.WindowRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 2.5f;
    style.TabRounding = 4.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    ImVec4* c = style.Colors;

    c[ImGuiCol_WindowBg]  = ImVec4(0.085f, 0.095f, 0.117f, 1.00f);
    c[ImGuiCol_ChildBg]   = ImVec4(0.110f, 0.120f, 0.148f, 1.00f);
    c[ImGuiCol_PopupBg]   = ImVec4(0.125f, 0.137f, 0.165f, 1.00f);

    c[ImGuiCol_Border]    = ImVec4(0.30f, 0.33f, 0.40f, 0.60f);

    ImVec4 blue        = ImVec4(0.20f, 0.45f, 0.85f, 1.00f);
    ImVec4 blueHover   = ImVec4(0.30f, 0.55f, 0.95f, 1.00f);
    ImVec4 blueActive  = ImVec4(0.15f, 0.35f, 0.70f, 1.00f);

    c[ImGuiCol_Button]         = blue;
    c[ImGuiCol_ButtonHovered]  = blueHover;
    c[ImGuiCol_ButtonActive]   = blueActive;
    c[ImGuiCol_Header]         = ImVec4(0.20f, 0.45f, 0.85f, 0.35f);
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
    c[ImGuiCol_TabSelected]    = blue;
}

bool Gui::Initialize() {
    g_activeGui = this;

    HICON appIcon = LoadIconA(GetModuleHandleA(nullptr), "IDI_ICON1");

    WNDCLASSEXA wc = {
        sizeof(WNDCLASSEXA), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandleA(nullptr), appIcon, nullptr, nullptr, nullptr,
        "WireMergeWindowClass", appIcon
    };
    RegisterClassExA(&wc);

    constexpr int kWindowWidth = 1280;
    constexpr int kWindowHeight = 720;
    RECT workArea{};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0);
    int workWidth = workArea.right - workArea.left;
    int workHeight = workArea.bottom - workArea.top;
    int windowX = workArea.left + std::max(0, (workWidth - kWindowWidth) / 2);
    int windowY = workArea.top + std::max(0, (workHeight - kWindowHeight) / 2);

    HWND hwnd = CreateWindowExA(0, "WireMergeWindowClass", kWireMergeVersion,
                                 WS_OVERLAPPEDWINDOW, windowX, windowY, kWindowWidth, kWindowHeight,
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
    sourcesSplitNode_ = TilingLayout::FindPaneParent(*layout_, "sources");

    for (auto& line : UiLog::Instance().DrainAll()) {
        PushLogLine(line);
    }

    running_ = true;
    return true;
}

void Gui::PushLogLine(const std::string& line) {
    logLines_.push_back(line);
    if (logLines_.size() > 200) logLines_.pop_front();
    ++totalLogLinesPushed_;
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
    uint64_t underrunFrames = mixer_.GetUnderrunFrames(id);
    double underrunMs = static_cast<double>(underrunFrames) / 48000.0 * 1000.0;
    PushLogLine("Removed '" + sourceName + "'. Total time spent in underrun (audible "
                "silence gaps) this session: ~" + std::to_string(static_cast<long long>(underrunMs)) + "ms");
}

void Gui::RenderToolbar(float& outContentY) {
    ImGuiIO& io = ImGui::GetIO();
    float toolbarHeight = 68.0f;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, toolbarHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
    ImGui::Begin("##toolbar", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted(kWireMergeVersion);
    ImGui::SetWindowFontScale(1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.65f, 0.70f, 1.0f));
    ImGui::TextUnformatted("Lightweight USB & Android audio router");
    ImGui::PopStyleColor();

    const char* exitLabel = "Exit WireMerge";
    ImVec2 exitSize = ImGui::CalcTextSize(exitLabel);
    ImVec2 exitPad = ImVec2(ImGui::GetStyle().FramePadding.x + 8.0f,
                             ImGui::GetStyle().FramePadding.y + 6.0f);
    float exitWidth = exitSize.x + exitPad.x * 2.0f;
    float exitHeight = exitSize.y + exitPad.y * 2.0f;

    ImGui::SetCursorPos(ImVec2(io.DisplaySize.x - exitWidth - 24.0f,
                                (toolbarHeight - exitHeight) * 0.5f + 4.0f));

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.16f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.21f, 0.21f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.50f, 0.11f, 0.11f, 1.0f));
    bool exitClicked = ImGui::Button(exitLabel, ImVec2(exitWidth, exitHeight));
    ImGui::PopStyleColor(3);
    if (exitClicked) {
        RequestExit();
    }

    const char* perfLabel = "Performance";
    ImVec2 perfSize = ImGui::CalcTextSize(perfLabel);
    float perfWidth = perfSize.x + exitPad.x * 2.0f;
    ImGui::SetCursorPos(ImVec2(io.DisplaySize.x - exitWidth - 24.0f - perfWidth - 12.0f,
                                (toolbarHeight - exitHeight) * 0.5f + 4.0f));
    if (ImGui::Button(perfLabel, ImVec2(perfWidth, exitHeight))) {
        showPerfWindow_ = !showPerfWindow_;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    ImU32 borderCol = ImGui::GetColorU32(ImGuiCol_Border);
    drawList->AddLine(ImVec2(winPos.x, winPos.y + toolbarHeight),
                       ImVec2(winPos.x + io.DisplaySize.x, winPos.y + toolbarHeight),
                       borderCol, 1.0f);

    ImGui::End();
    ImGui::PopStyleVar(3);

    outContentY = toolbarHeight;
}

std::string Gui::PaneDisplayName(const std::string& paneId) const {
    if (paneId == "output") return "Output Device";
    if (paneId == "inputs") return "Inputs";
    if (paneId == "devices") return "Devices";
    if (paneId == "sources") return "Active Sources";
    if (paneId == "log") return "Log";
    return paneId;
}

void Gui::RenderPane(const std::string& paneId, const PaneRenderContext& ctx) {
    if (paneId == "output") RenderOutputContent(ctx);
    else if (paneId == "inputs") RenderInputsContent(ctx);
    else if (paneId == "devices") RenderDevicesContent(ctx);
    else if (paneId == "sources") RenderSourcesContent(ctx);
    else if (paneId == "log") RenderLogContent(ctx);
}

void Gui::RenderOutputContent(const PaneRenderContext& /*ctx*/) {
    static float masterVolume = 1.0f;

    constexpr float kVolumeLabelTrailingMargin = 8.0f;
    float labelWidth = ImGui::CalcTextSize("Master Volume").x;
    ImGui::SetNextItemWidth(-(labelWidth + ImGui::GetStyle().ItemInnerSpacing.x + kVolumeLabelTrailingMargin));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    bool volumeChanged = ImGui::SliderFloat("Master Volume", &masterVolume, 0.0f, 1.5f, "%.2f");
    ImGui::PopStyleVar();
    if (volumeChanged) {
        mixer_.SetMasterVolume(masterVolume);
    }
    ImGui::Separator();

    ImGui::Dummy(ImVec2(0, 6.0f));

    constexpr double kOutputListRefreshMs = 2000.0;
    static std::vector<AudioDeviceInfo> cachedOutputs;
    static double lastOutputListRefresh = -kOutputListRefreshMs;
    double outputsNowMs = ImGui::GetTime() * 1000.0;
    if (outputsNowMs - lastOutputListRefresh >= kOutputListRefreshMs) {
        cachedOutputs = audio_.ListOutputDevices();
        lastOutputListRefresh = outputsNowMs;
    }
    auto& outputs = cachedOutputs;
    std::string preview = selectedOutputDevice_ >= 0 ? "Selected" : "Choose output...";
    for (auto& d : outputs) {
        if (d.index == selectedOutputDevice_) preview = d.name;
    }

    ImGui::TextUnformatted("Output");
    ImGui::SetNextItemWidth(-1);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(11.0f, 11.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    bool comboOpened = ImGui::BeginCombo("##Output", preview.c_str(), ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleVar(2);
    DrawSkeletalDropdownArrow();
    if (comboOpened) {
        for (auto& d : outputs) {
            bool selected = (d.index == selectedOutputDevice_);
            std::string label = d.name + (d.isDefaultOutput ? " (default)" : "");
            if (ImGui::Selectable(label.c_str(), selected)) {
                if (d.index != selectedOutputDevice_) {
                    selectedOutputDevice_ = d.index;
                    if (outputOpen_) {
                        audio_.CloseOutput();
                        if (audio_.OpenOutput(mixer_, selectedOutputDevice_)) {
                            PushLogLine("Output switched to " + d.name + ".");
                        } else {
                            outputOpen_ = false;
                            PushLogLine("Failed to switch output to " + d.name + ". Check log file for details.");
                        }
                    }
                }
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Dummy(ImVec2(0, 8.0f));

    ImGui::BeginDisabled(selectedOutputDevice_ < 0 || outputOpen_ || audio_.IsRescanInProgress());
    if (ImGui::Button("Start Output")) {
        if (audio_.OpenOutput(mixer_, selectedOutputDevice_)) {
            outputOpen_ = true;
            PushLogLine("Output started.");
        } else {
            PushLogLine("Failed to start output. Check log file for details.");
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

void Gui::RenderInputsContent(const PaneRenderContext& /*ctx*/) {
    ImVec4 subsectionBg(0.095f, 0.105f, 0.130f, 1.0f);
    static bool pcExpanded = false;
    static float pcContentHeight = 150.0f;

    float padY = ImGui::GetStyle().WindowPadding.y;
    float collapsedHeight = SubsectionHeaderRowHeight() + padY * 2.0f;
    float expandedHeight = pcContentHeight + padY;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, subsectionBg);
    ImGui::BeginChild("##pc_subsection", ImVec2(-1, pcExpanded ? expandedHeight : collapsedHeight), ImGuiChildFlags_Border);

    RenderSubsectionHeader("Regulated Inputs to PC", pcExpanded);

    if (pcExpanded) {
        ImGui::Dummy(ImVec2(0, 4.0f));
        static int selectedInput = -1;

        constexpr double kInputListRefreshMs = 2000.0;
        static std::vector<AudioDeviceInfo> cachedInputs;
        static double lastInputListRefresh = -kInputListRefreshMs;
        double nowMs = ImGui::GetTime() * 1000.0;
        if (nowMs - lastInputListRefresh >= kInputListRefreshMs) {
            cachedInputs = audio_.ListInputDevices();
            lastInputListRefresh = nowMs;
        }
        bool rescanSucceeded = false;
        if (audio_.TryTakeRescanResult(rescanSucceeded)) {
            if (rescanSucceeded) {
                cachedInputs = audio_.ListInputDevices();
                lastInputListRefresh = nowMs;
                PushLogLine("Regulated input devices rescanned.");
            } else {
                PushLogLine("Rescan failed to reinitialize PortAudio. Check log file.");
            }
        }

        auto& inputs = cachedInputs;
        std::string inPreview = "Choose input...";
        for (auto& d : inputs) if (d.index == selectedInput) inPreview = d.name;

        ImGui::TextUnformatted("Input");
        ImGui::SetNextItemWidth(-1);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(11.0f, 11.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        bool inputComboOpened = ImGui::BeginCombo("##Input", inPreview.c_str(), ImGuiComboFlags_NoArrowButton);
        ImGui::PopStyleVar(2);
        DrawSkeletalDropdownArrow();
        if (inputComboOpened) {
            for (auto& d : inputs) {
                bool selected = (d.index == selectedInput);
                std::string label = d.name + (d.isDefaultInput ? " (default)" : "");
                if (ImGui::Selectable(label.c_str(), selected)) selectedInput = d.index;
            }
            ImGui::EndCombo();
        }

        ImGui::Dummy(ImVec2(0, 4.2f));

        ImGui::BeginDisabled(selectedInput < 0 || audio_.IsRescanInProgress());
        if (ImGui::Button("Add Source")) {
            SourceId id = audio_.OpenInputSource(mixer_, selectedInput);
            if (id != 0) {
                std::string name = "input source";
                for (auto& d : inputs) if (d.index == selectedInput) name = d.name;
                PushLogLine("Regulated input added: " + name + ".");
            } else {
                PushLogLine("Failed to add regulated input. Check log file.");
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(audio_.IsRescanInProgress());
        if (ImGui::Button("Rescan Devices")) {
            if (!audio_.RescanDevicesAsync()) {
                PushLogLine("Rescan needs Output and all Sources stopped first "
                            "(rescanning reinitializes PortAudio).");
            }
        }
        ImGui::EndDisabled();
        if (audio_.IsRescanInProgress()) {
            ImGui::SameLine();
            ImGui::TextUnformatted("Rescanning...");
        }

        pcContentHeight = ImGui::GetCursorPosY();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 4.0f));

    RenderDevicesContent({});
}

void Gui::RenderDevicesContent(const PaneRenderContext& /*ctx*/) {
    ImVec4 subsectionBg(0.095f, 0.105f, 0.130f, 1.0f);
    static bool androidExpanded = true;
    static float androidContentHeight = 190.0f;

    float padY = ImGui::GetStyle().WindowPadding.y;
    float collapsedHeight = SubsectionHeaderRowHeight() + padY * 2.0f;
    float expandedHeight = androidContentHeight + padY;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, subsectionBg);
    ImGui::BeginChild("##android_subsection", ImVec2(-1, androidExpanded ? expandedHeight : collapsedHeight), ImGuiChildFlags_Border);

    RenderSubsectionHeader("Devices (Android)", androidExpanded);

    if (androidExpanded) {
        ImGui::Dummy(ImVec2(0, 4.0f));
        if (!adb_.IsAvailable()) {
            if (adb_.IsDownloadInProgress()) {
                ImGui::TextWrapped("Downloading adb.exe / sndcpy.apk...");
            } else {
                ImGui::TextWrapped("Not set up: adb.exe / sndcpy.apk missing from "
                                    "'tools' (see Log / README).");
                ImGui::Dummy(ImVec2(0, 4.0f));
                if (ImGui::Button("Download Tools")) {
                    showToolsDownloadPrompt_ = true;
                }
            }
            androidContentHeight = ImGui::GetCursorPosY();
            ImGui::EndChild();
            ImGui::PopStyleColor();
            return;
        }

        ImGui::TextWrapped("Captures phone app audio (e.g. Spotify) over USB.\n"
                            "Requires a one-time on-device permission per capture.");

        ImGui::Dummy(ImVec2(0, 2.8f));

        constexpr double kRescanIntervalMs = 2000.0;
        static std::vector<AdbDeviceInfo> cachedDevices;
        static double lastScanTime = -kRescanIntervalMs;

        double now = ImGui::GetTime() * 1000.0;
        if (now - lastScanTime >= kRescanIntervalMs) {
            adb_.RequestDeviceScan();
            lastScanTime = now;
        }
        std::vector<AdbDeviceInfo> scannedDevices;
        if (adb_.TryTakeDeviceScanResult(scannedDevices)) {
            cachedDevices = std::move(scannedDevices);
        }

        {
            ImVec2 mainPad = ImGui::GetStyle().FramePadding;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(mainPad.x * 0.7f, mainPad.y * 0.7f));
            if (ImGui::Button("Rescan Now")) {
                adb_.RequestDeviceScan();
                lastScanTime = now;
                PushLogLine("Android device rescan started.");
            }
            ImGui::PopStyleVar();
        }

        ImGui::Dummy(ImVec2(0, 2.8f));

        static std::string selectedSerial;
        if (cachedDevices.empty()) {
            ImGui::TextDisabled("No devices detected.");
        } else {
            for (auto& d : cachedDevices) {
                bool isSelected = (d.serial == selectedSerial);
                std::string label = d.serial + " [" + d.state + "]";
                if (ImGui::RadioButton(label.c_str(), isSelected)) {
                    selectedSerial = d.serial;
                }
            }
        }

        for (auto& d : cachedDevices) {
            SourceId result;
            if (adb_.TryTakeStartResult(d.serial, result)) {
                if (result != 0) {
                    PushLogLine("Started Android capture for " + d.serial + " (source added).");
                } else {
                    PushLogLine("Failed to start Android capture for " + d.serial +
                                ". Check log file for details.");
                }
            }
        }

        bool starting = !selectedSerial.empty() && adb_.IsStarting(selectedSerial);
        bool canStart = !selectedSerial.empty() && !starting;

        ImGui::Dummy(ImVec2(0, 2.8f));

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
                                "Starting on %s. Check phone for prompt.", selectedSerial.c_str());
        }

        androidContentHeight = ImGui::GetCursorPosY();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void Gui::RenderSourcesContent(const PaneRenderContext& /*ctx*/) {
    auto sources = mixer_.ListSources();
    if (sources.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 p0 = ImGui::GetCursorScreenPos();

        const char* line1 = "No active sources";
        const char* line2 = "Add a regulated input or start Android capture to begin";
        ImVec2 size1 = ImGui::CalcTextSize(line1);
        ImVec2 size2 = ImGui::CalcTextSize(line2);
        float centerY = p0.y + (std::max(80.0f, avail.y) - size1.y - size2.y - 8.0f) * 0.5f;
        ImU32 mutedCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddText(ImVec2(p0.x + (avail.x - size1.x) * 0.5f, centerY), mutedCol, line1);
        drawList->AddText(ImVec2(p0.x + (avail.x - size2.x) * 0.5f, centerY + size1.y + 8.0f),
                           mutedCol, line2);

        ImGui::Dummy(ImVec2(avail.x, std::max(80.0f, avail.y)));
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
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        bool trimChanged = ImGui::SliderFloat("Trim", &volume, 0.0f, 2.0f, "%.2f");
        ImGui::PopStyleVar();
        if (trimChanged) {
            mixer_.SetVolume(s.id, volume);
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
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
        ImGui::Dummy(ImVec2(0, 8.0f));
        ImGui::PopID();
    }

    if (!sourcesRatioSettled_ && sourcesSplitNode_) {
        float scrollMax = ImGui::GetScrollMaxY();
        if (scrollMax > 1.0f) {
            float paneHeight = std::max(1.0f, ImGui::GetWindowHeight());
            float totalColumnHeight = paneHeight / std::max(0.01f, sourcesSplitNode_->ratio);
            float ratioStep = 6.0f / totalColumnHeight;
            sourcesSplitNode_->ratio = std::min(sourcesSplitNode_->ratio + ratioStep, 0.80f);
        } else {
            sourcesRatioSettled_ = true;
        }
    }
}

void Gui::RenderLogContent(const PaneRenderContext& /*ctx*/) {
    ImGui::BeginChild("##log_content", ImVec2(-1, -1), ImGuiChildFlags_Border);

    size_t pushedCount = totalLogLinesPushed_;
    bool linesAdded = pushedCount != lastSeenLogLineCount_;

    for (auto& line : logLines_) {
        ImGui::TextWrapped("%s", line.c_str());
    }

    if (linesAdded) {
        if (wasAtBottomLastFrame_) {
            ImGui::SetScrollHereY(1.0f);
        }
        lastSeenLogLineCount_ = pushedCount;
    }

    wasAtBottomLastFrame_ = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

    ImGui::EndChild();
}

void Gui::RenderToolsDownloadPrompt() {
    if (!loggedInitialToolsCheck_ && adb_.IsAsyncInitDone()) {
        loggedInitialToolsCheck_ = true;
        if (adb_.IsAvailable()) {
            PushLogLine("Android capture tools found in tools/. Android capture is ready.");
        } else {
            PushLogLine("Android capture tools not found in tools/. Android capture is unavailable "
                        "until installed.");
        }
    }

    bool isDownloading = adb_.IsDownloadInProgress();
    if (wasDownloadInProgress_ && !isDownloading) {
        if (adb_.IsAvailable()) {
            PushLogLine("Android capture tools installed successfully. Android capture is ready.");
        } else {
            PushLogLine("Android capture tools installation failed. See Log for detail, or try "
                        "Download Tools again.");
        }
    }
    wasDownloadInProgress_ = isDownloading;

    if (!toolsPromptShown_ && adb_.IsAsyncInitDone() && !adb_.IsAvailable() && !adb_.IsDownloadInProgress()) {
        toolsPromptShown_ = true;
        showToolsDownloadPrompt_ = true;
    }

    if (showToolsDownloadPrompt_) {
        ImGui::OpenPopup("Download Android Capture Tools?");
        showToolsDownloadPrompt_ = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Download Android Capture Tools?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        RegisterFooterOccluder();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
        ImGui::TextWrapped("WireMerge can download the small ADB + sndcpy tools needed for "
                            "Android app-audio capture (phone audio over USB) from the internet.");
        ImGui::TextWrapped("This is entirely optional. Everything else (USB mic/DAC input and "
                            "output) works without it.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 10.0f));

        if (ImGui::Button("Download", ImVec2(120, 0))) {
            adb_.DownloadToolsAsync();
            PushLogLine("Downloading Android capture tools (adb.exe + sndcpy.apk)...");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Not Now", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Gui::RegisterFooterOccluder() {
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    footerOccluderRects_.push_back({pos.x, pos.y, pos.x + size.x, pos.y + size.y});
}

void Gui::DrawFooterTextClipped(ImDrawList* dl, ImFont* font, float fontSize,
                                 ImVec2 pos, ImVec2 size, ImU32 color, const char* text) {
    struct Span { float minX, maxX; };
    std::vector<Span> visible = {{pos.x, pos.x + size.x}};
    for (auto& r : footerOccluderRects_) {
        bool verticallyOverlaps = !(pos.y + size.y < r.minY || pos.y > r.maxY);
        if (!verticallyOverlaps) continue;
        std::vector<Span> next;
        for (auto& s : visible) {
            if (r.maxX <= s.minX || r.minX >= s.maxX) {
                next.push_back(s);
                continue;
            }
            if (r.minX > s.minX) next.push_back({s.minX, std::min(r.minX, s.maxX)});
            if (r.maxX < s.maxX) next.push_back({std::max(r.maxX, s.minX), s.maxX});
        }
        visible = next;
    }
    for (auto& s : visible) {
        if (s.maxX <= s.minX) continue;
        dl->PushClipRect(ImVec2(s.minX, pos.y - 2.0f), ImVec2(s.maxX, pos.y + size.y + 2.0f), true);
        dl->AddText(font, fontSize, pos, color, text);
        dl->PopClipRect();
    }
}

static void DrawStatRow(const char* label, const char* valueText, const char* detailText = nullptr) {
    ImGui::Text("%s: %s", label, valueText);
    if (detailText && detailText[0] != '\0') {
        float targetSize = std::max(1.0f, ImGui::GetFontSize() - 1.0f);
        ImGui::SetWindowFontScale(targetSize / ImGui::GetFontSize());
        ImGui::TextDisabled("%s", detailText);
        ImGui::SetWindowFontScale(1.0f);
    }
}

void Gui::RenderPerformanceWindow() {
    if (!showPerfWindow_) return;

    ImGuiIO& io = ImGui::GetIO();

    double frameTimeMs = static_cast<double>(io.DeltaTime) * 1000.0;
    constexpr double kFrameEmaAlpha = 0.05;
    perfFrameTimeMsAvg_ = (perfFrameTimeMsAvg_ <= 0.0)
        ? frameTimeMs
        : (perfFrameTimeMsAvg_ * (1.0 - kFrameEmaAlpha) + frameTimeMs * kFrameEmaAlpha);

    double nowMs = ImGui::GetTime() * 1000.0;
    constexpr double kSampleIntervalMs = 500.0;
    if (nowMs - perfLastSampleMs_ >= kSampleIntervalMs) {
        FILETIME creationTime, exitTime, kernelTime, userTime;
        if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime)) {
            ULARGE_INTEGER kernel{}, user{};
            kernel.LowPart = kernelTime.dwLowDateTime;
            kernel.HighPart = kernelTime.dwHighDateTime;
            user.LowPart = userTime.dwLowDateTime;
            user.HighPart = userTime.dwHighDateTime;
            uint64_t totalCpu100ns = kernel.QuadPart + user.QuadPart;

            if (perfPrevWallMs_ > 0.0) {
                double wallDeltaMs = nowMs - perfPrevWallMs_;
                uint64_t cpuDelta100ns = totalCpu100ns - (perfPrevKernelTime100ns_ + perfPrevUserTime100ns_);
                double cpuDeltaMs = static_cast<double>(cpuDelta100ns) / 10000.0;
                SYSTEM_INFO sysInfo;
                GetSystemInfo(&sysInfo);
                double numCores = static_cast<double>(std::max<DWORD>(1, sysInfo.dwNumberOfProcessors));
                if (wallDeltaMs > 0.0) {
                    perfCpuPercentCur_ = std::clamp((cpuDeltaMs / (wallDeltaMs * numCores)) * 100.0, 0.0, 100.0);
                    constexpr double kCpuEmaAlpha = 0.3;
                    perfCpuPercentAvg_ = (perfCpuPercentAvg_ <= 0.0)
                        ? perfCpuPercentCur_
                        : (perfCpuPercentAvg_ * (1.0 - kCpuEmaAlpha) + perfCpuPercentCur_ * kCpuEmaAlpha);
                }
            }
            perfPrevKernelTime100ns_ = kernel.QuadPart;
            perfPrevUserTime100ns_ = user.QuadPart;
            perfPrevWallMs_ = nowMs;

            FILETIME nowFt;
            GetSystemTimeAsFileTime(&nowFt);
            ULARGE_INTEGER created{}, now{};
            created.LowPart = creationTime.dwLowDateTime;
            created.HighPart = creationTime.dwHighDateTime;
            now.LowPart = nowFt.dwLowDateTime;
            now.HighPart = nowFt.dwHighDateTime;
            perfUptimeSeconds_ = static_cast<double>(now.QuadPart - created.QuadPart) / 10000000.0;
        }

        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                  reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            perfWorkingSetMB_ = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
            perfPeakWorkingSetMB_ = static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
            perfPrivateBytesMB_ = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
            perfPageFaultCount_ = pmc.PageFaultCount;
        }

        DWORD handleCount = 0;
        if (GetProcessHandleCount(GetCurrentProcess(), &handleCount)) {
            perfHandleCount_ = handleCount;
        }
        perfGdiObjectCount_ = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        perfUserObjectCount_ = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);

        perfLastSampleMs_ = nowMs;
    }

    constexpr float kPerfSpawnMarginX = 20.0f;
    constexpr float kPerfSpawnMarginY = 20.0f;
    constexpr float kPerfSpawnToolbarHeight = 68.0f;
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x - kPerfSpawnMarginX, kPerfSpawnToolbarHeight + kPerfSpawnMarginY),
        ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));

    constexpr float kPerfBoxPad = 16.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPerfBoxPad, kPerfBoxPad));
    ImGui::Begin("Performance", &showPerfWindow_,
                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    RegisterFooterOccluder();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));

    constexpr float kPerfDividerGap = 6.0f;
    auto PerfDivider = [&]() {
        ImGui::Dummy(ImVec2(0.0f, kPerfDividerGap));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, kPerfDividerGap));
    };
    {
        char val[64], detail[96];

        snprintf(val, sizeof(val), "%.1f%%", perfCpuPercentCur_);
        snprintf(detail, sizeof(detail), "avg %.1f%%", perfCpuPercentAvg_);
        DrawStatRow("CPU Usage", val, detail);

        PerfDivider();

        snprintf(val, sizeof(val), "%.1f MB", perfWorkingSetMB_);
        snprintf(detail, sizeof(detail), "peak %.1f MB", perfPeakWorkingSetMB_);
        DrawStatRow("RAM Usage", val, detail);

        snprintf(val, sizeof(val), "%.1f MB", perfPrivateBytesMB_);
        DrawStatRow("Committed Memory", val);

        snprintf(val, sizeof(val), "%llu", static_cast<unsigned long long>(perfPageFaultCount_));
        DrawStatRow("Memory Page Faults", val);

        PerfDivider();

        snprintf(val, sizeof(val), "%u", perfHandleCount_);
        DrawStatRow("Handles", val);

        snprintf(val, sizeof(val), "%u", perfGdiObjectCount_);
        DrawStatRow("GDI Objects", val);

        snprintf(val, sizeof(val), "%u", perfUserObjectCount_);
        DrawStatRow("User Objects", val);

        PerfDivider();

        int upH = static_cast<int>(perfUptimeSeconds_) / 3600;
        int upM = (static_cast<int>(perfUptimeSeconds_) % 3600) / 60;
        int upS = static_cast<int>(perfUptimeSeconds_) % 60;
        snprintf(val, sizeof(val), "%d:%02d:%02d", upH, upM, upS);
        DrawStatRow("Process Uptime", val);

        snprintf(val, sizeof(val), "%.2f ms", frameTimeMs);
        snprintf(detail, sizeof(detail), "avg %.2f ms  (%.0f FPS)", perfFrameTimeMsAvg_, io.Framerate);
        DrawStatRow("Frame Time", val, detail);

        PerfDivider();

        auto sources = mixer_.ListSources();
        uint64_t totalUnderrunFrames = 0;
        for (auto& s : sources) totalUnderrunFrames += mixer_.GetUnderrunFrames(s.id);
        double totalUnderrunMs = static_cast<double>(totalUnderrunFrames) / 48000.0 * 1000.0;
        snprintf(val, sizeof(val), "%zu", sources.size());
        DrawStatRow("Active Sources", val);
        snprintf(val, sizeof(val), "%.0f ms", totalUnderrunMs);
        DrawStatRow("Total Underrun Time", val);
    }
    ImGui::PopStyleVar();
    ImGui::End();
    ImGui::PopStyleVar();
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

    constexpr float kFooterTopGap = 8.0f;
    constexpr float kFooterTextHeight = 12.0f;
    constexpr float kFooterBottomGap = 8.0f;
    constexpr float kFooterTotalHeight = kFooterTopGap + kFooterTextHeight + kFooterBottomGap;

    float contentY = 0.0f;
    RenderToolbar(contentY);

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
    ImGui::PopStyleVar(1);

    if (layout_) {
        constexpr float kOuterMargin = 16.0f;
        TilingLayout::Render(*layout_,
                              kOuterMargin, contentY + kOuterMargin,
                              io.DisplaySize.x - kOuterMargin * 2.0f,
                              io.DisplaySize.y - contentY - kOuterMargin - kFooterTotalHeight,
                              [this](const std::string& paneId, const PaneRenderContext& ctx) {
                                  RenderPane(paneId, ctx);
                              },
                              [this](const std::string& paneId) {
                                  return PaneDisplayName(paneId);
                              });
    }

    ImGui::End();
    ImGui::PopStyleVar(2);

    footerOccluderRects_.clear();

    RenderToolsDownloadPrompt();
    RenderPerformanceWindow();

    if (showLicensesWindow_) {
        ImGui::OpenPopup("Licenses");
        showLicensesWindow_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Licenses", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        RegisterFooterOccluder();
        constexpr float kLicensesWidth = 480.0f;
        constexpr float kLicensesBodyHeight = 320.0f;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kLicensesWidth);
        ImGui::TextWrapped("WireMerge uses the following third-party software, libraries, and services:");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 6.0f));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
        ImGui::BeginChild("##licenses_body", ImVec2(kLicensesWidth, kLicensesBodyHeight), ImGuiChildFlags_Border);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kLicensesWidth - 20.0f);

        auto LicenseEntry = [&](const char* name, const char* license, const char* desc) {
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.9f, 1.0f), "%s", name);
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", license);
            ImGui::TextWrapped("%s", desc);
            ImGui::Dummy(ImVec2(0, 6.0f));
        };

        LicenseEntry("Dear ImGui", "MIT License",
                      "Immediate-mode GUI library used for the entire WireMerge interface.");
        LicenseEntry("PortAudio", "MIT-style License",
                      "Cross-platform audio I/O library used for capturing and playing back "
                      "audio from USB devices.");
        LicenseEntry("libusb", "GNU LGPL v2.1",
                      "USB device access library used for detecting and enumerating connected "
                      "USB audio hardware.");
        LicenseEntry("Android Debug Bridge (adb)", "Apache License 2.0",
                      "Command-line tool from the Android SDK Platform Tools, used to "
                      "communicate with connected Android devices for app-audio capture.");
        LicenseEntry("sndcpy", "MIT License",
                      "Android-side capture tool used to route app audio from a connected "
                      "phone into WireMerge.");
        LicenseEntry("vcpkg", "MIT License",
                      "C++ package manager used to acquire and statically link the "
                      "dependencies above during the build process.");

        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 6.0f));
        ImGui::TextWrapped("Full license texts for each of the above are available from their "
                            "respective project pages. WireMerge does not modify or redistribute "
                            "any of these projects' source code beyond standard linking.");

        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::Dummy(ImVec2(0, 8.0f));
        if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (showTestersWindow_) {
        ImGui::OpenPopup("Testers");
        showTestersWindow_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Testers", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        RegisterFooterOccluder();
        ImGui::TextUnformatted("Thanks to everyone who tested WireMerge:");
        ImGui::Dummy(ImVec2(0, 6.0f));
        static const char* kTesterNames[] = {
            "Brook - hailegna (tester) (Logo artist)",
            "urlate (tester)",
            "kldprm (tester)",
        };
        for (const char* name : kTesterNames) {
            ImGui::TextUnformatted(name);
        }
        ImGui::Dummy(ImVec2(0, 10.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 6.0f));
        ImGui::TextWrapped("This project is run by Zerrin Siya as the sole Maintainer and reviewer.");
        ImGui::Dummy(ImVec2(0, 8.0f));
        if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    bool anyFooterModalOpen = ImGui::IsPopupOpen("Licenses", ImGuiPopupFlags_None) ||
                               ImGui::IsPopupOpen("Testers", ImGuiPopupFlags_None) ||
                               ImGui::IsPopupOpen("Download Android Capture Tools?", ImGuiPopupFlags_None);

    {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImFont* font = ImGui::GetFont();
        float smallSize = kFooterTextHeight;
        ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text, 0.55f);
        ImU32 textColHot = ImGui::GetColorU32(ImGuiCol_Text);
        ImU32 textColInert = ImGui::GetColorU32(ImGuiCol_Text, 0.30f);

        float textY = io.DisplaySize.y - kFooterBottomGap - kFooterTextHeight;
        float footerTop = textY - kFooterTopGap;
        float footerBottom = io.DisplaySize.y;
        bool mouseInFooterRow = !anyFooterModalOpen &&
                                 io.MousePos.y >= footerTop && io.MousePos.y <= footerBottom;

        const char* testersText = "Testers";
        ImVec2 testersSize = font->CalcTextSizeA(smallSize, 100000.0f, 0.0f, testersText);
        ImVec2 testersPos(16.0f, textY);
        bool testersHover = mouseInFooterRow &&
                             io.MousePos.x >= testersPos.x && io.MousePos.x <= testersPos.x + testersSize.x;
        DrawFooterTextClipped(dl, font, smallSize, testersPos, testersSize,
                               anyFooterModalOpen ? textColInert : (testersHover ? textColHot : textCol),
                               testersText);
        if (testersHover) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            float underlineY = testersPos.y + testersSize.y + 1.0f;
            dl->AddLine(ImVec2(testersPos.x, underlineY), ImVec2(testersPos.x + testersSize.x, underlineY), textColHot);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) showTestersWindow_ = true;
        }

        const char* footerText = "\xC2\xA9 2026 Zerrin Siya. This software is released under the MIT License.";
        ImVec2 footerSize = font->CalcTextSizeA(smallSize, 100000.0f, 0.0f, footerText);
        ImVec2 footerPos(io.DisplaySize.x - footerSize.x - 16.0f, textY);
        bool footerHover = mouseInFooterRow &&
                            io.MousePos.x >= footerPos.x && io.MousePos.x <= footerPos.x + footerSize.x;
        DrawFooterTextClipped(dl, font, smallSize, footerPos, footerSize,
                               anyFooterModalOpen ? textColInert : (footerHover ? textColHot : textCol),
                               footerText);
        if (footerHover) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            float underlineY = footerPos.y + footerSize.y + 1.0f;
            dl->AddLine(ImVec2(footerPos.x, underlineY), ImVec2(footerPos.x + footerSize.x, underlineY), textColHot);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) showLicensesWindow_ = true;
        }
    }

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

}
