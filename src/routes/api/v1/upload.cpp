#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <random>
#include <chrono>
#include <cstdlib>

#include "Aether/aether.h"

using namespace Aether::Http;
using json = nlohmann::json;

static std::string sanitizeFilename(const std::string& name) {
    if (name.empty()) return {};
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return {};
    if (name == "." || name == "..") return {};
    return name;
}

static std::string sanitizeRelativePath(const std::string& path) {
    if (path.empty()) return {};
    if (path[0] == '/' || path[0] == '\\') return {};
    if (path.find("..") != std::string::npos) return {};
    return path;
}

static std::string sanitizeFolder(const std::string& name) {
    if (name.empty()) return {};
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return {};
    if (name == "." || name == "..") return {};
    return name;
}

static std::string randomSuffix() {
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::mt19937_64 rng(static_cast<uint64_t>(now));
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << dist(rng);
    return oss.str().substr(0, 8);
}

static std::vector<unsigned char> base64Decode(const std::string& input) {
    static constexpr unsigned char kDecTable[] = {
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
        52,53,54,55,56,57,58,59,60,61,64,64,64,65,64,64,
        64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
        64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64
    };

    std::vector<unsigned char> out;
    out.reserve((input.size() * 3) / 4);

    unsigned int buffer = 0;
    int bitsCollected = 0;
    for (unsigned char c : input) {
        if (c == '=') {
            break;
        }
        if (c > 127) throw std::runtime_error("Invalid base64 character");
        unsigned char dec = kDecTable[c];
        if (dec == 64) throw std::runtime_error("Invalid base64 character");
        if (dec == 65) continue; // ignore padding markers
        buffer = (buffer << 6) | dec;
        bitsCollected += 6;
        if (bitsCollected >= 8) {
            bitsCollected -= 8;
            out.push_back(static_cast<unsigned char>((buffer >> bitsCollected) & 0xFF));
        }
    }
    return out;
}

void registerAPIv1UploadRoute(Server& app) {
    app.post("/api/v1/upload", [](Request& req, Response& res) {
        try {
            json body = json::parse(req.getBody());
            std::string filename = sanitizeFilename(body.value("filename", ""));
            std::string dataB64  = body.value("data", "");
            std::string deployment = sanitizeFolder(body.value("deployment", ""));
            std::string entrypoint = sanitizeRelativePath(body.value("entrypoint", filename));

            if (filename.empty()) throw std::runtime_error("filename is required");
            if (dataB64.empty()) throw std::runtime_error("data is required (base64)");
            if (deployment.empty()) {
                deployment = "bundle_" + randomSuffix();
            }
            if (entrypoint.empty()) throw std::runtime_error("entrypoint is required");

            std::vector<unsigned char> decoded = base64Decode(dataB64);

            std::filesystem::path outDir = std::filesystem::path("uploads") / deployment;
            std::filesystem::create_directories(outDir);
            std::filesystem::path outPath = outDir / entrypoint;
            std::filesystem::create_directories(outPath.parent_path());

            std::ofstream ofs(outPath, std::ios::binary);
            if (!ofs) throw std::runtime_error("Failed to open output file");
            ofs.write(reinterpret_cast<const char*>(decoded.data()), static_cast<std::streamsize>(decoded.size()));
            ofs.close();

            bool isZip = outPath.extension() == ".zip";
            if (isZip) {
                std::string cmd = "unzip -o \"" + outPath.string() + "\" -d \"" + outDir.string() + "\"";
                int rc = std::system(cmd.c_str());
                if (rc != 0) {
                    throw std::runtime_error("unzip failed");
                }
            }

            std::filesystem::path entryPath = outDir / entrypoint;
            if (!std::filesystem::exists(entryPath) || !std::filesystem::is_regular_file(entryPath)) {
                throw std::runtime_error("entrypoint not found or not a file: " + entryPath.string());
            }
            std::filesystem::permissions(
                entryPath,
                std::filesystem::perms::owner_all |
                std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
                std::filesystem::perm_options::replace
            );
            // Remove zip after extraction
            if (isZip) {
                std::filesystem::remove(outPath);
            }

            json resp;
            resp["url"] = "/uploads/" + deployment + "/" + entrypoint;
            resp["filename"] = entrypoint;
            resp["size"] = decoded.size();
            resp["executable"] = true;
            resp["deployment"] = deployment;
            resp["entrypoint"] = entrypoint;

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
