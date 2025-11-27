// app_repository.h
#pragma once

#include <stdexcept>
#include <string>
#include <sqlite3.h>
#include "models/app.h"
#include "db_utils.h"

class AppRepository {
public:
    explicit AppRepository(sqlite3* db)
        : db_(db) {
        if (!db_) {
            throw std::runtime_error("AppRepository: db pointer is null");
        }
    }

    // Upsert an app by slug:
    // - if slug exists, return existing row
    // - else insert new row with generated id + default domain
    App upsertBySlug(const std::string& slug,
                     const std::string& default_domain = "") {
        // 1. Try to find existing app by slug
        if (auto existing = findBySlug(slug)) {
            return *existing;
        }

        // 2. Insert new app
        return insertNew(slug, default_domain);
    }

private:
    sqlite3* db_;

    std::optional<App> findBySlug(const std::string& slug) {
        const char* sql =
            "SELECT id, slug, default_domain, "
            "       COALESCE(current_release_id, ''), "
            "       created_at, updated_at "
            "FROM apps "
            "WHERE slug = ?1 "
            "LIMIT 1;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("AppRepository::findBySlug prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }

        sqlite3_bind_text(stmt, 1, slug.c_str(), -1, SQLITE_TRANSIENT);

        std::optional<App> result;

        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            App app;
            app.id                 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            app.slug               = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            app.default_domain     = sqlite3_column_type(stmt, 2) == SQLITE_NULL
                                       ? ""
                                       : reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            app.current_release_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            app.created_at         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            app.updated_at         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            result = app;
        } else if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw std::runtime_error(std::string("AppRepository::findBySlug step failed: ") +
                                     sqlite3_errmsg(db_));
        }

        sqlite3_finalize(stmt);
        return result;
    }

    App insertNew(const std::string& slug,
                  const std::string& default_domain) {
        const char* sql =
            "INSERT INTO apps (id, slug, default_domain, "
            "                  current_release_id, created_at, updated_at) "
            "VALUES (?1, ?2, ?3, NULL, ?4, ?5);";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("AppRepository::insertNew prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }

        std::string id        = generateId("app_");
        std::string createdAt = nowIso8601();
        std::string updatedAt = createdAt;

        sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, slug.c_str(), -1, SQLITE_TRANSIENT);

        if (!default_domain.empty()) {
            sqlite3_bind_text(stmt, 3, default_domain.c_str(), -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, 3);
        }

        sqlite3_bind_text(stmt, 4, createdAt.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, updatedAt.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw std::runtime_error(std::string("AppRepository::insertNew step failed: ") +
                                     sqlite3_errmsg(db_));
        }

        sqlite3_finalize(stmt);

        App app;
        app.id                 = id;
        app.slug               = slug;
        app.default_domain     = default_domain;
        app.current_release_id = "";
        app.created_at         = createdAt;
        app.updated_at         = updatedAt;
        return app;
    }
};
