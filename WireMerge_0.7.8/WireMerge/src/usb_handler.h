#pragma once
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <cstdint>

// ---------------------------------------------------------------------------
// usb_handler.h
//
// IMPORTANT DESIGN NOTE (read this before extending this file):
// This does NOT read raw audio data off the USB bus. Implementing the USB
// Audio Class (UAC 1.0/2.0) protocol -- isochronous transfer scheduling,
// feedback/sync endpoints, sample-rate clock recovery -- from scratch with
// libusb is a huge undertaking on its own and would fight with Windows'
// own USB audio class driver for ownership of the device.
//
// Instead: once a phone/TV is plugged in and Windows enumerates it as a
// USB Audio Class device, Windows' built-in driver already exposes it as a
// normal recording device, which PortAudio (see audio_handler.h) can open
// directly via WASAPI. That's the actual audio path.
//
// What libusb IS useful for here:
//   - Enumerating connected USB devices so the GUI can show "USB device
//     plugged in" with vendor/product info, even before the user has
//     picked which PortAudio input corresponds to it.
//   - Hotplug notifications (plug/unplug) so the GUI can refresh its
//     device list live instead of requiring a manual rescan.
// ---------------------------------------------------------------------------

namespace wm {

struct UsbDeviceInfo {
    uint16_t vendorId;
    uint16_t productId;
    std::string manufacturer; // may be empty if device didn't answer descriptor request
    std::string product;
    std::string serial;
    // Heuristic guess only -- libusb sees interface class codes, but the
    // authoritative "is this an audio input" answer comes from whether it
    // shows up in AudioHandler::ListInputDevices(). This flag just helps
    // the GUI pre-filter/highlight likely candidates.
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

    // Registers a callback invoked on device plug/unplug. Starts a
    // background thread that pumps libusb's event loop; safe to call
    // once after Initialize().
    void StartHotplugMonitor(UsbHotplugCallback callback);
    void StopHotplugMonitor();

    bool IsInitialized() const { return initialized_; }

    // Called by the libusb hotplug thunk (defined in usb_handler.cpp) on
    // the libusb event thread. Public so the free-function thunk required
    // by libusb's C callback signature can reach it; not intended to be
    // called from application code.
    void HandleHotplugEvent(UsbEvent event, const UsbDeviceInfo& info);

private:
    void EventLoop();

    bool initialized_ = false;
    void* usbContext_ = nullptr; // libusb_context*, opaque here to avoid forcing libusb.h on includers
    int hotplugHandle_ = -1;     // libusb_hotplug_callback_handle
    std::thread eventThread_;
    std::atomic<bool> running_{false};
    UsbHotplugCallback callback_;
};

} // namespace wm
