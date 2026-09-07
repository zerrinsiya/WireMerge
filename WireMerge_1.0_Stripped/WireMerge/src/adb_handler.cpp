#include "adb_handler.h"
#include "utils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <avrt.h>
#include <sstream>
#include <fstream>
#include <iterator>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "avrt.lib")

namespace wm {

//sndcpy wire format: raw 16-bit signed PCM, 48kHz, stereo.
static constexpr int kSndcpySampleRate = 48000;
static constexpr int kSndcpyChannels = 2;

static std::string FindNextToExe(const std::string& filename) {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir = dir.substr(0, slash + 1);
    std::string candidate = dir + "tools\\" + filename;

    DWORD attrs = GetFileAttributesA(candidate.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return candidate;
    }
    return "";
}

AdbHandler::AdbHandler() = default;

AdbHandler::~AdbHandler() {
    StopAll();
}

bool AdbHandler::Initialize() {
    std::string adb = FindNextToExe("adb.exe");
    std::string apk = FindNextToExe("sndcpy.apk");

    if (adb.empty()) {
        WM_LOG_WARN("AdbHandler: tools/adb.exe not found. Android app-audio capture "
                     "will be unavailable. Get platform-tools from "
                     "https://developer.android.com/tools/releases/platform-tools "
                     "and place adb.exe (+ its DLLs) in a 'tools' folder next to WireMerge.exe.");
    } else {
        adbPath_ = adb;
    }

    if (apk.empty()) {
        WM_LOG_WARN("AdbHandler: tools/sndcpy.apk not found. Android app-audio capture "
                     "will be unavailable. Get it from https://github.com/rom1v/sndcpy "
                     "(download the release zip, extract sndcpy.apk) and place it in a "
                     "'tools' folder next to WireMerge.exe.");
    } else {
        apkPath_ = apk;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        WM_LOG_ERROR("AdbHandler: WSAStartup failed.");
        asyncInitDone_.store(true, std::memory_order_release);
        return false;
    }

    bool ok = adbPath_.has_value() && apkPath_.has_value();
    asyncInitDone_.store(true, std::memory_order_release);
    return ok;
}

void AdbHandler::InitializeAsync() {
    if (asyncInitThread_.joinable()) asyncInitThread_.join();
    asyncInitThread_ = std::thread([this]() {
        Initialize();
    });
}

int AdbHandler::RunAdb(const std::vector<std::string>& args, std::string& output) const {
    if (!adbPath_) return -1;
    everInvokedAdb_ = true;

    std::ostringstream cmd;
    cmd << "\"" << *adbPath_ << "\"";
    for (auto& a : args) cmd << " " << a;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe, writePipe;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return -1;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    std::string cmdStr = cmd.str();
    std::vector<char> cmdBuf(cmdStr.begin(), cmdStr.end());
    cmdBuf.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);

    if (!ok) {
        CloseHandle(readPipe);
        WM_LOG_ERROR("AdbHandler: failed to launch adb.exe (" + cmdStr + ")");
        return -1;
    }

    char buf[4096];
    DWORD bytesRead;
    while (ReadFile(readPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        output += buf;
    }
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, 15000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return static_cast<int>(exitCode);
}

void AdbHandler::FireAndForgetAdb(const std::vector<std::string>& args) const {
    if (!adbPath_) return;
    everInvokedAdb_ = true;

    std::ostringstream cmd;
    cmd << "\"" << *adbPath_ << "\"";
    for (auto& a : args) cmd << " " << a;

    STARTUPINFOA si{};
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi{};
    std::string cmdStr = cmd.str();
    std::vector<char> cmdBuf(cmdStr.begin(), cmdStr.end());
    cmdBuf.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) {
        WM_LOG_WARN("AdbHandler: fire-and-forget launch failed for adb " +
                     (args.empty() ? "" : args[0]));
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

std::vector<AdbDeviceInfo> AdbHandler::ListDevices() const {
    std::vector<AdbDeviceInfo> devices;
    if (!adbPath_) return devices;

    std::string output;
    RunAdb({"devices"}, output);

    std::istringstream stream(output);
    std::string line;
    std::getline(stream, line);
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        AdbDeviceInfo info;
        info.serial = line.substr(0, tab);
        info.state = line.substr(tab + 1);
        if (!info.state.empty() && info.state.back() == '\r') info.state.pop_back();
        devices.push_back(info);
    }
    return devices;
}

void AdbHandler::RequestDeviceScan() {
    bool expected = false;
    if (!deviceScanInProgress_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (deviceScanThread_.joinable()) deviceScanThread_.join();
    deviceScanThread_ = std::thread(&AdbHandler::DeviceScanLoop, this);
}

void AdbHandler::DeviceScanLoop() {
    auto result = ListDevices();
    deviceScanResult_ = std::move(result);
    deviceScanResultReady_.store(true, std::memory_order_release);
    deviceScanInProgress_.store(false, std::memory_order_release);
}

bool AdbHandler::TryTakeDeviceScanResult(std::vector<AdbDeviceInfo>& outDevices) {
    if (!deviceScanResultReady_.load(std::memory_order_acquire)) return false;
    outDevices = std::move(deviceScanResult_);
    deviceScanResultReady_.store(false, std::memory_order_release);
    return true;
}

SourceId AdbHandler::StartCaptureBlocking(Mixer& mixer, const std::string& deviceSerial, int localPort) {
    if (!IsAvailable()) {
        WM_LOG_ERROR("AdbHandler::StartCapture called but adb/sndcpy.apk not available.");
        return 0;
    }

    std::string out;
    if (RunAdb({"-s", deviceSerial, "install", "-r", "-g", "\"" + *apkPath_ + "\""}, out) != 0) {
        WM_LOG_ERROR("AdbHandler: sndcpy.apk install failed: " + out);
        return 0;
    }

    RunAdb({"-s", deviceSerial, "shell", "appops", "set", "com.rom1v.sndcpy",
            "PROJECT_MEDIA", "allow"}, out);

    if (RunAdb({"-s", deviceSerial, "forward", "tcp:" + std::to_string(localPort),
                "localabstract:sndcpy"}, out) != 0) {
        WM_LOG_ERROR("AdbHandler: adb forward failed: " + out);
        return 0;
    }

    if (RunAdb({"-s", deviceSerial, "shell", "am", "start",
                "com.rom1v.sndcpy/.MainActivity"}, out) != 0) {
        WM_LOG_ERROR("AdbHandler: launching sndcpy on device failed: " + out);
        return 0;
    }

    Sleep(2000);

    SourceId sourceId = mixer.AddSource("Android (" + deviceSerial + ")",
                                         kSndcpySampleRate, kSndcpyChannels,
                                         /*bufferMs=*/500);

    auto session = std::make_unique<Session>();
    session->deviceSerial = deviceSerial;
    session->localPort = localPort;
    session->sourceId = sourceId;
    session->running = true;

    Session* rawPtr = session.get();
    session->readerThread = std::thread(&AdbHandler::ReaderLoop, this, rawPtr, &mixer);

    sessions_.push_back(std::move(session));
    WM_LOG_INFO("AdbHandler: started capture for device " + deviceSerial);
    return sourceId;
}

void AdbHandler::StartCaptureAsync(Mixer& mixer, const std::string& deviceSerial, int localPort) {
    for (auto& p : pendingStarts_) {
        if (p->deviceSerial == deviceSerial && !p->done) return;
    }

    auto pending = std::make_unique<PendingStart>();
    pending->deviceSerial = deviceSerial;
    PendingStart* rawPtr = pending.get();

    pending->thread = std::thread([this, rawPtr, &mixer, deviceSerial, localPort]() {
        SourceId result = StartCaptureBlocking(mixer, deviceSerial, localPort);
        rawPtr->result = result;
        rawPtr->done = true;
    });

    pendingStarts_.push_back(std::move(pending));
}

bool AdbHandler::IsStarting(const std::string& deviceSerial) const {
    for (auto& p : pendingStarts_) {
        if (p->deviceSerial == deviceSerial && !p->done) return true;
    }
    return false;
}

bool AdbHandler::TryTakeStartResult(const std::string& deviceSerial, SourceId& outSourceId) {
    for (auto it = pendingStarts_.begin(); it != pendingStarts_.end(); ++it) {
        if ((*it)->deviceSerial == deviceSerial && (*it)->done) {
            outSourceId = (*it)->result;
            if ((*it)->thread.joinable()) (*it)->thread.join();
            pendingStarts_.erase(it);
            return true;
        }
    }
    return false;
}

void AdbHandler::ReaderLoop(Session* session, Mixer* mixer) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        WM_LOG_ERROR("AdbHandler: socket() failed.");
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(session->localPort));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    bool connected = false;
    for (int attempt = 0; attempt < 10 && session->running; ++attempt) {
        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            connected = true;
            break;
        }
        Sleep(500);
    }

    if (!connected) {
        WM_LOG_ERROR("AdbHandler: could not connect to forwarded sndcpy socket on port "
                      + std::to_string(session->localPort));
        closesocket(sock);
        return;
    }

    session->socket = static_cast<uintptr_t>(sock);
    WM_LOG_INFO("AdbHandler: connected to Android audio stream, playback starting.");

    DWORD recvTimeoutMs = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&recvTimeoutMs), sizeof(recvTimeoutMs));

    DWORD mmcssTaskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsA("Audio", &mmcssTaskIndex);
    if (!mmcssHandle) {
        WM_LOG_WARN("AdbHandler: AvSetMmThreadCharacteristics(\"Audio\") failed "
                     "(GetLastError=" + std::to_string(GetLastError()) + "). Android "
                     "audio capture will still work but may be more prone to stutter "
                     "under background CPU load.");
    }

    constexpr size_t kChunkFrames = 960;
    std::vector<int16_t> rawBuf(kChunkFrames * kSndcpyChannels);
    std::vector<float> floatBuf(kChunkFrames * kSndcpyChannels);

    int consecutiveTimeouts = 0;
    constexpr int kMaxConsecutiveTimeouts = 10;

    while (session->running) {
        size_t totalWanted = rawBuf.size() * sizeof(int16_t);
        size_t got = 0;
        char* dst = reinterpret_cast<char*>(rawBuf.data());
        bool sessionDead = false;

        while (got < totalWanted && session->running) {
            int n = recv(sock, dst + got, static_cast<int>(totalWanted - got), 0);

            if (n > 0) {
                got += static_cast<size_t>(n);
                consecutiveTimeouts = 0;
                continue;
            }

            if (n == 0) {
                WM_LOG_INFO("AdbHandler: Android audio stream ended (device unplugged or app stopped).");
                sessionDead = true;
                break;
            }

            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                consecutiveTimeouts++;
                if (consecutiveTimeouts >= kMaxConsecutiveTimeouts) {
                    WM_LOG_WARN("AdbHandler: no audio data from phone for " +
                                 std::to_string(kMaxConsecutiveTimeouts * 3) +
                                 "s, treating capture as stalled (phone may have locked, "
                                 "the app may have been backgrounded/killed, or the cable "
                                 "was disturbed). Stopping this source; restart capture from "
                                 "the Android panel if the phone is still connected.");
                    sessionDead = true;
                    break;
                }
                continue;
            }

            WM_LOG_WARN("AdbHandler: socket error during Android audio capture (WSA error " +
                         std::to_string(err) + "); ending this source.");
            sessionDead = true;
            break;
        }

        if (sessionDead || !session->running) {
            session->running = false;
            break;
        }

        size_t framesGot = got / sizeof(int16_t) / kSndcpyChannels;
        for (size_t i = 0; i < framesGot * kSndcpyChannels; ++i) {
            floatBuf[i] = static_cast<float>(rawBuf[i]) / 32768.0f;
        }
        mixer->PushSamples(session->sourceId, floatBuf.data(), framesGot);
    }

    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
    closesocket(sock);
    session->socket = static_cast<uintptr_t>(~0);

    uint64_t underrunFrames = mixer->GetUnderrunFrames(session->sourceId);
    double underrunMs = static_cast<double>(underrunFrames) / kSndcpySampleRate * 1000.0;
    WM_LOG_INFO("Android source for " + session->deviceSerial +
                " removed. Total time spent in underrun (audible silence gaps) "
                "this session: ~" + std::to_string(static_cast<long long>(underrunMs)) + "ms");

    mixer->RemoveSource(session->sourceId);
}

void AdbHandler::StopCapture(const std::string& deviceSerial) {
    for (auto& s : sessions_) {
        if (s->deviceSerial == deviceSerial && s->running) {
            s->running = false;
            if (s->socket != static_cast<uintptr_t>(~0)) {
                SOCKET sock = static_cast<SOCKET>(s->socket);
                shutdown(sock, SD_BOTH);
                closesocket(sock);
                s->socket = static_cast<uintptr_t>(~0);
            }
            if (s->readerThread.joinable()) s->readerThread.join();

            FireAndForgetAdb({"-s", deviceSerial, "shell", "am", "force-stop", "com.rom1v.sndcpy"});
            FireAndForgetAdb({"-s", deviceSerial, "forward", "--remove", "tcp:" + std::to_string(s->localPort)});
        }
    }
}

void AdbHandler::Shutdown() {
    StopAll();
}

void AdbHandler::StopAll() {
    if (asyncInitThread_.joinable()) asyncInitThread_.join();
    if (deviceScanThread_.joinable()) deviceScanThread_.join();

    for (auto& p : pendingStarts_) {
        if (p->thread.joinable()) p->thread.join();
    }
    pendingStarts_.clear();

    for (auto& s : sessions_) {
        s->running = false;
        if (s->socket != static_cast<uintptr_t>(~0)) {
            SOCKET sock = static_cast<SOCKET>(s->socket);
            shutdown(sock, SD_BOTH);
            closesocket(sock);
            s->socket = static_cast<uintptr_t>(~0);
        }
        if (s->readerThread.joinable()) s->readerThread.join();

        FireAndForgetAdb({"-s", s->deviceSerial, "shell", "am", "force-stop", "com.rom1v.sndcpy"});
        FireAndForgetAdb({"-s", s->deviceSerial, "forward", "--remove", "tcp:" + std::to_string(s->localPort)});
    }
    sessions_.clear();

    if (everInvokedAdb_) {
        FireAndForgetAdb({"kill-server"});
    }

    WSACleanup();
}

}
