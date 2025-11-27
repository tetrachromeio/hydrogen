using json = nlohmann::json;
#include "../../../../core/database/app_repository.h"

static void registerAPIv1AppsDeployRoute(Server& app, ApiContext& ctx){
    app.post("/api/v1/app/deploy", [&ctx](Request& req, Response& res) {

    try {
            // 1. Parse JSON body
            json body = json::parse(req.getBody());

            std::string project_slug  = body.at("project_slug").get<std::string>();
            std::string image         = body.at("image").get<std::string>();
            std::string config_hash   = body.at("config_version").get<std::string>();
            json        cloud_config  = body.at("cloud_config");



            // Derive a default domain if you want
            std::string defaultDomain = project_slug + ".tetr4.dev";

            AppRepository appRepo(ctx.db);
            App app = appRepo.upsertBySlug(project_slug, defaultDomain);

            // TODO: 2. Upsert App in DB (apps table)
            // auto appModel = ctx.appRepo.upsertBySlug(project_slug, ...);

            // TODO: 3. Create new Release row
            // auto release = ctx.releaseRepo.create(appModel.id, image, config_hash, ...);

            // TODO: 4. Parse services from cloud_config["services"] and upsert Service rows

            // TODO: 5. Kick scheduler/compute (for now, you can just log)

            json resp;
            resp["app"] = {
                {"id", app.id},
                {"slug", app.slug},
                {"default_domain", app.default_domain},
                {"created_at", app.created_at},
                {"updated_at", app.updated_at}
            };
            
            resp["release"] = {
                {"image", image},
                {"config_version", config_hash},
                {"status", "deploying"}
            };

            res.setStatus(201);
            res.setHeader("Content-Type", "application/json");
            res.send(resp.dump());

        } catch (const std::exception& e) {
            json err;
            err["error"] = "invalid_request";
            err["message"] = e.what();

            res.setStatus(400);
            res.setHeader("Content-Type", "application/json");
            res.send(err.dump());
    }

});
}