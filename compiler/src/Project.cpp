#include "hydrogen/Project.h"
#include "hydrogen/Toml.h"

#include <cctype>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace h2 {
namespace fs = std::filesystem;

namespace {

fs::path normalize(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(fs::absolute(p), ec);
    return ec ? fs::absolute(p).lexically_normal() : c;
}

std::string display(const fs::path& p) {
    std::error_code ec;
    fs::path rel = p.lexically_relative(fs::current_path(ec));
    if (!ec && !rel.empty() && *rel.begin() != "..") return rel.generic_string();
    return p.generic_string();
}

} // namespace

bool isValidModuleName(const std::string& name) {
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
    for (char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
}

std::optional<fs::path> findProjectRoot(const fs::path& start) {
    fs::path dir = normalize(start);
    for (;;) {
        std::error_code ec;
        if (fs::is_regular_file(dir / kManifestName, ec)) return dir;
        if (dir == dir.root_path() || dir.parent_path() == dir) return std::nullopt;
        dir = dir.parent_path();
    }
}

std::optional<Manifest> loadManifest(const fs::path& dirIn, std::vector<std::string>& errors) {
    Manifest m;
    m.dir = normalize(dirIn);
    m.file = m.dir / kManifestName;
    std::string where = display(m.file);
    size_t errorsBefore = errors.size();
    auto err = [&](int line, const std::string& msg) {
        errors.push_back(where + (line ? ":" + std::to_string(line) : "") + ": " + msg);
    };

    std::ifstream in(m.file);
    if (!in) {
        err(0, "cannot open (is this a Hydrogen project? create one with 'hy new <name>')");
        return std::nullopt;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    toml::ParseResult doc = toml::parse(buf.str());
    if (!doc.error.empty()) {
        err(doc.errorLine, doc.error);
        return std::nullopt;
    }

    for (const auto& [key, val] : doc.root)
        if (key != "project" && key != "dependencies")
            err(val.line, "unknown section '" + key + "' (expected [project] or [dependencies])");

    auto proj = doc.root.find("project");
    if (proj == doc.root.end() || !proj->second.asTable()) {
        err(0, "missing [project] section");
        return std::nullopt;
    }
    std::string type = "app";
    std::string entry;
    for (const auto& [key, val] : *proj->second.asTable()) {
        auto str = [&](std::string& out) {
            if (const auto* s = val.asString())
                out = *s;
            else
                err(val.line, "'" + key + "' must be a string, not " + val.typeName());
        };
        if (key == "name") str(m.name);
        else if (key == "version") str(m.version);
        else if (key == "type") str(type);
        else if (key == "entry") str(entry);
        else err(val.line, "unknown key '" + key + "' in [project] (expected name, version, type or entry)");
    }
    if (m.name.empty())
        err(proj->second.line, "[project] needs a name, e.g. name = \"hello\"");
    else if (!isValidModuleName(m.name))
        err(proj->second.line, "project name '" + m.name +
                                   "' must be an identifier (letters, digits, '_'), because it is also a module name");
    if (type != "app" && type != "lib") err(proj->second.line, "type must be \"app\" or \"lib\", not \"" + type + "\"");
    m.isLibrary = type == "lib";
    m.entry = normalize(m.dir / (entry.empty() ? (m.isLibrary ? "init.h2" : "main.h2") : entry));
    if (m.isLibrary && m.entry.filename() != "init.h2")
        err(proj->second.line, "a library's entry must be init.h2, so 'use " + m.name + "' can find it");

    if (auto deps = doc.root.find("dependencies"); deps != doc.root.end()) {
        if (!deps->second.asTable()) {
            err(deps->second.line, "[dependencies] must be a table");
        } else {
            for (const auto& [name, val] : *deps->second.asTable()) {
                if (!isValidModuleName(name)) {
                    err(val.line, "dependency name '" + name + "' must be an identifier: it is the module name you 'use'");
                    continue;
                }
                const toml::Table* t = val.asTable();
                if (!t) {
                    err(val.line, "dependency '" + name + "' must be a table, e.g. " + name +
                                      " = { path = \"../" + name + "\" }");
                    continue;
                }
                const toml::Value* path = nullptr;
                bool reported = false;
                for (const auto& [k, v] : *t) {
                    if (k == "path") {
                        path = &v;
                        continue;
                    }
                    reported = true;
                    if (k == "git" || k == "version")
                        err(v.line, "dependency '" + name + "': only path dependencies are supported so far");
                    else
                        err(v.line, "dependency '" + name + "': unknown key '" + k + "'");
                }
                if (reported) continue;
                if (!path || !path->asString()) {
                    err(val.line, "dependency '" + name + "' needs path = \"...\"");
                    continue;
                }
                m.dependencies.push_back({name, normalize(m.dir / *path->asString()), val.line});
            }
        }
    }
    if (errors.size() != errorsBefore) return std::nullopt;
    return m;
}

std::map<std::string, fs::path> resolveDependencies(const Manifest& root, std::vector<std::string>& errors) {
    std::map<std::string, fs::path> packages;
    std::set<fs::path> visited;
    if (root.isLibrary) packages[root.name] = root.dir;

    std::function<void(const Manifest&)> visit = [&](const Manifest& m) {
        if (!visited.insert(m.dir).second) return;
        for (const auto& dep : m.dependencies) {
            std::string where = display(m.file) + ":" + std::to_string(dep.line) + ": ";
            if (dep.name == "Atomic" || dep.name == "Elemental") {
                errors.push_back(where + "'" + dep.name + "' is part of the standard library; don't list it");
                continue;
            }
            if (auto it = packages.find(dep.name); it != packages.end()) {
                if (it->second != dep.path)
                    errors.push_back(where + "two different projects are both named '" + dep.name + "': " +
                                     display(it->second) + " and " + display(dep.path));
                continue;
            }
            std::vector<std::string> depErrors;
            auto dm = loadManifest(dep.path, depErrors);
            if (!dm) {
                errors.push_back(where + "cannot load dependency '" + dep.name + "' from " + display(dep.path));
                for (auto& e : depErrors) errors.push_back("    " + e);
                continue;
            }
            if (!dm->isLibrary) {
                errors.push_back(where + "dependency '" + dep.name + "' is an app; set type = \"lib\" in its hy.toml");
                continue;
            }
            if (dm->name != dep.name) {
                errors.push_back(where + "dependency '" + dep.name + "' points to a project named '" + dm->name +
                                 "'; the names must match");
                continue;
            }
            packages[dep.name] = dm->dir;
            visit(*dm);
        }
    };
    visit(root);
    return packages;
}

} // namespace h2
