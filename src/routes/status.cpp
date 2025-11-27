#include "../core/status/core_status.h"

using namespace Aether::Http;



void registerStatusRoute(Server& app) {
    app.get("/status", [](Request& req, Response& res) {
        auto snap = CoreStatus::get();

        int http_status = 200;
        switch (snap.code) {
            case CoreStatusCode::OK:
            case CoreStatusCode::BOOTING:
            case CoreStatusCode::SYNCING:
            case CoreStatusCode::DEGRADED:
            case CoreStatusCode::DB_DEGRADED:
                http_status = 200;
                break;

            default:
                http_status = 503;
                break;
        }

        std::string body = "{";
        body += "\"status\":\""; 
        body += to_string(snap.code);
        body += "\",";
        body += "\"updated_at_ms\":";
        body += std::to_string(snap.updated_at_ms);
        body += "}";

        res.setStatus(http_status);
        res.setHeader("Content-Type", "application/json");
        res.send(body);
    });
}


void registerHealthRoute(Server& app) {
    app.get("/health", [](Request& req, Response& res) {
        res.send("OK");
    });
}