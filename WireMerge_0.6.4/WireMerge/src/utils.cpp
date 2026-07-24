#include "utils.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>

namespace wm {

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::SetLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.open(path, std::ios::out | std::ios::app);
}

static const char* LevelToStr(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

void Logger::Log(LogLevel level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);

    std::ostringstream line;
    line << "[" << buf << "] [" << LevelToStr(level) << "] " << msg;

    std::cout << line.str() << std::endl;
    if (file_.is_open()) {
        file_ << line.str() << std::endl;
        file_.flush();
    }
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool Config::Load(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(trimmed.substr(0, eq));
        std::string val = Trim(trimmed.substr(eq + 1));
        values_[key] = val;
    }
    return true;
}

bool Config::Save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return false;
    out << "# WireMerge configuration (auto-generated)\n";
    for (const auto& kv : values_) {
        out << kv.first << "=" << kv.second << "\n";
    }
    return true;
}

std::string Config::GetString(const std::string& key, const std::string& def) const {
    auto it = values_.find(key);
    return it == values_.end() ? def : it->second;
}

int Config::GetInt(const std::string& key, int def) const {
    auto it = values_.find(key);
    if (it == values_.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

float Config::GetFloat(const std::string& key, float def) const {
    auto it = values_.find(key);
    if (it == values_.end()) return def;
    try { return std::stof(it->second); } catch (...) { return def; }
}

bool Config::GetBool(const std::string& key, bool def) const {
    auto it = values_.find(key);
    if (it == values_.end()) return def;
    return it->second == "1" || it->second == "true" || it->second == "yes";
}

void Config::Set(const std::string& key, const std::string& value) { values_[key] = value; }
void Config::Set(const std::string& key, int value) { values_[key] = std::to_string(value); }
void Config::Set(const std::string& key, float value) { values_[key] = std::to_string(value); }
void Config::Set(const std::string& key, bool value) { values_[key] = value ? "1" : "0"; }

// ---------------------------------------------------------------------------
// UiLog
// ---------------------------------------------------------------------------

UiLog& UiLog::Instance() {
    static UiLog instance;
    return instance;
}

void UiLog::Push(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(line);
}

std::vector<std::string> UiLog::DrainAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.swap(lines_);
    return result;
}

} // namespace wm
