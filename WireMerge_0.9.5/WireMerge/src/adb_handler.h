#pragma once
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <optional>
#include <memory>
#include "mixer.h"

namespace wm {

struct AdbDeviceInfo {
    std::string serial;
    std::string state; //"device", "unauthorized", "offline"
};

class AdbHandler {
public:
    AdbHandler();
    ~AdbHandler();

    bool Initialize(bool autoDownloadIfMissing = true);

    void InitializeAsync(bool autoDownloadIfMissing = true);

    bool IsAvailable() const {
        return asyncInitDone_.load(std::memory_order_acquire) &&
               adbPath_.has_value() && apkPath_.has_value();
    }

    bool IsAsyncInitDone() const { return asyncInitDone_.load(std::memory_order_acquire); }

    void DownloadToolsAsync();
    bool IsDownloadInProgress() const { return downloadInProgress_.load(std::memory_order_acquire); }

    std::vector<AdbDeviceInfo> ListDevices() const;

    void RequestDeviceScan();

    bool TryTakeDeviceScanResult(std::vector<AdbDeviceInfo>& outDevices);

    void StartCaptureAsync(Mixer& mixer, const std::string& deviceSerial, int localPort = 28200);

    bool IsStarting(const std::string& deviceSerial) const;

    bool TryTakeStartResult(const std::string& deviceSerial, SourceId& outSourceId);

    void StopCapture(const std::string& deviceSerial);

    void Shutdown();
    void StopAll();

private:
    struct Session {
        std::string deviceSerial;
        int localPort = 0;
        SourceId sourceId = 0;
        std::thread readerThread;
        std::atomic<bool> running{false};
        uintptr_t socket = static_cast<uintptr_t>(~0); //INVALID_SOCKET, SOCKET is a UINT_PTR
    };

    int RunAdb(const std::vector<std::string>& args, std::string& output) const;

    void FireAndForgetAdb(const std::vector<std::string>& args) const;

    bool TryAutoDownload(const std::string& toolsDir);

    SourceId StartCaptureBlocking(Mixer& mixer, const std::string& deviceSerial, int localPort);

    void ReaderLoop(Session* session, Mixer* mixer);

    void DeviceScanLoop();

    struct PendingStart {
        std::string deviceSerial;
        std::thread thread;
        std::atomic<bool> done{false};
        SourceId result = 0;
    };

    std::optional<std::string> adbPath_;
    std::optional<std::string> apkPath_;
    std::thread asyncInitThread_;
    std::atomic<bool> asyncInitDone_{false};
    std::thread downloadThread_;
    std::atomic<bool> downloadInProgress_{false};
    std::thread deviceScanThread_;
    std::atomic<bool> deviceScanInProgress_{false};
    std::vector<AdbDeviceInfo> deviceScanResult_;
    std::atomic<bool> deviceScanResultReady_{false};
    mutable bool everInvokedAdb_ = false;
    std::vector<std::unique_ptr<Session>> sessions_;
    std::vector<std::unique_ptr<PendingStart>> pendingStarts_;
};

}
