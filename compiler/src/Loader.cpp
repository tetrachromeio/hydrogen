#include "hydrogen/Lexer.h"
#include "hydrogen/Parser.h"
#include "hydrogen/Program.h"

#include <fstream>
#include <map>
#include <sstream>

namespace h2 {
namespace fs = std::filesystem;

namespace {

std::vector<std::string> splitPath(const std::string& modPath) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : modPath) {
        if (c == '.') {
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(std::move(cur));
    return out;
}

// Shortest readable form of `p` for diagnostics: relative to the current
// directory when inside it, absolute otherwise.
std::string displayPath(const fs::path& p) {
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (!ec) {
        fs::path rel = p.lexically_relative(cwd);
        if (!rel.empty() && *rel.begin() != "..") return rel.generic_string();
    }
    return p.generic_string();
}

fs::path normalize(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(fs::absolute(p), ec);
    return ec ? fs::absolute(p).lexically_normal() : c;
}

class Loader {
public:
    Loader(Program& prog, Diagnostics& diags) : prog_(prog), diags_(diags) {}

    void run(const fs::path& mainFile, const LoadOptions& opts) {
        fs::path main = normalize(mainFile);
        prog_.searchPaths.push_back(main.parent_path());
        for (const auto& p : opts.libPaths) prog_.searchPaths.push_back(normalize(p));
        for (const auto& [name, dir] : opts.packages) prog_.packages[name] = normalize(dir);

        if (!load(main, inferModulePath(main))) {
            diags_.error(displayPath(main), {1, 1}, "cannot open file");
            return;
        }
        // prog_.modules grows while we iterate: each new module gets its uses loaded.
        for (size_t i = 0; i < prog_.modules.size(); ++i) {
            ModuleInfo& m = *prog_.modules[i];
            for (auto& f : m.files) loadImports(m, *f);
        }
    }

private:
    Program& prog_;
    Diagnostics& diags_;
    std::map<std::string, ModuleInfo*> byPath_;
    std::map<fs::path, ModuleInfo*> byFile_;
    bool reportedMissingPrelude_ = false;

    // A main file at <lib>/A/B/init.h2 *is* module A.B, so it behaves the same
    // whether it is compiled directly or reached through `use`.
    std::string inferModulePath(const fs::path& file) {
        if (file.filename() != "init.h2") return "";
        for (const auto& [name, root] : prog_.packages) {
            fs::path rel = file.parent_path().lexically_relative(root);
            if (rel.empty() || *rel.begin() == "..") continue;
            std::string path = name;
            if (rel != ".")
                for (const auto& seg : rel) path += "." + seg.string();
            return path;
        }
        for (size_t i = 1; i < prog_.searchPaths.size(); ++i) {
            fs::path rel = file.parent_path().lexically_relative(prog_.searchPaths[i]);
            if (rel.empty() || rel == "." || *rel.begin() == "..") continue;
            std::string path;
            for (const auto& seg : rel) {
                if (!path.empty()) path += '.';
                path += seg.string();
            }
            return path;
        }
        return "";
    }

    std::unique_ptr<SourceFile> parseFile(const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return nullptr;
        std::stringstream buf;
        buf << in.rdbuf();
        std::string src = buf.str();

        auto f = std::make_unique<SourceFile>();
        f->file = file;
        f->displayFile = displayPath(file);
        diags_.setFile(f->displayFile);
        f->ast = parse(lex(src, diags_), diags_);
        f->ast.fileName = f->displayFile;
        return f;
    }

    ModuleInfo* load(const fs::path& file, const std::string& modPath) {
        auto rootFile = parseFile(file);
        if (!rootFile) return nullptr;
        auto m = std::make_unique<ModuleInfo>();
        m->path = modPath;
        m->files.push_back(std::move(rootFile));
        m->self = prog_.newSymbol(Symbol::Kind::Module, m->displayName(), {1, 1}, m.get());

        ModuleInfo* raw = m.get();
        prog_.modules.push_back(std::move(m));
        byFile_[file] = raw;
        if (!modPath.empty()) byPath_[modPath] = raw;
        loadIncludes(*raw);
        return raw;
    }

    // The root file's `include x` lines add sibling files x.h2 to the module.
    void loadIncludes(ModuleInfo& m) {
        SourceFile& root = m.root();
        fs::path dir = root.file.parent_path();
        std::map<std::string, SourceLoc> seen;
        for (const auto& d : root.ast.decls) {
            if (d->kind != DeclKind::Include) continue;
            const auto& inc = static_cast<const IncludeDecl&>(*d);
            auto err = [&](const std::string& msg) { diags_.error(root.displayFile, inc.loc, msg); };
            if (auto [it, fresh] = seen.emplace(inc.name, inc.loc); !fresh) {
                err("'" + inc.name + "' is already included at line " + std::to_string(it->second.line));
                continue;
            }
            fs::path p = normalize(dir / (inc.name + ".h2"));
            if (p == root.file) {
                err("a file cannot include itself");
                continue;
            }
            if (inc.name == "init") {
                err("init.h2 is the root of a module; it cannot be included");
                continue;
            }
            auto f = parseFile(p);
            if (!f) {
                std::error_code ec;
                std::string hint = fs::is_regular_file(dir / inc.name / "init.h2", ec)
                                       ? ". '" + inc.name + "' is a folder with init.h2, so it is a submodule: use it with 'use'"
                                       : "";
                err("cannot include '" + inc.name + "': " + displayPath(p) + " not found" + hint);
                continue;
            }
            for (const auto& nested : f->ast.decls)
                if (nested->kind == DeclKind::Include)
                    diags_.error(f->displayFile, nested->loc,
                                 "only a module's root file (" + root.file.filename().string() +
                                     ") can include files; move this include there");
            byFile_[p] = &m;
            m.files.push_back(std::move(f));
        }
    }

    // Finds or loads module `modPath`. Returns null and fills `tried` if missing.
    ModuleInfo* get(const std::string& modPath, std::vector<fs::path>& tried) {
        if (auto it = byPath_.find(modPath); it != byPath_.end()) return it->second;
        auto segs = splitPath(modPath);
        std::vector<fs::path> candidates;
        if (auto pkg = prog_.packages.find(segs[0]); pkg != prog_.packages.end()) {
            // A package owns its whole namespace: no fallback to search paths.
            fs::path p = pkg->second;
            for (size_t i = 1; i < segs.size(); ++i) p /= segs[i];
            candidates.push_back(p / "init.h2");
        } else {
            for (const auto& dir : prog_.searchPaths) {
                fs::path p = dir;
                for (const auto& seg : segs) p /= seg;
                candidates.push_back(p / "init.h2");
            }
        }
        for (fs::path p : candidates) {
            tried.push_back(p);
            std::error_code ec;
            if (!fs::is_regular_file(p, ec)) continue;
            p = normalize(p);
            if (auto it = byFile_.find(p); it != byFile_.end()) {
                // Same file reached under a different search path.
                byPath_[modPath] = it->second;
                return it->second;
            }
            return load(p, modPath);
        }
        return nullptr;
    }

    void loadImports(ModuleInfo& m, SourceFile& file) {
        if (m.path != "Atomic") {
            std::vector<fs::path> tried;
            ModuleInfo* atomic = get("Atomic", tried);
            if (!atomic && !reportedMissingPrelude_) {
                reportedMissingPrelude_ = true;
                diags_.error(file.displayFile, {1, 1},
                             "the Atomic core library was not found; pass -L <dir> where <dir> contains "
                             "Atomic/init.h2, or set H2_PATH");
            }
            file.imports.push_back({nullptr, atomic});
        }
        for (const auto& d : file.ast.decls) {
            if (d->kind != DeclKind::Use) continue;
            const auto& use = static_cast<const UseDecl&>(*d);
            if (use.path == m.path) {
                diags_.error(file.displayFile, use.loc, "module '" + use.path + "' cannot use itself");
                file.imports.push_back({&use, nullptr});
                continue;
            }
            std::vector<fs::path> tried;
            ModuleInfo* target = get(use.path, tried);
            if (!target) {
                std::string msg = "module '" + use.path + "' not found. Looked for:";
                for (const auto& t : tried) msg += "\n    " + displayPath(t);
                diags_.error(file.displayFile, use.loc, msg);
            }
            file.imports.push_back({&use, target});
        }
    }
};

} // namespace

std::unique_ptr<Program> loadProgram(const fs::path& mainFile, const LoadOptions& options,
                                     Diagnostics& diags) {
    auto prog = std::make_unique<Program>();
    Loader(*prog, diags).run(mainFile, options);
    return prog;
}

} // namespace h2
