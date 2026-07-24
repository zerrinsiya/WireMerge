#include "gui.h"
#include "utils.h"
#include <algorithm>

#include <d3d11.h>
#include <windows.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace wm {

// Draws a small "skeletal" (outline, not filled) chevron over the
// right-hand side of the item most recently rendered (call immediately
// after BeginCombo). Used with ImGuiComboFlags_NoArrowButton to replace
// ImGui's default solid-triangle arrow (which some builds/themes render
// with its own colored background square) -- this draws directly over
// the combo's own existing frame background, so there's no separate
// "icon section" color at all, just two thin muted lines.
static void DrawSkeletalDropdownArrow() {
    ImVec2 itemMin = ImGui::GetItemRectMin();
    ImVec2 itemMax = ImGui::GetItemRectMax();
    float cy = (itemMin.y + itemMax.y) * 0.5f;
    float cx = itemMax.x - 18.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ImGuiComboFlags_NoArrowButton reserves NO space for an arrow at
    // all -- the whole widget is one uninterrupted clickable region, so
    // long preview text can extend right up to (and visually collide
    // with) wherever an arrow gets drawn on top of it. Painting a small
    // solid backdrop patch first (matching the frame's own background)
    // guarantees the arrow always reads cleanly regardless of preview
    // text length, rather than sometimes overlapping it.
    ImU32 backdropCol = ImGui::GetColorU32(ImGuiCol_FrameBg);
    dl->AddRectFilled(ImVec2(itemMax.x - 30.0f, itemMin.y + 1.0f),
                       ImVec2(itemMax.x - 1.0f, itemMax.y - 1.0f), backdropCol);

    float arrowHalfWidth = 4.0f;
    ImU32 arrowCol = ImGui::GetColorU32(ImGuiCol_TextDisabled); // muted gray, not the blue accent
    dl->AddLine(ImVec2(cx - arrowHalfWidth, cy - 2.0f), ImVec2(cx, cy + 2.0f), arrowCol, 1.5f);
    dl->AddLine(ImVec2(cx, cy + 2.0f), ImVec2(cx + arrowHalfWidth, cy - 2.0f), arrowCol, 1.5f);
}

// Skeletal (outline) chevron for collapse/expand indicators -- CollapsingHeader's
// built-in arrow is a filled triangle with no style override to make it
// skeletal, so subsection headers use this via a custom Selectable+state
// pattern instead of CollapsingHeader (see RenderSubsectionHeader below).
// Points right when collapsed, down when expanded -- same convention as
// CollapsingHeader's own arrow, so the interaction still reads familiarly.
static void DrawSkeletalChevron(ImVec2 center, bool expanded) {
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float s = 4.0f;
    if (expanded) {
        // pointing down: v shape
        dl->AddLine(ImVec2(center.x - s, center.y - s * 0.5f), ImVec2(center.x, center.y + s * 0.5f), col, 1.5f);
        dl->AddLine(ImVec2(center.x, center.y + s * 0.5f), ImVec2(center.x + s, center.y - s * 0.5f), col, 1.5f);
    } else {
        // pointing right: > shape
        dl->AddLine(ImVec2(center.x - s * 0.5f, center.y - s), ImVec2(center.x + s * 0.5f, center.y), col, 1.5f);
        dl->AddLine(ImVec2(center.x + s * 0.5f, center.y), ImVec2(center.x - s * 0.5f, center.y + s), col, 1.5f);
    }
}

// Shared by RenderSubsectionHeader (to size/draw the title) and by the
// collapsed child-height calculation -- one function so nothing can
// disagree about how big the title actually is.
static float SubsectionTitleFontSize() {
    return ImGui::GetFontSize() + 1.0f; // item 4: subsection titles bumped up 1px
}

// Shared by RenderSubsectionHeader (to size the row) and by the collapsed
// child-height calculation in RenderInputsContent/RenderDevicesContent --
// having one function means the two can never disagree with each other.
static float SubsectionHeaderRowHeight() {
    constexpr float kHeaderPadY = 3.0f; // item 1: halved from 6px, was reading as too much padding
    return SubsectionTitleFontSize() + kHeaderPadY * 2.0f;
}

// Renders a subsection header row: skeletal chevron + standard-size
// title, toggling `expanded` on click. Replaces CollapsingHeader
// specifically so the arrow can be skeletal/outline instead of ImGui's
// built-in filled triangle -- "skeletal arrows are universal for this
// project." Unlike top-level pane titles, subsection titles use the
// standard font size (the 15/13 scale-up was tried and explicitly
// rejected here).
//
// Rebuilt from scratch: previously used Selectable() for the hit-test
// and drew the chevron/text at independently-computed offsets from
// rowStart. On paper the math lined up, but in practice the
// interactive/highlighted area and the drawn icon read as offset from
// each other. Root-caused or not, the fix that removes the entire
// class of bug is to stop having two separate things that COULD drift:
// one InvisibleButton defines the row's hit-test rect, and the hover
// highlight, chevron, and text are all drawn directly from that same
// rect (rowMin/rowMax) -- there is no second position to go out of
// sync with.
static bool RenderSubsectionHeader(const char* label, bool& expanded) {
    constexpr float kChevronIndent = 8.0f;   // item 1: halved-ish from 14px
    constexpr float kTextIndent = 18.0f;     // item 1: halved-ish from 28px
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
    style.FramePadding = ImVec2(12, 8); // buttons need more inside padding, per feedback
    style.WindowPadding = ImVec2(16, 16); // spec: 16px padding around panel content
    style.IndentSpacing = 24.0f;

    // Spec: "ANY ROUNDING IS THE SAME 4PX" originally, later revised:
    // buttons specifically should be 2px, distinct from other frame
    // widgets (sliders, dropdowns, input fields) which stay at 4px.
    // FrameRounding is a single style value shared by ALL of these widget
    // types, so rather than locally override at every one of the ~9
    // button call sites, the default here is set to 2px (serving buttons
    // automatically) and the ~4 non-button sites (2 sliders, 2 combos)
    // locally push back to 4px instead -- fewer places to touch, same
    // visual result.
    style.WindowRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 2.5f; // slider thumb: minimum rounding, 2-3px
    style.TabRounding = 4.0f;

    // Visible thin borders everywhere -- this is the main thing that reads
    // as "plain/unstyled HTML form" rather than a modern flat-borderless
    // app look.
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    ImVec4* c = style.Colors;

    // Spec: dark charcoal background (#1c1f26 range), slightly lighter panels.
    c[ImGuiCol_WindowBg]  = ImVec4(0.085f, 0.095f, 0.117f, 1.00f); // slightly darker than before
    c[ImGuiCol_ChildBg]   = ImVec4(0.110f, 0.120f, 0.148f, 1.00f); // still lighter than window bg, proportionally darker too
    c[ImGuiCol_PopupBg]   = ImVec4(0.125f, 0.137f, 0.165f, 1.00f);

    // Borders: visible, plain mid-gray-blue, not high-contrast/glowy.
    c[ImGuiCol_Border]    = ImVec4(0.30f, 0.33f, 0.40f, 0.60f);

    // Blue accent family for interactive elements -- the "current blue"
    // continuity note. Flat fills, no gradient, distinct hover/active
    // steps rather than a smooth animated feel.
    ImVec4 blue        = ImVec4(0.20f, 0.45f, 0.85f, 1.00f);
    ImVec4 blueHover   = ImVec4(0.30f, 0.55f, 0.95f, 1.00f);
    ImVec4 blueActive  = ImVec4(0.15f, 0.35f, 0.70f, 1.00f);

    c[ImGuiCol_Button]         = blue;
    c[ImGuiCol_ButtonHovered]  = blueHover;
    c[ImGuiCol_ButtonActive]   = blueActive;
    // Reverted to the earlier darker/subtler translucent blue for
    // CollapsingHeader (used by the Regulated Inputs to PC / Devices
    // Android subsections) -- the more solid "blue strip" version was
    // specific to the since-reverted mockup spec attempt.
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
                                 WS_OVERLAPPEDWINDOW, 100, 100, 1280, 720,
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
    sourcesSplitNode_ = TilingLayout::FindPaneParent(*layout_, "sources"); // item 5

    // Seed the Log panel with curated boot-time messages (PortAudio/ADB
    // readiness, etc) pushed to UiLog before this Gui instance existed --
    // see utils.h's UiLog doc comment for why that indirection is needed.
    // This is what makes those messages actually show up in the UI's Log
    // panel, not just WireMerge.log on disk.
    for (auto& line : UiLog::Instance().DrainAll()) {
        PushLogLine(line);
    }

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
    // Reduced from the earlier 95px -- header should establish hierarchy
    // but not eat excessive vertical space from the rest of the app.
    float toolbarHeight = 68.0f; // slightly taller to accommodate more top padding

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, toolbarHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f)); // more top padding, per feedback
    ImGui::Begin("##toolbar", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Title: larger than section labels, white. SetWindowFontScale scales
    // the existing font rather than loading a new one -- keeps fonts
    // unchanged while still getting a distinct title size.
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted(kWireMergeVersion);
    ImGui::SetWindowFontScale(1.0f);

    // Title and subtitle closed up tight (no extra gap) rather than the
    // previous 8px spacer -- reads as one cohesive title block.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.65f, 0.70f, 1.0f));
    ImGui::TextUnformatted("Lightweight USB & Android audio router");
    ImGui::PopStyleColor();

    // Top-right: red "Exit WireMerge" button. Symmetric padding, with
    // EXTRA padding beyond the global button default so it reads as
    // deliberately bigger/more prominent than ordinary buttons -- still
    // computed cleanly (text + 2*padding on each axis), not hand-tuned
    // asymmetric fudge constants.
    const char* exitLabel = "Exit WireMerge";
    ImVec2 exitSize = ImGui::CalcTextSize(exitLabel);
    ImVec2 exitPad = ImVec2(ImGui::GetStyle().FramePadding.x + 8.0f,
                             ImGui::GetStyle().FramePadding.y + 6.0f);
    float exitWidth = exitSize.x + exitPad.x * 2.0f;
    float exitHeight = exitSize.y + exitPad.y * 2.0f;

    ImGui::SetCursorPos(ImVec2(io.DisplaySize.x - exitWidth - 24.0f,
                                (toolbarHeight - exitHeight) * 0.5f + 4.0f)); // shifted slightly down, per feedback

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.16f, 0.16f, 1.0f)); // slightly darker red
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.21f, 0.21f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.50f, 0.11f, 0.11f, 1.0f));
    bool exitClicked = ImGui::Button(exitLabel, ImVec2(exitWidth, exitHeight));
    ImGui::PopStyleColor(3);
    if (exitClicked) {
        RequestExit();
    }

    // Subtle bottom border separating the header from content, drawn
    // manually rather than via the window's own border (which would put
    // a line on all four sides instead of just the bottom).
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

    // SetNextItemWidth(-1) previously gave the slider the FULL available
    // width, leaving zero room for its inline trailing label ("Master
    // Volume" drawn immediately after the widget on the same line) -- cut
    // down to just the "M". Reserve real space for the label based on its
    // actual measured text width instead of guessing a fixed number.
    // Item 3 fix: reserving EXACTLY the label's width left it flush against
    // the pane's right border with no breathing room -- add a fixed extra
    // margin on top of the measured width.
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

    ImGui::Dummy(ImVec2(0, 6.0f)); // item 3: halved from 12px, per feedback

    auto outputs = audio_.ListOutputDevices();
    std::string preview = selectedOutputDevice_ >= 0 ? "Selected" : "Choose output...";
    for (auto& d : outputs) {
        if (d.index == selectedOutputDevice_) preview = d.name;
    }

    // Item 2.3/2.4 fix (kept from prior round): the combo's inline label
    // and full-width stretch fought each other, clipping a letter off
    // "Output"/"Input". Fix: hidden inline label ("##Output") + a
    // separate Text() line above -- robust at any pane width.
    ImGui::TextUnformatted("Output");
    ImGui::SetNextItemWidth(-1);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(11.0f, 11.5f)); // thicker, matches Input combo
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f); // dropdowns keep 4px, only buttons dropped to 2px
    bool comboOpened = ImGui::BeginCombo("##Output", preview.c_str(), ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleVar(2);
    DrawSkeletalDropdownArrow();
    if (comboOpened) {
        for (auto& d : outputs) {
            bool selected = (d.index == selectedOutputDevice_);
            std::string label = d.name + (d.isDefaultOutput ? " (default)" : "");
            if (ImGui::Selectable(label.c_str(), selected)) {
                selectedOutputDevice_ = d.index;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Dummy(ImVec2(0, 8.0f)); // item 3: cut 50% from prior 16px, per feedback

    // Buttons on one row, 12px gap, NOT stretched to fill width.
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

    ImGui::SameLine(); // ambient spacing, matches original older-version button spacing
    ImGui::BeginDisabled(!outputOpen_);
    if (ImGui::Button("Stop Output")) {
        audio_.CloseOutput();
        outputOpen_ = false;
        PushLogLine("Output stopped.");
    }
    ImGui::EndDisabled();
}

void Gui::RenderInputsContent(const PaneRenderContext& /*ctx*/) {
    // Both subsections below get their own bordered, slightly-darker
    // child "window" -- distinct from the parent Inputs pane's own
    // background, matching how this looked in an earlier version.
    ImVec4 subsectionBg(0.095f, 0.105f, 0.130f, 1.0f);
    static bool pcExpanded = true;
    // Rebuild: no more guessed fixed pixel heights. We measure the actual
    // content height used at the END of each frame (via GetCursorPosY(),
    // which already accounts for every item's real height AND ImGui's
    // own automatic ItemSpacing between them) and feed it forward as the
    // target for next frame's BeginChild call. One frame of lag on first
    // toggle/font-change, imperceptible, and it can never drift out of
    // sync with the real content again -- fixes both the "dead space at
    // the bottom" and "content overflowing into a scrollbar" complaints
    // at the root, instead of re-guessing a new magic number each round.
    static float pcContentHeight = 150.0f; // corrected automatically after frame 1

    float padY = ImGui::GetStyle().WindowPadding.y;
    float collapsedHeight = SubsectionHeaderRowHeight() + padY * 2.0f;
    float expandedHeight = pcContentHeight + padY; // pcContentHeight already includes the top padding baked into GetCursorPosY()

    ImGui::PushStyleColor(ImGuiCol_ChildBg, subsectionBg);
    ImGui::BeginChild("##pc_subsection", ImVec2(-1, pcExpanded ? expandedHeight : collapsedHeight), ImGuiChildFlags_Border);

    // Header rendered INSIDE the bordered child (not before/above it) --
    // this is what "connects" the title to its content instead of it
    // floating disconnected above a separately-bordered box.
    RenderSubsectionHeader("Regulated Inputs to PC", pcExpanded);

    if (pcExpanded) {
        ImGui::Dummy(ImVec2(0, 4.0f)); // item 2: halved from 8px, per feedback
        static int selectedInput = -1;
        auto inputs = audio_.ListInputDevices();
        std::string inPreview = "Choose input...";
        for (auto& d : inputs) if (d.index == selectedInput) inPreview = d.name;

        ImGui::TextUnformatted("Input");
        ImGui::SetNextItemWidth(-1);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(11.0f, 11.5f)); // thicker dropdown, per feedback
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

        ImGui::Dummy(ImVec2(0, 4.2f)); // item 7: cut 30% from prior 6px, per feedback

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

        ImGui::SameLine();
        if (ImGui::Button("Rescan Devices")) {
            if (audio_.RescanDevices()) {
                PushLogLine("Regulated input devices rescanned.");
            } else {
                PushLogLine("Rescan needs Output and all Sources stopped first "
                            "(rescanning reinitializes PortAudio).");
            }
        }

        pcContentHeight = ImGui::GetCursorPosY(); // measured for next frame's expandedHeight
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 4.0f)); // item 3: halved from 8px, per feedback

    RenderDevicesContent({});
}

void Gui::RenderDevicesContent(const PaneRenderContext& /*ctx*/) {
    ImVec4 subsectionBg(0.095f, 0.105f, 0.130f, 1.0f);
    static bool androidExpanded = true;
    // Rebuild: same measured-height approach as Reg Inputs above -- no
    // more guessed fixed pixel heights that need re-tuning every round.
    static float androidContentHeight = 190.0f; // corrected automatically after frame 1

    float padY = ImGui::GetStyle().WindowPadding.y;
    float collapsedHeight = SubsectionHeaderRowHeight() + padY * 2.0f;
    float expandedHeight = androidContentHeight + padY;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, subsectionBg);
    ImGui::BeginChild("##android_subsection", ImVec2(-1, androidExpanded ? expandedHeight : collapsedHeight), ImGuiChildFlags_Border);

    RenderSubsectionHeader("Devices (Android)", androidExpanded);

    if (androidExpanded) {
        ImGui::Dummy(ImVec2(0, 4.0f)); // item 2: halved from 8px, per feedback
        if (!adb_.IsAvailable()) {
            ImGui::TextWrapped("Not set up: adb.exe / sndcpy.apk missing from "
                                "'tools' (see Log / README).");
            androidContentHeight = ImGui::GetCursorPosY(); // measure this shorter state too
            ImGui::EndChild();
            ImGui::PopStyleColor();
            return;
        }

        ImGui::TextWrapped("Captures phone app audio (e.g. Spotify) over USB.\n"
                            "Requires a one-time on-device permission per capture.");

        ImGui::Dummy(ImVec2(0, 2.8f)); // item 7: tightened gap around Rescan Now

        // Rescan Now stays (explicitly requested to keep it) -- it drives
        // the same cached device list used for both display and capture
        // target selection below.
        constexpr double kRescanIntervalMs = 2000.0;
        static std::vector<AdbDeviceInfo> cachedDevices;
        static double lastScanTime = -kRescanIntervalMs;

        double now = ImGui::GetTime() * 1000.0;
        if (now - lastScanTime >= kRescanIntervalMs) {
            cachedDevices = adb_.ListDevices();
            lastScanTime = now;
        }

        // Item 7: Rescan Now should read as visually secondary/smaller
        // than the main Start/Stop Android Capture buttons -- 30% smaller
        // via a scaled FramePadding (same technique already used
        // elsewhere in this file for locally overriding button/frame
        // size at a single call site).
        {
            ImVec2 mainPad = ImGui::GetStyle().FramePadding;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(mainPad.x * 0.7f, mainPad.y * 0.7f));
            if (ImGui::Button("Rescan Now")) {
                cachedDevices = adb_.ListDevices();
                lastScanTime = now;
                PushLogLine("Android devices rescanned.");
            }
            ImGui::PopStyleVar();
        }

        ImGui::Dummy(ImVec2(0, 2.8f)); // item 7: tightened gap around Rescan Now

        // Shows "No devices detected." when empty; the actual selection
        // list renders here instead when devices ARE present. Since this
        // list can genuinely grow with more devices connected, the
        // measured-height approach below means the box will correctly
        // grow/shrink with it instead of needing yet another manual
        // re-tune.
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
                                " -- check log file for details.");
                }
            }
        }

        bool starting = !selectedSerial.empty() && adb_.IsStarting(selectedSerial);
        bool canStart = !selectedSerial.empty() && !starting;

        ImGui::Dummy(ImVec2(0, 2.8f)); // item 7: tightened gap between entities

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

        androidContentHeight = ImGui::GetCursorPosY(); // measured for next frame's expandedHeight
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void Gui::RenderSourcesContent(const PaneRenderContext& /*ctx*/) {
    auto sources = mixer_.ListSources();
    if (sources.empty()) {
        // Uses the live content region (now that the pane's own padding
        // bug is fixed -- see RenderFrame's WindowPadding pop) rather
        // than the stale ctx rect, which was computed before padding was
        // applied and no longer matches the pane's real usable area.
        // No box drawn here anymore -- the pane itself already has its
        // own border (DrawSubtlePanelFrame, see layout.cpp), so a second
        // hand-drawn border around this empty-state text was a literal
        // box-inside-a-box. Just center the muted text in the pane's own
        // (correctly-padded) content area.
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
        ImGui::Dummy(ImVec2(0, 8.0f));
        ImGui::PopID();
    }

    // Item 5: rather than guess a fixed split ratio that's only correct
    // for one specific window size / source count, measure it directly.
    // We're still inside this pane's own scrollable child window here
    // (TilingLayout::RenderNode wraps every leaf's content in one), so
    // GetScrollMaxY() > 0 means content is currently overflowing and a
    // scrollbar is showing. Grow the Sources/Inputs split ratio by a
    // small step -- ~6px worth per frame -- until it stops, then latch
    // sourcesRatioSettled_ so this never runs again and never fights a
    // manual splitter drag later in the session. This converges within
    // a handful of frames (well under a tenth of a second), and lands
    // "just enough" rather than a large guessed jump because it stops
    // the instant the scrollbar is gone.
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
    // Rounded border around the log CONTENT specifically (standard app
    // rounding, via ChildRounding) -- distinct from the outer pane's own
    // border drawn by DrawSubtlePanelFrame, which only wraps the title.
    ImGui::BeginChild("##log_content", ImVec2(-1, -1), ImGuiChildFlags_Border);

    // Item 4 fix: the old code called SetScrollHereY(1.0f) unconditionally
    // every frame, which re-pins the view to the bottom on EVERY frame --
    // including the ones where the user is actively dragging the scrollbar
    // or turning the mouse wheel upward. That's what made the log feel
    // "locked", forcibly yanking back down mid-scroll. Fix: only snap to
    // bottom (a) right after new lines arrive, and (b) only if the user
    // was already at/near the bottom when they arrived -- i.e. auto-follow
    // like a normal terminal/chat log, not a hard pin. If the user has
    // scrolled up to read history, new lines no longer drag them away.
    size_t lineCount = logLines_.size();
    bool linesAdded = lineCount != lastSeenLogLineCount_;

    for (auto& line : logLines_) {
        ImGui::TextWrapped("%s", line.c_str());
    }

    if (linesAdded) {
        // wasAtBottomLastFrame_ was captured at the END of the previous
        // frame (below), i.e. before this frame's new lines were appended
        // -- so it reflects where the user actually left the view.
        if (wasAtBottomLastFrame_) {
            ImGui::SetScrollHereY(1.0f);
        }
        lastSeenLogLineCount_ = lineCount;
    }

    // Recompute "at bottom" AFTER this frame's content + any scroll call
    // above, using a small epsilon since ScrollMaxY comparisons can be off
    // by a fraction of a pixel.
    wasAtBottomLastFrame_ = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

    ImGui::EndChild();
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
    // Pop WindowPadding right away -- it was only ever meant for this one
    // invisible wrapper window. Leaving it pushed through the
    // TilingLayout::Render() call below meant every leaf pane's
    // BeginChild() (which reads WindowPadding from whatever's currently
    // on the style stack, not a fixed per-window value) inherited zero
    // padding too. That's what caused content sticking flush to every
    // pane's borders, and made the Active Sources empty-state box look
    // like "a box inside a box" (it was drawn flush against its own
    // zero-padding child border instead of having breathing room).
    ImGui::PopStyleVar(1);

    if (layout_) {
        constexpr float kOuterMargin = 16.0f; // spec: 16px outer margin on all sides
        TilingLayout::Render(*layout_,
                              kOuterMargin, contentY + kOuterMargin,
                              io.DisplaySize.x - kOuterMargin * 2.0f,
                              io.DisplaySize.y - contentY - kOuterMargin * 2.0f,
                              [this](const std::string& paneId, const PaneRenderContext& ctx) {
                                  RenderPane(paneId, ctx);
                              },
                              [this](const std::string& paneId) {
                                  return PaneDisplayName(paneId);
                              });
    }

    ImGui::End();
    ImGui::PopStyleVar(2); // WindowRounding, WindowBorderSize -- WindowPadding was already popped above

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
