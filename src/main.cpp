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


using namespace Aether;
using namespace Aether::Http;


// set a universal status code variable our status tool can change automatically.



int main() {
    CoreContext core;
    initCore(core);
    
    auto app = Server(); // Create the HTTP server

    bootLoader(); // Run bootloader checks

    registerRoutes(app); // Register all routes

    app.run(3000); // Listen on Port 3000

    core.scheduler.stop();

    return 0;
}
