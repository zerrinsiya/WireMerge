#pragma once
#include <string>
#include <mutex>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace wm {

//Global version string.
inline constexpr const char* kWireMergeVersion = "WireMerge_0.9.5";

enum class LogLevel { Debug, Info, Warn, Error };

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

class UiLog {
public:
    static UiLog& Instance();

    void Push(const std::string& line);

    std::vector<std::string> DrainAll();

private:
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

//INI-style config store.
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

}
