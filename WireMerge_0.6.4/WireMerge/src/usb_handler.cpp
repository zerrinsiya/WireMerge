#include "usb_handler.h"
#include "utils.h"
#include <libusb.h>
#include <cstring>

namespace wm {

// USB Audio Class = interface class 0x01. We check top-level device class
// (rare, most composite devices report 0x00 at device level and declare
// class per-interface) and fall back to a per-interface scan.
static bool DeviceLooksLikeAudioClass(libusb_device* dev) {
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(dev, &desc) != 0) return false;

    if (desc.bDeviceClass == LIBUSB_CLASS_AUDIO) return true;

    libusb_config_descriptor* config = nullptr;
    if (libusb_get_active_config_descriptor(dev, &config) != 0 || !config) return false;

    bool found = false;
    for (int i = 0; i < config->bNumInterfaces && !found; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting && !found; ++a) {
            if (iface.altsetting[a].bInterfaceClass == LIBUSB_CLASS_AUDIO) {
                found = true;
            }
        }
    }
    libusb_free_config_descriptor(config);
    return found;
}

static UsbDeviceInfo DescribeDevice(libusb_device* dev, libusb_device_handle* handleOpt) {
    UsbDeviceInfo info{};
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(dev, &desc) != 0) return info;

    info.vendorId = desc.idVendor;
    info.productId = desc.idProduct;
    info.looksLikeAudioClass = DeviceLooksLikeAudioClass(dev);

    // String descriptors require an open handle; opening every device just
    // to read its name can fail (permissions/driver claims it), so we treat
    // failures as "leave name blank" rather than an error.
    bool openedHere = false;
    libusb_device_handle* handle = handleOpt;
    if (!handle) {
        if (libusb_open(dev, &handle) == 0) openedHere = true;
        else handle = nullptr;
    }

    if (handle) {
        unsigned char buf[256];
        if (desc.iManufacturer &&
            libusb_get_string_descriptor_ascii(handle, desc.iManufacturer, buf, sizeof(buf)) > 0) {
            info.manufacturer = reinterpret_cast<char*>(buf);
        }
        if (desc.iProduct &&
            libusb_get_string_descriptor_ascii(handle, desc.iProduct, buf, sizeof(buf)) > 0) {
            info.product = reinterpret_cast<char*>(buf);
        }
        if (desc.iSerialNumber &&
            libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, buf, sizeof(buf)) > 0) {
            info.serial = reinterpret_cast<char*>(buf);
        }
        if (openedHere) libusb_close(handle);
    }

    return info;
}

UsbHandler::UsbHandler() = default;

UsbHandler::~UsbHandler() {
    Shutdown();
}

bool UsbHandler::Initialize() {
    libusb_context* ctx = nullptr;
    int rc = libusb_init(&ctx);
    if (rc != 0) {
        WM_LOG_ERROR(std::string("libusb_init failed: ") + libusb_error_name(rc));
        return false;
    }
    usbContext_ = ctx;
    initialized_ = true;
    WM_LOG_INFO("libusb initialized.");
    return true;
}

void UsbHandler::Shutdown() {
    StopHotplugMonitor();
    if (usbContext_) {
        libusb_exit(static_cast<libusb_context*>(usbContext_));
        usbContext_ = nullptr;
    }
    initialized_ = false;
}

std::vector<UsbDeviceInfo> UsbHandler::Enumerate() const {
    std::vector<UsbDeviceInfo> result;
    if (!initialized_) return result;

    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(static_cast<libusb_context*>(usbContext_), &list);
    if (count < 0) {
        WM_LOG_ERROR("libusb_get_device_list failed");
        return result;
    }

    for (ssize_t i = 0; i < count; ++i) {
        result.push_back(DescribeDevice(list[i], nullptr));
    }

    libusb_free_device_list(list, 1);
    return result;
}

static int LIBUSB_CALL HotplugCallbackThunk(libusb_context* /*ctx*/, libusb_device* device,
                                             libusb_hotplug_event event, void* userData) {
    auto* self = static_cast<UsbHandler*>(userData);
    if (!self) return 0;

    UsbDeviceInfo info = DescribeDevice(device, nullptr);
    UsbEvent wmEvent = (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
                            ? UsbEvent::Connected
                            : UsbEvent::Disconnected;

    // Note: this thunk runs on the libusb event thread, not the GUI thread.
    // gui.cpp's callback implementation is responsible for marshaling this
    // onto the UI thread (e.g. pushing to a thread-safe queue drained once
    // per frame) rather than touching ImGui state directly from here.
    self->HandleHotplugEvent(wmEvent, info);
    return 0; // 0 = keep listening for further events on this registration
}

void UsbHandler::HandleHotplugEvent(UsbEvent event, const UsbDeviceInfo& info) {
    if (callback_) callback_(event, info);
}

void UsbHandler::StartHotplugMonitor(UsbHotplugCallback callback) {
    if (!initialized_ || running_) return;

    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        WM_LOG_WARN("libusb hotplug not supported on this platform/build; "
                     "falling back to manual 'Rescan' in the GUI.");
        return;
    }

    callback_ = std::move(callback);
    running_ = true;

    libusb_hotplug_callback_handle handle;
    int rc = libusb_hotplug_register_callback(
        static_cast<libusb_context*>(usbContext_),
        static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                           LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
        LIBUSB_HOTPLUG_ENUMERATE,
        LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
        HotplugCallbackThunk, this, &handle);

    if (rc != LIBUSB_SUCCESS) {
        WM_LOG_ERROR(std::string("libusb_hotplug_register_callback failed: ") +
                      libusb_error_name(rc));
        running_ = false;
        return;
    }
    hotplugHandle_ = static_cast<int>(handle);

    eventThread_ = std::thread(&UsbHandler::EventLoop, this);
    WM_LOG_INFO("USB hotplug monitor started.");
}

void UsbHandler::StopHotplugMonitor() {
    if (!running_) return;
    running_ = false;
    if (eventThread_.joinable()) eventThread_.join();
    if (hotplugHandle_ != -1 && usbContext_) {
        libusb_hotplug_deregister_callback(static_cast<libusb_context*>(usbContext_),
                                            static_cast<libusb_hotplug_callback_handle>(hotplugHandle_));
        hotplugHandle_ = -1;
    }
}

void UsbHandler::EventLoop() {
    struct timeval tv{0, 100000}; // 100ms poll, keeps shutdown responsive
    while (running_) {
        libusb_handle_events_timeout_completed(static_cast<libusb_context*>(usbContext_), &tv, nullptr);
    }
}

} // namespace wm
