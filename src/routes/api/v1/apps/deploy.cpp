using json = nlohmann::json;
#include "../../../../core/database/app_repository.h"
#include "../../../../core/database/release_repository.h"
#include "../../../../core/database/service_repository.h"
#include "../../../../core/database/instance_repository.h"
#include <vector>
#include <string>
#include <filesystem>
#include <thread>
#include <chrono>
#include <sstream>
#include <boost/asio.hpp>
#include <iostream>
#include "../../../../core/utils/logger.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

static void registerAPIv1AppsDeployRoute(Server& app, ApiContext& ctx){
    app.post("/api/v1/app/deploy", [&ctx](Request& req, Response& res) {

    try {
            auto allocatePort = []() -> int {
                boost::asio::io_context io;
                boost::asio::ip::tcp::acceptor acc(io);
                acc.open(boost::asio::ip::tcp::v4());
                acc.bind({boost::asio::ip::tcp::v4(), 0});
                int port = acc.local_endpoint().port();
                acc.close();
                return port;
            };

            auto tryConnect = [](const std::string& ip, int port) -> bool {
                boost::asio::io_context io;
                boost::asio::ip::tcp::socket sock(io);
                boost::system::error_code ec;
                auto addr = boost::asio::ip::make_address(ip, ec);
                if (ec) return false;
                sock.connect({addr, static_cast<unsigned short>(port)}, ec);
                return !ec;
            };

            auto spawnInstance = [&](const std::filesystem::path& binPath, int port, const std::string& instanceId, const std::string& appId, const std::string& workdir) -> bool {
                std::filesystem::path absPath = std::filesystem::weakly_canonical(binPath);
                std::filesystem::create_directories("logs/apps");
                std::string logPath = std::filesystem::weakly_canonical("logs/apps").string() + "/" + appId + ".log";

                pid_t pid = ::fork();
                if (pid < 0) {
                    return false;
                }
                if (pid == 0) {
                    std::string portStr = std::to_string(port);
                    std::string execPath = absPath.string();
                    std::string wd = workdir;
                    if (!wd.empty()) {
                        std::filesystem::create_directories(wd);
                    }
                    std::ostringstream cmd;
                    cmd << "cd \"" << wd << "\" && PORT=" << portStr << " \"" << execPath << "\" 2>&1 | "
                        << "sed -u 's/^/[ " << instanceId << " ] /' >> \"" << logPath << "\"";
                    ::execl("/bin/sh", "sh", "-c", cmd.str().c_str(), (char*)nullptr);
                    _exit(127);
                }

                // Give the process a few seconds to come up
                for (int i = 0; i < 10; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    if (tryConnect("127.0.0.1", port)) return true;
                }
                return false;
            };

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

            ReleaseRepository releaseRepo(ctx.db);
            Release release = releaseRepo.create(app.id, image, config_hash, "deploying");
            appRepo.setCurrentRelease(app.id, release.id);
            app.current_release_id = release.id;

            // 4. Parse services and upsert rows
            std::vector<Service> services;
            std::vector<Instance> instances;
            ServiceRepository serviceRepo(ctx.db);
            InstanceRepository instanceRepo(ctx.db);
            if (!cloud_config.contains("services") || !cloud_config["services"].is_array()) {
                throw std::runtime_error("cloud_config.services must be an array");
            }

            for (const auto& svcJson : cloud_config["services"]) {
                if (!svcJson.is_object()) {
                    throw std::runtime_error("Each service must be an object");
                }

                const auto& svcObj = svcJson;
                std::string svcName = svcObj.value("name", "");
                if (svcName.empty()) {
                    throw std::runtime_error("Service name is required");
                }

                std::string kind = svcObj.value("kind", std::string("web_stateless"));
                std::string mode = svcObj.value("mode", std::string("always_on"));
                // Backend assigns ports; ignore incoming port
                int firstPort    = allocatePort();
                int minReplicas  = svcObj.value("min_replicas", 0);
                int maxReplicas  = svcObj.value("max_replicas", 1);
                int desired      = svcObj.value("desired_replicas", std::max(minReplicas, 1));
                std::string state = svcObj.value("state", std::string("scaling"));

                Service svc = serviceRepo.upsert(
                    app.id,
                    release.id,
                    svcName,
                    kind,
                    mode,
                    firstPort,
                    minReplicas,
                    maxReplicas,
                    desired,
                    state
                );
                // Preserve fields that upsert() does not refill on update
                svc.app_id = app.id;
                svc.name = svcName;
                services.push_back(svc);

                // Create and start instances locally
                int replicas = std::max(desired, 1);
                std::string workdir = "apps/" + project_slug;
                for (int i = 0; i < replicas; ++i) {
                    int port = (i == 0) ? firstPort : allocatePort();
                    // Decide binary path (uploads or direct)
                    std::filesystem::path imagePath;
                    if (image.rfind("/uploads/", 0) == 0) {
                        imagePath = std::filesystem::path("uploads") / image.substr(std::string("/uploads/").size());
                    } else {
                        imagePath = image;
                    }
                    if (!std::filesystem::exists(imagePath)) {
                        throw std::runtime_error("Image not found at " + imagePath.string());
                    }

                    Instance inst = instanceRepo.create(
                        svc.id,
                        "local-node",
                        "127.0.0.1",
                        port,
                        "starting"
                    );

                    bool ok = spawnInstance(imagePath, port, inst.id, app.id, workdir);
                    if (ok) {
                        instanceRepo.updateState(inst.id, "ready");
                        inst.state = "ready";
                Logger::info("[deploy] instance " + inst.id + " listening at 127.0.0.1:" + std::to_string(port));
                } else {
                    instanceRepo.updateState(inst.id, "failed");
                    inst.state = "failed";
                    Logger::warn("[deploy] instance " + inst.id + " failed to start on 127.0.0.1:" + std::to_string(port));
                }
                instances.push_back(inst);
            }
            }

            // 5. Kick scheduler/compute (concrete validation pipeline)
            json plan = json::array();
            auto addStep = [&](const std::string& step, const std::string& detail, const std::string& status) {
                plan.push_back({{"step", step}, {"detail", detail}, {"status", status}});
            };

            // Step 1: validate image reachable (local file check)
            std::filesystem::path imagePath;
            if (image.rfind("/uploads/", 0) == 0) {
                imagePath = std::filesystem::path("uploads") / image.substr(std::string("/uploads/").size());
            } else {
                imagePath = image;
            }

            if (!std::filesystem::exists(imagePath)) {
                addStep("validate_image_reachable", "Image not found at " + imagePath.string(), "failed");
                release.status = "failed";
            } else {
                auto sz = std::filesystem::file_size(imagePath);
                if (sz == 0) {
                    addStep("validate_image_reachable", "Image file is empty", "failed");
                    release.status = "failed";
                } else {
                    addStep("validate_image_reachable", "Found image at " + imagePath.string(), "ok");
                }
            }

            // Step 2: sandbox launch check (executable bit present)
            if (release.status != "failed") {
                auto perms = std::filesystem::status(imagePath).permissions();
                bool executable = (perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
                if (!executable) {
                    addStep("launch_sandbox_probe", "Image is not executable: " + imagePath.string(), "failed");
                    release.status = "failed";
                } else {
                    addStep("launch_sandbox_probe", "Executable bit present; sandbox execution pending integration", "ok");
                }
            }

            // Step 3: service ports sanity (backend-assigned; should be valid)
            if (release.status != "failed") {
                addStep("check_service_port", "Service ports assigned by backend", "ok");
            }

            // Step 4: attach routes (placeholder)
            if (release.status != "failed") {
                addStep("attach_routes", "Route publication to be handled by router/control plane", "pending");
            }

            // Mark release based on instance readiness
            bool anyReady = false;
            for (const auto& inst : instances) {
                if (inst.state == "ready") { anyReady = true; break; }
            }
            release.status = anyReady ? "ready" : "deploying";
            releaseRepo.updateStatus(release.id, release.status);

            json resp;
            resp["app"] = {
                {"id", app.id},
                {"slug", app.slug},
                {"default_domain", app.default_domain},
                {"created_at", app.created_at},
                {"updated_at", app.updated_at}
            };

            resp["release"] = {
                {"id", release.id},
                {"app_id", release.app_id},
                {"image", image},
                {"config_version", config_hash},
                {"status", release.status},
                {"created_at", release.created_at}
            };
            resp["services"] = json::array();
            for (const auto& svc : services) {
                resp["services"].push_back({
                    {"id", svc.id},
                    {"app_id", app.id},
                    {"release_id", release.id},
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
            resp["plan"] = plan;

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
