#pragma once

#include <sqlite3.h>
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>
#include <boost/asio.hpp>
#include <sstream>

#include "../database/instance_repository.h"

inline bool tryConnectTcp(const std::string& ip, int port) {
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket sock(io);
    boost::system::error_code ec;
    auto addr = boost::asio::ip::make_address(ip, ec);
    if (ec) return false;
    sock.connect({addr, static_cast<unsigned short>(port)}, ec);
    return !ec;
}

inline bool spawnInstanceProcess(const std::filesystem::path& binPath, int port, const std::string& instanceId, const std::string& appId) {
    std::filesystem::path absPath = std::filesystem::weakly_canonical(binPath);
    std::filesystem::create_directories("logs/apps");
    std::string logPath = "logs/apps/" + appId + ".log";
    int fd = ::open(logPath.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);

    pid_t pid = ::fork();
    if (pid < 0) {
        if (fd >= 0) ::close(fd);
        return false;
    }
    if (pid == 0) {
        if (fd >= 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            ::close(fd);
        }
        std::string portStr = std::to_string(port);
        ::setenv("PORT", portStr.c_str(), 1);
        std::string argv0 = absPath.filename().string();
        std::string execPath = absPath.string();
        ::execl(execPath.c_str(), argv0.c_str(), (char*)nullptr);
        _exit(127);
    }
    if (fd >= 0) ::close(fd);

    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (tryConnectTcp("127.0.0.1", port)) return true;
    }
    return false;
}

inline void resetInstanceStates(sqlite3* db) {
    if (!db) return;
    const char* sql = "UPDATE instances SET state = 'stopped' WHERE state IN ('ready','starting');";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

inline void bootstrapInstances(sqlite3* db) {
    if (!db) return;
    InstanceRepository instRepo(db);

    auto allocatePort = []() -> int {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor acc(io);
        acc.open(boost::asio::ip::tcp::v4());
        acc.bind({boost::asio::ip::tcp::v4(), 0});
        int port = acc.local_endpoint().port();
        acc.close();
        return port;
    };

    // For each service of any release, ensure at least one ready/starting instance exists.
    const char* svcSql =
        "SELECT services.id, releases.image, services.desired_replicas, services.app_id "
        "FROM services "
        "JOIN releases ON releases.id = services.release_id "
        "WHERE services.mode != 'cold_start';";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, svcSql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[bootstrap] failed to prepare services query: " << sqlite3_errmsg(db) << std::endl;
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string serviceId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string image     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        int desired           = sqlite3_column_int(stmt, 2);
        std::string appId     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (desired <= 0) desired = 1;

        // Count existing ready/starting instances for this service
        int count = 0;
        const char* countSql =
            "SELECT COUNT(*) FROM instances WHERE service_id = ?1 AND state IN ('ready','starting');";
        sqlite3_stmt* countStmt = nullptr;
        if (sqlite3_prepare_v2(db, countSql, -1, &countStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(countStmt, 1, serviceId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(countStmt) == SQLITE_ROW) {
                count = sqlite3_column_int(countStmt, 0);
            }
        }
        sqlite3_finalize(countStmt);

        int target = std::max(1, desired); // always ensure at least one
        int toStart = target - count;
        if (toStart <= 0) {
            Logger::info("[bootstrap] service " + serviceId + " already has " + std::to_string(count) + " running/starting (target " + std::to_string(target) + ")");
            continue;
        }

        std::filesystem::path binPath;
        if (image.rfind("/uploads/", 0) == 0) {
            binPath = std::filesystem::path("uploads") / image.substr(std::string("/uploads/").size());
        } else {
            binPath = image;
        }
        if (!std::filesystem::exists(binPath)) {
            Logger::warn("[bootstrap] binary missing for service " + serviceId + " at " + binPath.string());
            // Mark existing instances as failed so they aren't considered ready
            const char* failSql =
                "UPDATE instances SET state = 'failed' WHERE service_id = ?1;";
            sqlite3_stmt* failStmt = nullptr;
            if (sqlite3_prepare_v2(db, failSql, -1, &failStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(failStmt, 1, serviceId.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(failStmt);
            }
            sqlite3_finalize(failStmt);
            continue;
        }

        for (int i = 0; i < toStart; ++i) {
            int port = allocatePort();
            Instance inst = instRepo.create(
                serviceId,
                "local-node",
                "127.0.0.1",
                port,
                "starting"
            );

            Logger::info("[bootstrap] starting instance " + inst.id + " at 127.0.0.1:" + std::to_string(port) + " for service " + serviceId);
            bool ok = spawnInstanceProcess(binPath, port, inst.id, appId);
            if (ok) {
                instRepo.updateState(inst.id, "ready");
                Logger::info("[bootstrap] instance " + inst.id + " ready on 127.0.0.1:" + std::to_string(port));
            } else {
                instRepo.updateState(inst.id, "failed");
                Logger::warn("[bootstrap] instance " + inst.id + " failed on 127.0.0.1:" + std::to_string(port));
            }
        }
    }
    sqlite3_finalize(stmt);
}
