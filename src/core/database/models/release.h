// release.h
#pragma once
#include <string>

struct Release {
    std::string id;
    std::string app_id;
    std::string image;
    std::string config_version;
    std::string status;
    std::string created_at; // ISO8601 string
};
