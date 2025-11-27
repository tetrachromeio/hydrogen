// instance_repository.h
#pragma once

#include <stdexcept>
#include <string>
#include <vector>
#include <optional>
#include <sqlite3.h>

#include "models/instance.h"
#include "db_utils.h"

class InstanceRepository {
public:
    explicit InstanceRepository(sqlite3* db)
        : db_(db) {
        if (!db_) throw std::runtime_error("InstanceRepository: db pointer is null");
    }

    Instance create(const std::string& serviceId,
                    const std::string& node,
                    const std::string& ip,
                    int port,
                    const std::string& state = "starting") {
        const char* sql =
            "INSERT INTO instances "
            "(id, service_id, node, ip, port, state, started_at, last_heartbeat_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, NULL);";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("InstanceRepository::create prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }

        Instance inst;
        inst.id              = generateId("inst_");
        inst.service_id      = serviceId;
        inst.node            = node;
        inst.ip              = ip;
        inst.port            = port;
        inst.state           = state;
        inst.started_at      = nowIso8601();
        inst.last_heartbeat  = "";

        sqlite3_bind_text(stmt, 1, inst.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, inst.service_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, inst.node.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, inst.ip.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, inst.port);
        sqlite3_bind_text(stmt, 6, inst.state.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, inst.started_at.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw std::runtime_error(std::string("InstanceRepository::create step failed: ") +
                                     sqlite3_errmsg(db_));
        }
        sqlite3_finalize(stmt);
        return inst;
    }

    void updateState(const std::string& id, const std::string& state) {
        const char* sql =
            "UPDATE instances "
            "SET state = ?1, last_heartbeat_at = ?2 "
            "WHERE id = ?3;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("InstanceRepository::updateState prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }

        std::string now = nowIso8601();
        sqlite3_bind_text(stmt, 1, state.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, id.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw std::runtime_error(std::string("InstanceRepository::updateState step failed: ") +
                                     sqlite3_errmsg(db_));
        }
        sqlite3_finalize(stmt);
    }

private:
    sqlite3* db_;
};
