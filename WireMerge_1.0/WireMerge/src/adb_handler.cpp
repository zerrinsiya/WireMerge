#include "adb_handler.h"
#include "utils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <urlmon.h>
#include <avrt.h>
#include <sstream>
#include <fstream>
#include <iterator>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "urlmon.lib")
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

bool AdbHandler::Initialize(bool autoDownloadIfMissing) {
    std::string adb = FindNextToExe("adb.exe");
    std::string apk = FindNextToExe("sndcpy.apk");

    if ((adb.empty() || apk.empty()) && autoDownloadIfMissing) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string dir(exePath);
        size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) dir = dir.substr(0, slash + 1);
        std::string toolsDir = dir + "tools";

        WM_LOG_INFO("AdbHandler: tools/ incomplete. This is expected only if you're "
                     "running a stripped-down build; normal WireMerge distributions "
                     "ship tools/ already populated. Attempting a one-time auto-download "
                     "as a fallback (requires internet access)...");

        if (TryAutoDownload(toolsDir)) {
            adb = FindNextToExe("adb.exe");
            apk = FindNextToExe("sndcpy.apk");
        }
    }

    if (adb.empty()) {
        WM_LOG_WARN("AdbHandler: tools/adb.exe not found (and auto-download did not "
                     "provide it). Android app-audio capture will be unavailable. "
                     "Get platform-tools from "
                     "https://developer.android.com/tools/releases/platform-tools "
                     "and place adb.exe (+ its DLLs) in a 'tools' folder next to WireMerge.exe.");
    } else {
        adbPath_ = adb;
    }

    if (apk.empty()) {
        WM_LOG_WARN("AdbHandler: tools/sndcpy.apk not found (and auto-download did not "
                     "provide it). Android app-audio capture will be unavailable. Get it "
                     "from https://github.com/rom1v/sndcpy (download the release zip, "
                     "extract sndcpy.apk) and place it in a 'tools' folder next to WireMerge.exe.");
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

void AdbHandler::InitializeAsync(bool autoDownloadIfMissing) {
    if (asyncInitThread_.joinable()) asyncInitThread_.join();
    asyncInitThread_ = std::thread([this, autoDownloadIfMissing]() {
        Initialize(autoDownloadIfMissing);
    });
}

void AdbHandler::DownloadToolsAsync() {
    if (downloadInProgress_.load(std::memory_order_acquire)) return;
    if (downloadThread_.joinable()) downloadThread_.join();
    downloadInProgress_.store(true, std::memory_order_release);

    downloadThread_ = std::thread([this]() {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string dir(exePath);
        size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) dir = dir.substr(0, slash + 1);
        std::string toolsDir = dir + "tools";

        WM_LOG_INFO("AdbHandler: downloading tools (user-consented)...");
        TryAutoDownload(toolsDir);

        std::string adb = FindNextToExe("adb.exe");
        std::string apk = FindNextToExe("sndcpy.apk");
        if (!adb.empty()) adbPath_ = adb;
        if (!apk.empty()) apkPath_ = apk;

        if (adbPath_.has_value() && apkPath_.has_value()) {
            WM_LOG_INFO("AdbHandler: download complete. Android app-audio capture now available.");
        } else {
            WM_LOG_WARN("AdbHandler: download did not fully succeed. Android app-audio "
                         "capture still unavailable. See earlier log lines for detail.");
        }

        asyncInitDone_.store(true, std::memory_order_release);
        downloadInProgress_.store(false, std::memory_order_release);
    });
}

static bool DownloadToFile(const std::string& url, const std::string& destPath) {
    HRESULT hr = URLDownloadToFileA(nullptr, url.c_str(), destPath.c_str(), 0, nullptr);
    if (FAILED(hr)) {
        char hexBuf[16];
        snprintf(hexBuf, sizeof(hexBuf), "%08lX", static_cast<unsigned long>(hr));
        WM_LOG_ERROR("AdbHandler: download failed for " + url + " (hr=0x" + hexBuf + ")");
        return false;
    }
    return true;
}

static bool RunCommandSilent(const std::string& commandLine) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<char> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return false;

    WaitForSingleObject(pi.hProcess, 60000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0;
}

static bool ExtractZip(const std::string& zipPath, const std::string& destDir) {
    std::string cmd = "powershell.exe -NoProfile -Command \"Expand-Archive -LiteralPath '" +
                       zipPath + "' -DestinationPath '" + destDir + "' -Force\"";
    return RunCommandSilent(cmd);
}

static std::string FindFileRecursive(const std::string& dir, const std::string& targetName, int depth = 0) {
    if (depth > 4) return "";

    std::string pattern = dir + "\\*";
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return "";

    std::string result;
    do {
        std::string name = findData.cFileName;
        if (name == "." || name == "..") continue;
        std::string fullPath = dir + "\\" + name;

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            result = FindFileRecursive(fullPath, targetName, depth + 1);
        } else {
            std::string lowerName = name, lowerTarget = targetName;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
            std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), ::tolower);
            if (lowerName == lowerTarget) result = fullPath;
        }
    } while (result.empty() && FindNextFileA(hFind, &findData));

    FindClose(hFind);
    return result;
}

static std::string ExtractFirstAssetUrl(const std::string& json, const std::string& extension) {
    const std::string key = "\"browser_download_url\":\"";
    size_t pos = 0;
    while ((pos = json.find(key, pos)) != std::string::npos) {
        size_t start = pos + key.size();
        size_t end = json.find('"', start);
        if (end == std::string::npos) break;
        std::string url = json.substr(start, end - start);
        if (url.size() >= extension.size() &&
            url.compare(url.size() - extension.size(), extension.size(), extension) == 0) {
            return url;
        }
        pos = end;
    }
    return "";
}

bool AdbHandler::TryAutoDownload(const std::string& toolsDir) {
    struct ComGuard {
        bool initialized = false;
        ComGuard() {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            initialized = (hr == S_OK || hr == S_FALSE);
        }
        ~ComGuard() { if (initialized) CoUninitialize(); }
    } comGuard;

    CreateDirectoryA(toolsDir.c_str(), nullptr);

    bool adbOk = !FindNextToExe("adb.exe").empty();
    bool apkOk = !FindNextToExe("sndcpy.apk").empty();

    if (!adbOk) {
        std::string zipPath = toolsDir + "\\_dl_platform-tools.zip";
        std::string extractDir = toolsDir + "\\_dl_platform-tools";

        WM_LOG_INFO("AdbHandler: downloading platform-tools (adb.exe)...");
        if (DownloadToFile("https://dl.google.com/android/repository/platform-tools-latest-windows.zip",
                            zipPath) &&
            ExtractZip(zipPath, extractDir)) {

            for (const char* file : {"adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll"}) {
                std::string found = FindFileRecursive(extractDir, file);
                if (!found.empty()) {
                    CopyFileA(found.c_str(), (toolsDir + "\\" + file).c_str(), FALSE);
                }
            }
            adbOk = !FindNextToExe("adb.exe").empty();
        }

        DeleteFileA(zipPath.c_str());
        std::string cleanupCmd = "powershell.exe -NoProfile -Command \"Remove-Item -LiteralPath '" +
                                  extractDir + "' -Recurse -Force -ErrorAction SilentlyContinue\"";
        if (adbOk) RunCommandSilent(cleanupCmd);

        WM_LOG_INFO(std::string("AdbHandler: adb.exe auto-download ") + (adbOk ? "succeeded." : "FAILED."));
    }

    if (!apkOk) {
        std::string jsonPath = toolsDir + "\\_dl_sndcpy_release.json";
        WM_LOG_INFO("AdbHandler: resolving latest sndcpy release...");

        if (DownloadToFile("https://api.github.com/repos/rom1v/sndcpy/releases/latest", jsonPath)) {
            std::ifstream in(jsonPath, std::ios::binary);
            std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            DeleteFileA(jsonPath.c_str());

            std::string apkUrl = ExtractFirstAssetUrl(json, ".apk");
            std::string zipUrl = apkUrl.empty() ? ExtractFirstAssetUrl(json, ".zip") : "";

            if (!apkUrl.empty()) {
                apkOk = DownloadToFile(apkUrl, toolsDir + "\\sndcpy.apk");
            } else if (!zipUrl.empty()) {
                std::string zipPath = toolsDir + "\\_dl_sndcpy.zip";
                std::string extractDir = toolsDir + "\\_dl_sndcpy";
                if (DownloadToFile(zipUrl, zipPath) && ExtractZip(zipPath, extractDir)) {
                    std::string found = FindFileRecursive(extractDir, "sndcpy.apk");
                    if (!found.empty()) {
                        apkOk = CopyFileA(found.c_str(), (toolsDir + "\\sndcpy.apk").c_str(), FALSE);
                    }
                }
                DeleteFileA(zipPath.c_str());
                std::string cleanupCmd = "powershell.exe -NoProfile -Command \"Remove-Item -LiteralPath '" +
                                          extractDir + "' -Recurse -Force -ErrorAction SilentlyContinue\"";
                if (apkOk) RunCommandSilent(cleanupCmd);
            } else {
                WM_LOG_ERROR("AdbHandler: could not find any .apk or .zip asset in the "
                              "sndcpy latest-release API response; the release format may "
                              "have changed. Manual download required, see README.");
            }
        }

        WM_LOG_INFO(std::string("AdbHandler: sndcpy.apk auto-download ") + (apkOk ? "succeeded." : "FAILED."));
    }

    return adbOk && apkOk;
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
    if (downloadThread_.joinable()) downloadThread_.join();
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
