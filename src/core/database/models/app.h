// app.h
#pragma once
#include <string>

struct App {
    std::string id;
    std::string slug;
    std::string default_domain;
    std::string current_release_id;
    std::string created_at;   // ISO8601 string
    std::string updated_at;   // ISO8601 string
};
