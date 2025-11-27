#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include "../status/core_status.h"

// Use toml11 (recommended) or replace with your parser of choice
// https://github.com/ToruNiina/toml11
#include "../utils/toml.hpp"

namespace DBHealth {

// ---------------------------------------------------------
// Structure representing parsed database config
// ---------------------------------------------------------
struct DatabaseConfig {
    std::string type;   // e.g. "sqlite"
    std::string path;   // "db.sqlite"
};

// ---------------------------------------------------------
// Load database config from aeon.toml
// ---------------------------------------------------------
inline DatabaseConfig loadConfig(const std::string& toml_path = "aeon.toml") {
    try {
        auto data = toml::parse(toml_path);

        const auto& db = toml::find(data, "database");

        DatabaseConfig cfg;
        cfg.type = toml::find<std::string>(db, "type");
        cfg.path = toml::find<std::string>(db, "path");

        return cfg;

    } catch (const std::exception& e) {
        std::cerr << "[DBHealth] Failed to load 'aeon.toml': " << e.what() << std::endl;
        CoreStatus::set(CoreStatusCode::FATAL);
        return {};
    }
}

// ---------------------------------------------------------
// SQLite health check
// ---------------------------------------------------------
inline bool checkSQLite(const std::string& path) {
    // Quick: can the file be opened?
    std::ifstream f(path);
    if (!f.good()) {
        std::cerr << "[DBHealth] SQLite file missing: " << path << std::endl;
        return false;
    }

    // Deep health (optional): attempt actual SQL open
    // (Requires <sqlite3.h>)
    #ifdef USE_SQLITE3
        sqlite3* db = nullptr;
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
            std::cerr << "[DBHealth] Cannot open SQLite DB: " << sqlite3_errmsg(db) << std::endl;
            sqlite3_close(db);
            return false;
        }
        sqlite3_close(db);
    #endif

    return true;
}

// ---------------------------------------------------------
// Main DB health check entry
// ---------------------------------------------------------
inline void checkDatabaseHealth(const DatabaseConfig& cfg) {
    if (cfg.type == "sqlite") {
        bool ok = checkSQLite(cfg.path);

        if (ok) {
            CoreStatus::set(CoreStatusCode::OK);
            //print("[DBHealth] SQLite database is healthy.\n");
        } else {
            CoreStatus::set(CoreStatusCode::DB_DOWN);
            print("[DBHealth] SQLite database health check failed.\n");
        }

    } else {
        std::cerr << "[DBHealth] Unsupported DB type: " << cfg.type << std::endl;
        CoreStatus::set(CoreStatusCode::FATAL);
    }
}

} // namespace DBHealth
