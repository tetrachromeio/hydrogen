#pragma once

#include "../status/core_status.h"
#include "../database/db_health.h"
#include "core_scheduler.h"

struct CoreContext {
    CoreScheduler scheduler;
    DBHealth::DatabaseConfig dbConfig;
};

// Initialise in-place instead of returning by value
inline void initCore(CoreContext& ctx) {
    // Initial state: BOOTING
    CoreStatus::set(CoreStatusCode::BOOTING);

    // Load DB config from aeon.toml
    ctx.dbConfig = DBHealth::loadConfig();  // sets FATAL if config is broken

    // Immediate first DB check
    DBHealth::checkDatabaseHealth(ctx.dbConfig);

    // Add periodic DB check every 5 seconds
    ctx.scheduler.addTask(
        "db_health",
        std::chrono::milliseconds(5000),
        [dbCfg = ctx.dbConfig]() {
            DBHealth::checkDatabaseHealth(dbCfg);
        }
    );

    ctx.scheduler.start();

    // If we got here without FATAL/DB_DOWN, mark as OK (for now)
    CoreStatus::set(CoreStatusCode::OK);
}
