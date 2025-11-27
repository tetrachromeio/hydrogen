// instance.h
#pragma once
#include <string>

struct Instance {
    std::string id;
    std::string service_id;
    std::string node;
    std::string ip;
    int port;
    std::string state;           // starting | ready | draining | failed
    std::string started_at;      // ISO8601
    std::string last_heartbeat;  // ISO8601 or empty
};
