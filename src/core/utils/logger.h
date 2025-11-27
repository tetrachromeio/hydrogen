#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <filesystem>

namespace Logger {

enum class Level { DEBUG, INFO, WARN, ERROR };

inline std::mutex& logMutex() {
    static std::mutex m;
    return m;
}

inline std::ofstream& logStream() {
    static std::ofstream ofs;
    return ofs;
}

inline std::ofstream& reqStream() {
    static std::ofstream ofs;
    return ofs;
}

inline void init(const std::string& path = "logs/hydrogen.log") {
    std::lock_guard<std::mutex> lock(logMutex());
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    logStream().open(path, std::ios::app);
    reqStream().open("logs/interface.log", std::ios::app);
}

inline std::string levelToStr(Level lvl) {
    switch (lvl) {
        case Level::DEBUG: return "DEBUG";
        case Level::INFO:  return "INFO";
        case Level::WARN:  return "WARN";
        case Level::ERROR: return "ERROR";
    }
    return "INFO";
}

inline void log(Level lvl, const std::string& msg) {
    std::lock_guard<std::mutex> lock(logMutex());
    if (!logStream().is_open()) return;
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    logStream() << "[" << buf << "] [" << levelToStr(lvl) << "] " << msg << std::endl;
}

inline void debug(const std::string& msg) { log(Level::DEBUG, msg); }
inline void info(const std::string& msg)  { log(Level::INFO, msg); }
inline void warn(const std::string& msg)  { log(Level::WARN, msg); }
inline void error(const std::string& msg) { log(Level::ERROR, msg); }

inline void logRequest(const std::string& method,
                       const std::string& path,
                       const std::string& clientIp,
                       int statusCode) {
    std::lock_guard<std::mutex> lock(logMutex());
    if (!reqStream().is_open()) return;
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    reqStream() << "[" << buf << "] "
                << clientIp << " "
                << method << " " << path << " "
                << statusCode << std::endl;
}

} // namespace Logger
