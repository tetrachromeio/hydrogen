// service_repository.h
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <sqlite3.h>

#include "models/service.h"
#include "db_utils.h"

class ServiceRepository {
public:
    explicit ServiceRepository(sqlite3* db)
        : db_(db) {
        if (!db_) {
            throw std::runtime_error("ServiceRepository: db pointer is null");
        }
    }

    Service upsert(const std::string& appId,
                   const std::string& releaseId,
                   const std::string& name,
                   const std::string& kind,
                   const std::string& mode,
                   int port,
                   int minReplicas,
                   int maxReplicas,
                   int desiredReplicas,
                   const std::string& state) {
        if (auto existing = findByAppAndName(appId, name)) {
            return updateExisting(existing->id, releaseId, kind, mode, port, minReplicas, maxReplicas, desiredReplicas, state);
        }
        return insertNew(appId, releaseId, name, kind, mode, port, minReplicas, maxReplicas, desiredReplicas, state);
    }

private:
    std::optional<Service> findByAppAndName(const std::string& appId,
                                            const std::string& name) {
        const char* sql =
            "SELECT id, app_id, release_id, name, kind, mode, port, "
            "min_replicas, max_replicas, desired_replicas, state, "
            "created_at, updated_at "
            "FROM services "
            "WHERE app_id = ?1 AND name = ?2 "
            "LIMIT 1;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("ServiceRepository::findByAppAndName prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }

        sqlite3_bind_text(stmt, 1, appId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);

        std::optional<Service> result;
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            Service svc;
            svc.id               = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            svc.app_id           = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            svc.release_id       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            svc.name             = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            svc.kind             = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            svc.mode             = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            svc.port             = sqlite3_column_int(stmt, 6);
            svc.min_replicas     = sqlite3_column_int(stmt, 7);
            svc.max_replicas     = sqlite3_column_int(stmt, 8);
            svc.desired_replicas = sqlite3_column_int(stmt, 9);
            svc.state            = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
            svc.created_at       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
            svc.updated_at       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
            result = svc;
        } else if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw std::runtime_error(std::string("ServiceRepository::findByAppAndName step failed: ") +
                                     sqlite3_errmsg(db_));
        }

        sqlite3_finalize(stmt);
        return result;
    }

    Service insertNew(const std::string& appId,
                      const std::string& releaseId,
                      const std::string& name,
                      const std::string& kind,
                      const std::string& mode,
                      int port,
                      int minReplicas,
                      int maxReplicas,
                      int desiredReplicas,
                      const std::string& state) {
        const char* sql =
            "INSERT INTO services "
            "(id, app_id, release_id, name, kind, mode, port, "
            "min_replicas, max_replicas, desired_replicas, state, created_at, updated_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13);";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("ServiceRepository::insertNew prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }

        Service svc;
        svc.id               = generateId("svc_");
        svc.app_id           = appId;
        svc.release_id       = releaseId;
        svc.name             = name;
        svc.kind             = kind;
        svc.mode             = mode;
        svc.port             = port;
        svc.min_replicas     = minReplicas;
        svc.max_replicas     = maxReplicas;
        svc.desired_replicas = desiredReplicas;
        svc.state            = state;
        svc.created_at       = nowIso8601();
        svc.updated_at       = svc.created_at;

        bindService(stmt, svc);

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw std::runtime_error(std::string("ServiceRepository::insertNew step failed: ") +
                                     sqlite3_errmsg(db_));
        }

        sqlite3_finalize(stmt);
        return svc;
    }

    Service updateExisting(const std::string& id,
                           const std::string& releaseId,
                           const std::string& kind,
                           const std::string& mode,
                           int port,
                           int minReplicas,
                           int maxReplicas,
                           int desiredReplicas,
                           const std::string& state) {
        const char* sql =
            "UPDATE services "
            "SET release_id = ?1, kind = ?2, mode = ?3, port = ?4, "
            "min_replicas = ?5, max_replicas = ?6, desired_replicas = ?7, "
            "state = ?8, updated_at = ?9 "
            "WHERE id = ?10;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("ServiceRepository::updateExisting prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }

        std::string updatedAt = nowIso8601();

        sqlite3_bind_text(stmt, 1, releaseId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, mode.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, port);
        sqlite3_bind_int(stmt, 5, minReplicas);
        sqlite3_bind_int(stmt, 6, maxReplicas);
        sqlite3_bind_int(stmt, 7, desiredReplicas);
        sqlite3_bind_text(stmt, 8, state.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, updatedAt.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 10, id.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw std::runtime_error(std::string("ServiceRepository::updateExisting step failed: ") +
                                     sqlite3_errmsg(db_));
        }

        sqlite3_finalize(stmt);

        // Return merged service
        Service svc;
        svc.id               = id;
        svc.app_id           = ""; // not queried here
        svc.release_id       = releaseId;
        svc.name             = "";
        svc.kind             = kind;
        svc.mode             = mode;
        svc.port             = port;
        svc.min_replicas     = minReplicas;
        svc.max_replicas     = maxReplicas;
        svc.desired_replicas = desiredReplicas;
        svc.state            = state;
        svc.created_at       = "";
        svc.updated_at       = updatedAt;
        return svc;
    }

    void bindService(sqlite3_stmt* stmt, const Service& svc) {
        sqlite3_bind_text(stmt, 1, svc.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, svc.app_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, svc.release_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, svc.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, svc.kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, svc.mode.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 7, svc.port);
        sqlite3_bind_int(stmt, 8, svc.min_replicas);
        sqlite3_bind_int(stmt, 9, svc.max_replicas);
        sqlite3_bind_int(stmt, 10, svc.desired_replicas);
        sqlite3_bind_text(stmt, 11, svc.state.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 12, svc.created_at.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 13, svc.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    }

    sqlite3* db_;
};
