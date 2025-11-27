#include "../../../../core/database/app_repository.h"
#include "../../../../core/database/release_repository.h"
#include "../../../../core/database/service_repository.h"
#include "../../../../core/database/instance_repository.h"

using json = nlohmann::json;

static void registerAPIv1AppGetRoute(Server& app, ApiContext& ctx){
    app.get("/api/v1/app/:slug", [&ctx](Request& req, Response& res) {
        auto getParam = [&](const std::string& key) -> std::string {
            auto it = req.params.find(key);
            if (it != req.params.end()) return it->second;
            return "";
        };
        std::string slug = getParam(":slug");
        if (slug.empty()) slug = getParam("slug");
        if (slug.empty()) {
            res.setStatus(400);
            res.setHeader("Content-Type", "application/json");
            res.send(R"({"error":"invalid_request","message":"slug required"})");
            return;
        }

        try {
            AppRepository appRepo(ctx.db);
            ServiceRepository svcRepo(ctx.db);
            ReleaseRepository relRepo(ctx.db);
            InstanceRepository instRepo(ctx.db);

            // Fetch app
            // Minimal lookup by slug
            const char* appSql =
                "SELECT id, slug, default_domain, current_release_id, created_at, updated_at "
                "FROM apps WHERE slug = ?1 LIMIT 1;";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(ctx.db, appSql, -1, &stmt, nullptr) != SQLITE_OK) {
                throw std::runtime_error("app lookup failed");
            }
            sqlite3_bind_text(stmt, 1, slug.c_str(), -1, SQLITE_TRANSIENT);
            App app;
            bool found = false;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                found = true;
                app.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                app.slug = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                app.default_domain = sqlite3_column_type(stmt, 2) == SQLITE_NULL ? "" : reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                app.current_release_id = sqlite3_column_type(stmt, 3) == SQLITE_NULL ? "" : reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                app.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                app.updated_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            }
            sqlite3_finalize(stmt);
            if (!found) {
                res.setStatus(404);
                res.setHeader("Content-Type", "application/json");
                res.send(R"({"error":"not_found","message":"app not found"})");
                return;
            }

            // Fetch release (by current_release_id)
            Release release{};
            if (!app.current_release_id.empty()) {
                const char* relSql =
                    "SELECT id, app_id, image, config_version, status, created_at "
                    "FROM releases WHERE id = ?1 LIMIT 1;";
                if (sqlite3_prepare_v2(ctx.db, relSql, -1, &stmt, nullptr) != SQLITE_OK) {
                    throw std::runtime_error("release lookup failed");
                }
                sqlite3_bind_text(stmt, 1, app.current_release_id.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    release.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    release.app_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    release.image = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                    release.config_version = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                    release.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                    release.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                }
                sqlite3_finalize(stmt);
            }

            // Fetch services
            std::vector<Service> services;
            const char* svcSql =
                "SELECT id, app_id, release_id, name, kind, mode, port, "
                "min_replicas, max_replicas, desired_replicas, state, created_at, updated_at "
                "FROM services WHERE app_id = ?1;";
            if (sqlite3_prepare_v2(ctx.db, svcSql, -1, &stmt, nullptr) != SQLITE_OK) {
                throw std::runtime_error("service lookup failed");
            }
            sqlite3_bind_text(stmt, 1, app.id.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                Service svc;
                svc.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                svc.app_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                svc.release_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                svc.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                svc.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                svc.mode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                svc.port = sqlite3_column_int(stmt, 6);
                svc.min_replicas = sqlite3_column_int(stmt, 7);
                svc.max_replicas = sqlite3_column_int(stmt, 8);
                svc.desired_replicas = sqlite3_column_int(stmt, 9);
                svc.state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
                svc.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
                svc.updated_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
                services.push_back(svc);
            }
            sqlite3_finalize(stmt);

            // Fetch instances
            std::vector<Instance> instances;
            const char* instSql =
                "SELECT id, service_id, node, ip, port, state, started_at, last_heartbeat_at "
                "FROM instances WHERE service_id IN (SELECT id FROM services WHERE app_id = ?1);";
            if (sqlite3_prepare_v2(ctx.db, instSql, -1, &stmt, nullptr) != SQLITE_OK) {
                throw std::runtime_error("instance lookup failed");
            }
            sqlite3_bind_text(stmt, 1, app.id.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                Instance inst;
                inst.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                inst.service_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                inst.node = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                inst.ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                inst.port = sqlite3_column_int(stmt, 4);
                inst.state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                inst.started_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
                inst.last_heartbeat = sqlite3_column_type(stmt, 7) == SQLITE_NULL
                    ? "" : reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
                instances.push_back(inst);
            }
            sqlite3_finalize(stmt);

            json resp;
            resp["app"] = {
                {"id", app.id},
                {"slug", app.slug},
                {"default_domain", app.default_domain},
                {"current_release_id", app.current_release_id},
                {"created_at", app.created_at},
                {"updated_at", app.updated_at}
            };
            resp["release"] = {
                {"id", release.id},
                {"app_id", release.app_id},
                {"image", release.image},
                {"config_version", release.config_version},
                {"status", release.status},
                {"created_at", release.created_at}
            };
            resp["services"] = json::array();
            for (const auto& svc : services) {
                resp["services"].push_back({
                    {"id", svc.id},
                    {"app_id", svc.app_id},
                    {"release_id", svc.release_id},
                    {"name", svc.name},
                    {"kind", svc.kind},
                    {"mode", svc.mode},
                    {"port", svc.port},
                    {"min_replicas", svc.min_replicas},
                    {"max_replicas", svc.max_replicas},
                    {"desired_replicas", svc.desired_replicas},
                    {"state", svc.state},
                    {"created_at", svc.created_at},
                    {"updated_at", svc.updated_at}
                });
            }
            resp["instances"] = json::array();
            for (const auto& inst : instances) {
                resp["instances"].push_back({
                    {"id", inst.id},
                    {"service_id", inst.service_id},
                    {"node", inst.node},
                    {"ip", inst.ip},
                    {"port", inst.port},
                    {"state", inst.state},
                    {"started_at", inst.started_at},
                    {"last_heartbeat", inst.last_heartbeat}
                });
            }

            res.setStatus(200);
            res.setHeader("Content-Type", "application/json");
            res.send(resp.dump());

        } catch (const std::exception& e) {
            res.setStatus(500);
            res.setHeader("Content-Type", "application/json");
            json err;
            err["error"] = "server_error";
            err["message"] = e.what();
            res.send(err.dump());
        }
    });
}
