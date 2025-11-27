// Aggregate route source so Aeon picks it up (it only compiles src/*.cpp)
#include "Aether/aether.h"
#include "core/scheduler/core_init.h"
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <boost/asio.hpp>
#include <sstream>
#include <csignal>
#include "Aether/Middleware/ServeStatic.h"
#include "core/utils/logger.h"

using namespace Aether;
using namespace Aether::Http;

// Shared context passed into API route registrations
struct ApiContext {
    CoreContext& core;
    sqlite3* db;
};

// Bring in route implementations (need ApiContext defined above)
#include "routes/status.cpp"
#include "routes/api_v1.cpp"

void registerRoutes(Server& app, ApiContext& ctx) {
    // Ensure uploads folder exists and serve it statically
    std::filesystem::create_directories("uploads");
    app.use(serveStatic("uploads"));
    app.use(serveStatic("ux"));

    struct InstanceTarget {
        std::string ip;
        int port;
        pid_t pid; // -1 for persistent instances
    };

    // Helper to resolve a ready instance (first service, first ready instance)
    auto resolveInstance = [&ctx](const std::string& slug) -> std::optional<InstanceTarget> {
        sqlite3_stmt* stmt = nullptr;
        std::string appId;
        std::string currentReleaseId;

        const char* appSql =
            "SELECT id, current_release_id FROM apps WHERE slug = ?1 LIMIT 1;";
        if (sqlite3_prepare_v2(ctx.db, appSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, slug.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                appId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
                    currentReleaseId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                }
            }
        }
        sqlite3_finalize(stmt);
        if (appId.empty()) return std::nullopt;
        if (currentReleaseId.empty()) return std::nullopt;

        // Pick any ready instance for this app/current release
        std::string ip;
        int port = 0;
        const char* instSql =
            "SELECT ip, port FROM instances "
            "WHERE service_id IN (SELECT id FROM services WHERE app_id = ?1 AND release_id = ?2) "
            "AND state = 'ready' "
            "ORDER BY started_at DESC LIMIT 1;";
        if (sqlite3_prepare_v2(ctx.db, instSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, appId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, currentReleaseId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                port = sqlite3_column_int(stmt, 1);
            }
        }
        sqlite3_finalize(stmt);

        if (ip.empty() || port == 0) {
            Logger::warn("[interface] no ready instance for slug=" + slug + " release=" + currentReleaseId);
            return std::nullopt;
        }

        // Verify the instance is reachable before redirecting
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket sock(io);
        boost::system::error_code ec;
        auto addr = boost::asio::ip::make_address(ip, ec);
        if (ec) {
            Logger::warn("[interface] bad address for instance " + ip + ":" + std::to_string(port) + " err=" + ec.message());
            return std::nullopt;
        }
        sock.connect({addr, static_cast<unsigned short>(port)}, ec);
        if (ec) {
            Logger::warn("[interface] instance unreachable at " + ip + ":" + std::to_string(port) + " err=" + ec.message());
            return std::nullopt;
        }

        return InstanceTarget{ip, port, -1};
    };

    auto allocatePort = []() -> int {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor acc(io);
        acc.open(boost::asio::ip::tcp::v4());
        acc.bind({boost::asio::ip::tcp::v4(), 0});
        int port = acc.local_endpoint().port();
        acc.close();
        return port;
    };

    auto tryConnectTcp = [](const std::string& ip, int port) -> bool {
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket sock(io);
        boost::system::error_code ec;
        auto addr = boost::asio::ip::make_address(ip, ec);
        if (ec) return false;
        sock.connect({addr, static_cast<unsigned short>(port)}, ec);
        return !ec;
    };

    auto spawnTemp = [](const std::filesystem::path& binPath, int port, const std::string& tag, const std::string& appId, const std::string& workdir) -> pid_t {
        std::filesystem::path absPath = std::filesystem::weakly_canonical(binPath);
        std::filesystem::create_directories("logs/apps");
        std::string logPath = std::filesystem::weakly_canonical("logs/apps").string() + "/" + appId + ".log";
        pid_t pid = ::fork();
        if (pid < 0) {
            return -1;
        }
        if (pid == 0) {
            std::string portStr = std::to_string(port);
            std::string execPath = absPath.string();
            std::filesystem::create_directories(workdir);
            std::ostringstream cmd;
            cmd << "cd \"" << workdir << "\" && PORT=" << portStr << " \"" << execPath << "\" 2>&1 | "
                << "sed -u 's/^/[ " << tag << " ] /' >> \"" << logPath << "\"";
            ::execl("/bin/sh", "sh", "-c", cmd.str().c_str(), (char*)nullptr);
            _exit(127);
        }
        return pid;
    };

    auto coldStartInstance = [&](const std::string& slug) -> std::optional<InstanceTarget> {
        sqlite3_stmt* stmt = nullptr;
        std::string appId;
        std::string currentReleaseId;

        const char* appSql =
            "SELECT id, current_release_id FROM apps WHERE slug = ?1 LIMIT 1;";
        if (sqlite3_prepare_v2(ctx.db, appSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, slug.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                appId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
                    currentReleaseId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                }
            }
        }
        sqlite3_finalize(stmt);
        if (appId.empty() || currentReleaseId.empty()) return std::nullopt;

        // Find a cold_start service and its release image
        std::string image;
        std::string serviceId;
        const char* svcSql =
            "SELECT services.id, releases.image FROM services "
            "JOIN releases ON releases.id = services.release_id "
            "WHERE services.app_id = ?1 AND services.release_id = ?2 AND services.mode = 'cold_start' "
            "LIMIT 1;";
        if (sqlite3_prepare_v2(ctx.db, svcSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, appId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, currentReleaseId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                serviceId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                image = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            }
        }
        sqlite3_finalize(stmt);

        if (image.empty()) return std::nullopt;

        std::filesystem::path binPath;
        if (image.rfind("/uploads/", 0) == 0) {
            binPath = std::filesystem::path("uploads") / image.substr(std::string("/uploads/").size());
        } else {
            binPath = image;
        }
        if (!std::filesystem::exists(binPath)) {
            Logger::warn("[interface] cold-start binary missing for slug=" + slug + " at " + binPath.string());
            return std::nullopt;
        }

        int port = allocatePort();
        std::string tag = "cold_" + slug;
        std::string workdir = "apps/" + slug;
        pid_t pid = spawnTemp(binPath, port, tag, appId, workdir);
        if (pid < 0) {
            Logger::warn("[interface] cold-start fork failed for slug=" + slug);
            return std::nullopt;
        }

        // Wait for readiness
        bool ready = false;
        for (int i = 0; i < 10; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (tryConnectTcp("127.0.0.1", port)) { ready = true; break; }
        }

        if (!ready) {
            ::kill(pid, SIGKILL);
            Logger::warn("[interface] cold-start failed to become ready for slug=" + slug);
            return std::nullopt;
        }

        return InstanceTarget{"127.0.0.1", port, pid};
    };

    auto proxyFetch = [](const std::string& ip, int port, const std::string& path) -> std::tuple<int, std::string, std::string> {
        try {
            boost::asio::io_context io;
            boost::asio::ip::tcp::socket sock(io);
            boost::system::error_code ec;
            auto addr = boost::asio::ip::make_address(ip, ec);
            if (ec) return {502, "bad address", "text/plain"};
            sock.connect({addr, static_cast<unsigned short>(port)}, ec);
            if (ec) return {502, "connect failed", "text/plain"};

            std::ostringstream req;
            req << "GET " << path << " HTTP/1.0\r\n";
            req << "Host: " << ip << "\r\n";
            req << "Connection: close\r\n\r\n";
            auto reqStr = req.str();
            boost::asio::write(sock, boost::asio::buffer(reqStr), ec);
            if (ec) return {502, "write failed", "text/plain"};

            std::string raw;
            for (;;) {
                char buf[4096];
                size_t n = sock.read_some(boost::asio::buffer(buf), ec);
                if (n > 0) raw.append(buf, n);
                if (ec == boost::asio::error::eof) break;
                if (ec) break;
            }
            sock.close();

            std::istringstream ss(raw);
            std::string statusLine;
            if (!std::getline(ss, statusLine)) return {502, "bad response", "text/plain"};
            if (!statusLine.empty() && statusLine.back() == '\r') statusLine.pop_back();
            int statusCode = 200;
            {
                std::istringstream sl(statusLine);
                std::string httpVer;
                sl >> httpVer >> statusCode;
            }

            std::string line;
            std::string contentType = "text/html";
            while (std::getline(ss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) break;
                auto pos = line.find(':');
                if (pos != std::string::npos) {
                    std::string key = line.substr(0, pos);
                    std::string val = line.substr(pos + 1);
                    while (!val.empty() && val.front() == ' ') val.erase(val.begin());
                    if (key == "Content-Type" || key == "content-type") contentType = val;
                }
            }

            std::string body((std::istreambuf_iterator<char>(ss)), std::istreambuf_iterator<char>());
            return {statusCode, body, contentType};
        } catch (...) {
            return {502, "proxy error", "text/plain"};
        }
    };

    // Proxy root interface to instance root
    app.get("/interface/:slug", [&ctx, resolveInstance, proxyFetch, coldStartInstance](Request& req, Response& res) {

        auto getParam = [&](const std::string& key) -> std::string {
            auto it = req.params.find(key);
            if (it != req.params.end()) return it->second;
            return "";
        };
        std::string slug = getParam(":slug");
        if (slug.empty()) slug = getParam("slug");
        if (slug.empty()) {
            res.setHeader("Content-Type", "text/plain");
            res.send("slug missing", 400);
            Logger::logRequest(req.method, req.path, "unknown", 400);
            return;
        }

        auto inst = resolveInstance(slug);
        if (!inst) {
            Logger::warn("[interface] no instance for slug=" + slug + " (root), trying cold start");
            auto cold = coldStartInstance(slug);
            if (!cold) {
                res.setHeader("Content-Type", "text/plain");
                res.send("no running instance", 502);
                Logger::logRequest(req.method, req.path, "unknown", 502);
                return;
            }
            inst = cold;
        }
        auto [statusCode, body, contentType] = proxyFetch(inst->ip, inst->port, "/");
        res.setHeader("Content-Type", contentType);
        res.send(body, statusCode);
        Logger::logRequest(req.method, req.path, "unknown", statusCode);
        if (inst->pid > 0) {
            ::kill(inst->pid, SIGTERM);
        }
    });

    // Proxy/redirect interface subroutes to the app instance (first ready instance)
    app.get("/interface/:slug/:page", [&ctx, resolveInstance, proxyFetch, coldStartInstance](Request& req, Response& res) {
        auto getParam = [&](const std::string& key) -> std::string {
            auto it = req.params.find(key);
            if (it != req.params.end()) return it->second;
            return "";
        };
        std::string slug = getParam(":slug");
        if (slug.empty()) slug = getParam("slug");
        if (slug.empty()) {
            res.setHeader("Content-Type", "text/plain");
            res.send("slug missing", 400);
            Logger::logRequest(req.method, req.path, "unknown", 400);
            return;
        }

        // Derive remainder path after "/interface/<slug>/"
        std::string remainder = req.path;
        std::string prefix = "/interface/" + slug + "/";
        if (remainder.rfind(prefix, 0) == 0) {
            remainder = remainder.substr(prefix.size());
        } else {
            remainder = "";
        }
        if (remainder.empty()) remainder = "";

        auto inst = resolveInstance(slug);
        if (!inst) {
            Logger::warn("[interface] no instance for slug=" + slug + " path=" + remainder + ", trying cold start");
            auto cold = coldStartInstance(slug);
            if (!cold) {
                res.setHeader("Content-Type", "text/plain");
                res.send("no running instance", 502);
                Logger::logRequest(req.method, req.path, "unknown", 502);
                return;
            }
            inst = cold;
        }

        std::string path = "/";
        if (!remainder.empty()) path += remainder;
        auto [statusCode, body, contentType] = proxyFetch(inst->ip, inst->port, path);
        res.setHeader("Content-Type", contentType);
        res.send(body, statusCode);
        Logger::logRequest(req.method, req.path, "unknown", statusCode);
        if (inst->pid > 0) {
            ::kill(inst->pid, SIGTERM);
        }
    });

    registerHealthRoute(app);
    registerStatusRoute(app);
    registerAPIv1Routes(app, ctx);
}
