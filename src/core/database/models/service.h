// service.h
#pragma once
#include <string>

struct Service {
    std::string id;
    std::string app_id;
    std::string release_id;
    std::string name;
    std::string kind;
    std::string mode;
    int port;
    int min_replicas;
    int max_replicas;
    int desired_replicas;
    std::string state;
    std::string created_at; // ISO8601 string
    std::string updated_at; // ISO8601 string
};
