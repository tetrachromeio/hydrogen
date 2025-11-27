#include "Aether/aether.h"
#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>

#include "routes.cpp"
#include "core/database/db_health.h"
#include "core/utils/boot.cpp"
#include "core/scheduler/core_scheduler.h"
#include "core/scheduler/core_init.h"
#include "core/scheduler/instance_bootstrap.h"
#include "core/utils/logger.h"
#include <sqlite3.h>


using namespace Aether;
using namespace Aether::Http;


// set a universal status code variable our status tool can change automatically.



int main() {
    CoreContext core;
    initCore(core);

    Logger::init(); // file logging to logs/hydrogen.log

    sqlite3* db = nullptr;
    if (sqlite3_open(core.dbConfig.path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Failed to open database: " << sqlite3_errmsg(db) << std::endl;
        return 1;
    }

    ApiContext apiCtx{core, db};

    auto app = Server(); // Create the HTTP server

    bootLoader(); // Run bootloader checks

    registerRoutes(app, apiCtx); // Register all routes
    resetInstanceStates(db); // Clear stale running flags
    bootstrapInstances(db); // Start any instances that should be up

    std::cout << "[hydrogen] Starting HTTP server on :3000..." << std::endl;
    app.run(3000); // Listen on Port 3000
    std::cout << "[hydrogen] Server stopped." << std::endl;
    sqlite3_close(db);

    core.scheduler.stop();

    return 0;
}
