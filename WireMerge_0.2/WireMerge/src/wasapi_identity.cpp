#include "wasapi_identity.h"
#include "utils.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#include <algorithm>
#include <cctype>
#include <cstdio>

#pragma comment(lib, "ole32.lib")

namespace wm {

// RAII helpers so every COM interface pointer this file touches gets
// released exactly once, even on early-return error paths -- this file
// does a fair amount of manual COM interface juggling, and that's exactly
// the kind of code where a missed Release() on an error path silently
// leaks until someone notices handle/memory growth much later.
template <typename T>
struct ComPtr {
    T* ptr = nullptr;
    ~ComPtr() { if (ptr) ptr->Release(); }
    T** AddressOf() { return &ptr; }
    T* operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

static std::string WideToUtf8(const wchar_t* wide) {
    if (!wide) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len - 1, '\0'); // len includes the null terminator
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), len, nullptr, nullptr);
    return result;
}

static std::vector<WasapiEndpointIdentity> EnumerateDirection(
    IMMDeviceEnumerator* enumerator, EDataFlow flow, bool isCapture) {
    std::vector<WasapiEndpointIdentity> results;

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.AddressOf()))) {
        return results;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.AddressOf()))) continue;

        LPWSTR rawId = nullptr;
        if (FAILED(device->GetId(&rawId)) || !rawId) continue;

        WasapiEndpointIdentity identity;
        identity.endpointId = rawId;
        identity.isCaptureDevice = isCapture;
        CoTaskMemFree(rawId);

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.AddressOf()))) {
            PROPVARIANT nameVar;
            PropVariantInit(&nameVar);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &nameVar)) &&
                nameVar.vt == VT_LPWSTR) {
                identity.friendlyName = WideToUtf8(nameVar.pwszVal);
            }
            PropVariantClear(&nameVar);
        }

        results.push_back(std::move(identity));
    }

    return results;
}

std::vector<WasapiEndpointIdentity> WasapiIdentity::EnumerateEndpoints() {
    std::vector<WasapiEndpointIdentity> all;

    // COINIT_MULTITHREADED: this can be called from the GUI/render thread
    // (see gui.cpp's caching wrapper), which is not the same thread model
    // assumption as a typical STA UI COM use case -- MTA avoids needing to
    // coordinate apartment types with the rest of the (non-COM) UI code.
    HRESULT coInitResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needsUninit = SUCCEEDED(coInitResult);
    // RPC_E_CHANGED_MODE means COM was already initialized on this thread
    // with a different apartment model by something else (e.g. a prior
    // call) -- proceed without calling CoUninitialize ourselves in that
    // case, since we didn't own that initialization.
    if (coInitResult == RPC_E_CHANGED_MODE) needsUninit = false;

    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator),
                                       reinterpret_cast<void**>(enumerator.AddressOf()));
        if (SUCCEEDED(hr) && enumerator) {
            auto captureDevices = EnumerateDirection(enumerator.ptr, eCapture, true);
            auto renderDevices = EnumerateDirection(enumerator.ptr, eRender, false);
            all.insert(all.end(), captureDevices.begin(), captureDevices.end());
            all.insert(all.end(), renderDevices.begin(), renderDevices.end());
        } else {
            WM_LOG_WARN("WasapiIdentity: failed to create IMMDeviceEnumerator (hr=0x" +
                         std::to_string(static_cast<unsigned long>(hr)) +
                         "); falling back to name-only device identification.");
        }
    }

    if (needsUninit) CoUninitialize();
    return all;
}

std::optional<WasapiEndpointIdentity> WasapiIdentity::FindByName(
    const std::vector<WasapiEndpointIdentity>& endpoints,
    const std::string& portAudioDeviceName,
    bool isCaptureDevice) {
    // Case-insensitive exact match first (PortAudio's WASAPI backend uses
    // the same friendly-name string, so this should be the common case).
    auto normalize = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    std::string targetName = normalize(portAudioDeviceName);

    for (const auto& ep : endpoints) {
        if (ep.isCaptureDevice != isCaptureDevice) continue;
        if (normalize(ep.friendlyName) == targetName) return ep;
    }

    // Fallback: substring match, in case PortAudio appended/trimmed
    // anything (some host API layers add suffixes like channel counts).
    for (const auto& ep : endpoints) {
        if (ep.isCaptureDevice != isCaptureDevice) continue;
        std::string epName = normalize(ep.friendlyName);
        if (!epName.empty() &&
            (epName.find(targetName) != std::string::npos ||
             targetName.find(epName) != std::string::npos)) {
            return ep;
        }
    }

    return std::nullopt;
}

std::string WasapiIdentity::ShortFingerprint(const std::wstring& endpointId) {
    // Endpoint IDs look like:
    //   {0.0.1.00000000}.{6f1b1a30-2c3f-4b8a-9e1a-...}
    // or for USB devices, embed the USB instance path segment. Rather than
    // parse the exact format (which varies by driver/bus type and isn't
    // documented as stable), take a short hash-like fingerprint from the
    // tail of the string -- enough to visibly distinguish two otherwise
    // identically-named devices without displaying the full raw ID.
    std::string utf8 = WideToUtf8(endpointId.c_str());
    if (utf8.size() <= 8) return utf8;

    // Simple, fast, non-cryptographic fingerprint -- this only needs to be
    // visually distinguishing for a handful of devices in a dropdown, not
    // collision-resistant against adversarial input.
    uint32_t hash = 2166136261u; // FNV-1a offset basis
    for (char c : utf8) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }

    char buf[9];
    snprintf(buf, sizeof(buf), "%08X", hash);
    return std::string(buf);
}

} // namespace wm
