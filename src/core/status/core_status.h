// core_status.h
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <chrono>

enum class CoreStatusCode {
    OK,
    BOOTING,
    SYNCING,
    DEGRADED,
    MAINTENANCE,
    READ_ONLY_MODE,
    DB_DOWN,
    DB_DEGRADED,
    SCHEDULER_DOWN,
    COMPUTE_DRIVER_DOWN,
    NETWORK_PARTITION,
    REGISTRY_STALE,
    NO_CAPACITY,
    CRITICAL,
    SUSPENDED,
    FATAL
};

inline const char* to_string(CoreStatusCode code) {
    switch (code) {
        case CoreStatusCode::OK:                  return "OK";
        case CoreStatusCode::BOOTING:             return "BOOTING";
        case CoreStatusCode::SYNCING:             return "SYNCING";
        case CoreStatusCode::DEGRADED:            return "DEGRADED";
        case CoreStatusCode::MAINTENANCE:         return "MAINTENANCE";
        case CoreStatusCode::READ_ONLY_MODE:      return "READ_ONLY_MODE";
        case CoreStatusCode::DB_DOWN:             return "DB_DOWN";
        case CoreStatusCode::DB_DEGRADED:         return "DB_DEGRADED";
        case CoreStatusCode::SCHEDULER_DOWN:      return "SCHEDULER_DOWN";
        case CoreStatusCode::COMPUTE_DRIVER_DOWN: return "COMPUTE_DRIVER_DOWN";
        case CoreStatusCode::NETWORK_PARTITION:   return "NETWORK_PARTITION";
        case CoreStatusCode::REGISTRY_STALE:      return "REGISTRY_STALE";
        case CoreStatusCode::NO_CAPACITY:         return "NO_CAPACITY";
        case CoreStatusCode::CRITICAL:            return "CRITICAL";
        case CoreStatusCode::SUSPENDED:           return "SUSPENDED";
        case CoreStatusCode::FATAL:               return "FATAL";
    }
    return "UNKNOWN";
}

struct CoreStatusSnapshot {
    CoreStatusCode code;
    std::int64_t   updated_at_ms;  // unix ms since epoch
};

namespace CoreStatus {

// internal globals
inline std::atomic<CoreStatusCode> g_code{CoreStatusCode::BOOTING};
inline std::atomic<std::int64_t>   g_updated_at_ms{0};

// helper: now in ms
inline std::int64_t now_ms() {
    using namespace std::chrono;
    auto now = time_point_cast<milliseconds>(system_clock::now());
    return now.time_since_epoch().count();
}

// set from anywhere in the core
inline void set(CoreStatusCode code) {
    g_code.store(code, std::memory_order_relaxed);
    g_updated_at_ms.store(now_ms(), std::memory_order_relaxed);
}

// read snapshot (for /status handler)
inline CoreStatusSnapshot get() {
    CoreStatusSnapshot s;
    s.code          = g_code.load(std::memory_order_relaxed);
    s.updated_at_ms = g_updated_at_ms.load(std::memory_order_relaxed);
    return s;
}

} // namespace CoreStatus


/*

USAGE EXAMPLE:

#include "core_status.h"

// e.g. when DB goes down:
CoreStatus::set(CoreStatusCode::DB_DOWN);

// when everything is healthy again:
CoreStatus::set(CoreStatusCode::OK);

*/