#pragma once
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <cstdint>

namespace wm {

struct UsbDeviceInfo {
    uint16_t vendorId;
    uint16_t productId;
    std::string manufacturer;
    std::string product;
    std::string serial;
    bool looksLikeAudioClass = false;
};

enum class UsbEvent { Connected, Disconnected };

using UsbHotplugCallback = std::function<void(UsbEvent, const UsbDeviceInfo&)>;

class UsbHandler {
public:
    UsbHandler();
    ~UsbHandler();

    bool Initialize();
    void Shutdown();

    std::vector<UsbDeviceInfo> Enumerate() const;

    void StartHotplugMonitor(UsbHotplugCallback callback);
    void StopHotplugMonitor();

    bool IsInitialized() const { return initialized_; }

    void HandleHotplugEvent(UsbEvent event, const UsbDeviceInfo& info);

private:
    void EventLoop();

    bool initialized_ = false;
    void* usbContext_ = nullptr; //libusb_context*
    int hotplugHandle_ = -1; //libusb_hotplug_callback_handle
    std::thread eventThread_;
    std::atomic<bool> running_{false};
    UsbHotplugCallback callback_;
};

}
