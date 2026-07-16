#pragma once
#include <string>
#include <mutex>
#include <fstream>
#include <unordered_map>

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
inline constexpr const char* kWireMergeVersion = "WireMerge_0.1.2";

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
