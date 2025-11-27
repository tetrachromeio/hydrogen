// Aggregate route source so Aeon picks it up (it only compiles src/*.cpp)
#include "routes/status.cpp"
#include "routes/api_v1.cpp"


struct ApiContext {
    CoreContext& core;
    sqlite3* db;
};



void registerRoutes(Server& app) {
    registerHealthRoute(app);
    registerStatusRoute(app);

    registerAPIv1Routes(Server& app, ApiContext& ctx);
}
