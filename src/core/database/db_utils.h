// db_utils.h
#pragma once
#include <string>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

inline std::string generateId(const std::string& prefix = "app_") {
    // very simple random hex id: app_<16-hex>
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<unsigned long long> dist;

    unsigned long long r = dist(gen);

    std::ostringstream oss;
    oss << prefix << std::hex << std::setw(16) << std::setfill('0') << r;
    return oss.str();
}

inline std::string nowIso8601() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}
