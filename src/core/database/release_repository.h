// release_repository.h
#pragma once

#include <stdexcept>
#include <string>
#include <sqlite3.h>

#include "models/release.h"
#include "db_utils.h"

class ReleaseRepository {
public:
    explicit ReleaseRepository(sqlite3* db)
        : db_(db) {
        if (!db_) {
            throw std::runtime_error("ReleaseRepository: db pointer is null");
        }
    }

    Release create(const std::string& appId,
                   const std::string& image,
                   const std::string& configVersion,
                   const std::string& status = "deploying") {
        const char* sql =
            "INSERT INTO releases (id, app_id, image, config_version, status, created_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6);";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("ReleaseRepository::create prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }

        Release release;
        release.id             = generateId("rel_");
        release.app_id         = appId;
        release.image          = image;
        release.config_version = configVersion;
        release.status         = status;
        release.created_at     = nowIso8601();

        sqlite3_bind_text(stmt, 1, release.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, release.app_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, release.image.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, release.config_version.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, release.status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, release.created_at.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw std::runtime_error(std::string("ReleaseRepository::create step failed: ") +
                                     sqlite3_errmsg(db_));
        }

        sqlite3_finalize(stmt);
        return release;
    }

    void updateStatus(const std::string& id,
                      const std::string& status) {
        const char* sql =
            "UPDATE releases "
            "SET status = ?1 "
            "WHERE id = ?2;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("ReleaseRepository::updateStatus prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }

        sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw std::runtime_error(std::string("ReleaseRepository::updateStatus step failed: ") +
                                     sqlite3_errmsg(db_));
        }

        sqlite3_finalize(stmt);
    }

private:
    sqlite3* db_;
};
