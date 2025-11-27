#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdlib>

#include "Aether/aether.h"

using namespace Aether::Http;
using json = nlohmann::json;

static std::string sanitizeFolderAsset(const std::string& name) {
    if (name.empty()) return {};
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return {};
    if (name == "." || name == "..") return {};
    return name;
}

static std::vector<unsigned char> base64DecodeAssets(const std::string& input) {
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
        if (c == '=') break;
        if (c > 127) throw std::runtime_error("Invalid base64 character");
        unsigned char dec = kDecTable[c];
        if (dec == 64) throw std::runtime_error("Invalid base64 character");
        if (dec == 65) continue;
        buffer = (buffer << 6) | dec;
        bitsCollected += 6;
        if (bitsCollected >= 8) {
            bitsCollected -= 8;
            out.push_back(static_cast<unsigned char>((buffer >> bitsCollected) & 0xFF));
        }
    }
    return out;
}

void registerAPIv1UploadAssetsRoute(Server& app) {
    app.post("/api/v1/upload_assets", [](Request& req, Response& res) {
        try {
            json body = json::parse(req.getBody());
            std::string slug = sanitizeFolderAsset(body.value("slug", ""));
            std::string filename = body.value("filename", "");
            std::string dataB64 = body.value("data", "");

            if (slug.empty()) throw std::runtime_error("slug is required");
            if (filename.empty()) throw std::runtime_error("filename is required");
            if (dataB64.empty()) throw std::runtime_error("data is required (base64)");

            std::vector<unsigned char> decoded = base64DecodeAssets(dataB64);
            std::filesystem::path appDir = std::filesystem::path("apps") / slug;
            std::filesystem::create_directories(appDir);
            std::filesystem::path zipPath = appDir / filename;

            std::ofstream ofs(zipPath, std::ios::binary);
            if (!ofs) throw std::runtime_error("Failed to open assets file");
            ofs.write(reinterpret_cast<const char*>(decoded.data()), static_cast<std::streamsize>(decoded.size()));
            ofs.close();

            if (zipPath.extension() != ".zip") {
                throw std::runtime_error("assets upload must be a zip");
            }

            // If leftover file blocks a folder, remove it
            const std::filesystem::path publicPath = appDir / "public";
            if (std::filesystem::exists(publicPath) && !std::filesystem::is_directory(publicPath)) {
                std::filesystem::remove(publicPath);
            }

            std::string cmd = "unzip -o \"" + zipPath.string() + "\" -d \"" + appDir.string() + "\"";
            int rc = std::system(cmd.c_str());
            if (rc != 0) throw std::runtime_error("unzip failed");

            std::filesystem::remove(zipPath);

            json resp;
            resp["app_dir"] = appDir.string();
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
