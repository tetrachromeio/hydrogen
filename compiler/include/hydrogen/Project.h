#pragma once
// hy.toml: the project manifest.
//
//   [project]
//   name = "hello"          # required; for a library this is its module name
//   version = "0.1.0"
//   type = "app"            # "app" (default) or "lib"
//   entry = "main.h2"       # default: main.h2 for apps, init.h2 for libraries
//
//   [dependencies]
//   Geometry = { path = "../geometry" }    # a local library project

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace h2 {

struct Dependency {
    std::string name;
    std::filesystem::path path; // absolute, normalized
    int line = 0;
};

struct Manifest {
    std::filesystem::path dir;  // directory containing hy.toml
    std::filesystem::path file; // the hy.toml itself
    std::string name;
    std::string version = "0.1.0";
    bool isLibrary = false;
    std::filesystem::path entry; // absolute
    std::vector<Dependency> dependencies;
};

inline constexpr const char* kManifestName = "hy.toml";

// Reads and validates <dir>/hy.toml. On failure, returns nullopt and appends
// "file:line: message" strings to `errors`.
std::optional<Manifest> loadManifest(const std::filesystem::path& dir, std::vector<std::string>& errors);

// Walks up from `start` to the nearest directory containing hy.toml.
std::optional<std::filesystem::path> findProjectRoot(const std::filesystem::path& start);

// Loads all dependencies transitively. Returns package name -> root dir.
// A library project also maps its own name to its root.
std::map<std::string, std::filesystem::path> resolveDependencies(const Manifest& root,
                                                                 std::vector<std::string>& errors);

bool isValidModuleName(const std::string& name);

} // namespace h2
