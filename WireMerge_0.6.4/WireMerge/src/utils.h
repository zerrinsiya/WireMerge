#pragma once
#include <string>
#include <mutex>
#include <fstream>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// utils.h
// Small dependency-free helpers used across WireMerge: logging + a tiny
// key=value config file reader/writer (for persisting last-used devices,
// per-source volumes, window size, etc).
// ---------------------------------------------------------------------------

namespace wm {

// Single source of truth for the version string. Update this one line on
// release; everything else (window title, log startup line, About text)
// reads from here rather than hardcoding the version separately.
inline constexpr const char* kWireMergeVersion = "WireMerge_0.6.4";

enum class LogLevel { Debug, Info, Warn, Error };

// Thread-safe logger. Writes to stdout (visible if launched from a console)
// and to WireMerge.log next to the executable. Cheap enough to call from
// the audio callback's *setup* code, but avoid calling it from inside the
// realtime PortAudio callback itself (file I/O there can cause glitches).
class Logger {
public:
    static Logger& Instance();

    void SetLogFile(const std::string& path);
    void Log(LogLevel level, const std::string& msg);

    void Debug(const std::string& msg) { Log(LogLevel::Debug, msg); }
    void Info(const std::string& msg)  { Log(LogLevel::Info, msg); }
    void Warn(const std::string& msg)  { Log(LogLevel::Warn, msg); }
    void Error(const std::string& msg) { Log(LogLevel::Error, msg); }

private:
    Logger() = default;
    std::mutex mutex_;
    std::ofstream file_;
};

#define WM_LOG_DEBUG(msg) ::wm::Logger::Instance().Debug(msg)
#define WM_LOG_INFO(msg)  ::wm::Logger::Instance().Info(msg)
#define WM_LOG_WARN(msg)  ::wm::Logger::Instance().Warn(msg)
#define WM_LOG_ERROR(msg) ::wm::Logger::Instance().Error(msg)

// A small, CURATED buffer of user-facing messages, separate from Logger's
// full internal log (which captures everything, including verbose detail
// not meant for end users). This exists specifically to solve one
// ordering problem: main.cpp's boot sequence (PortAudio/libusb/ADB
// readiness, the auto-download fallback, etc) runs BEFORE any Gui
// instance exists, so those messages have nowhere to go if Gui::PushLogLine
// were the only sink -- there's no Gui object yet to call it on. Boot
// code explicitly pushes the specific lines worth user visibility here
// (not a mirror of every WM_LOG_INFO call -- that's the "curated"/"not
// all of it, just some" part); Gui::Initialize() drains this once at
// startup to seed its own Log panel, then continues operating normally
// via PushLogLine for everything that happens after the GUI exists.
class UiLog {
public:
    static UiLog& Instance();

    void Push(const std::string& line);

    // Returns everything pushed so far and clears the internal buffer --
    // meant to be called exactly once, by Gui::Initialize(), to seed the
    // UI's own log deque. Calling it again after that returns nothing
    // useful (which is fine, since ongoing runtime messages after Gui
    // exists go through Gui::PushLogLine directly instead).
    std::vector<std::string> DrainAll();

private:
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

// Very small INI-style config store: "key=value" per line, '#' comments.
// Kept intentionally simple -- no nested sections, no external deps.
class Config {
public:
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;

    std::string GetString(const std::string& key, const std::string& def = "") const;
    int GetInt(const std::string& key, int def = 0) const;
    float GetFloat(const std::string& key, float def = 0.0f) const;
    bool GetBool(const std::string& key, bool def = false) const;

    void Set(const std::string& key, const std::string& value);
    void Set(const std::string& key, int value);
    void Set(const std::string& key, float value);
    void Set(const std::string& key, bool value);

private:
    std::unordered_map<std::string, std::string> values_;
};

} // namespace wm
