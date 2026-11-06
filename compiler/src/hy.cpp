// hy: the Hydrogen project tool.
//
//   hy new <name> [--lib]   create a project in ./<name>
//   hy check                parse and resolve the whole project
//   hy build                compile to build/<name>
//   hy run [-- args...]     build, then run
//   hy deps                 list resolved dependencies
//
// hy finds the project by walking up from the current directory to hy.toml.

#include "hydrogen/Program.h"
#include "hydrogen/Project.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#ifndef H2_STDLIB_DIR
#define H2_STDLIB_DIR ""
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kUsage = R"(hy: the Hydrogen project tool

usage:
  hy new <name> [--lib]   create a new project in ./<name>
  hy check                check the project for errors
  hy build                compile the project to build/<name>
  hy run [-- args...]     build and run the project
  hy deps                 show resolved dependencies
  hy help                 show this message

The project is configured by hy.toml in its root directory.
)";

int fail(const std::string& msg) {
    std::cerr << "error: " << msg << '\n';
    return 1;
}

std::vector<fs::path> stdlibPaths() {
    std::vector<fs::path> libs;
    if (const char* env = std::getenv("H2_PATH")) {
        std::stringstream ss(env);
        std::string dir;
        while (std::getline(ss, dir, ':'))
            if (!dir.empty()) libs.emplace_back(dir);
    }
    if (*H2_STDLIB_DIR) libs.emplace_back(H2_STDLIB_DIR);
    return libs;
}

bool writeFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p);
    out << text;
    return static_cast<bool>(out);
}

int cmdNew(const std::vector<std::string>& args) {
    std::string name;
    bool lib = false;
    for (const auto& a : args) {
        if (a == "--lib") lib = true;
        else if (!a.empty() && a[0] == '-') return fail("unknown option '" + a + "'");
        else if (name.empty()) name = a;
        else return fail("hy new takes one name");
    }
    if (name.empty()) return fail("usage: hy new <name> [--lib]");
    if (!h2::isValidModuleName(name))
        return fail("'" + name + "' is not a valid project name: use letters, digits and '_', starting with a letter");
    fs::path dir = name;
    std::error_code ec;
    if (fs::exists(dir, ec)) return fail("'" + name + "' already exists");
    fs::create_directories(dir, ec);
    if (ec) return fail("cannot create '" + name + "': " + ec.message());

    std::string manifest = "[project]\nname = \"" + name + "\"\nversion = \"0.1.0\"\n";
    if (lib) manifest += "type = \"lib\"\n";
    manifest += "\n[dependencies]\n# Mylib = { path = \"../mylib\" }\n";

    bool ok = writeFile(dir / h2::kManifestName, manifest) && writeFile(dir / ".gitignore", "build/\n");
    if (lib) {
        ok = ok && writeFile(dir / "init.h2",
                             "// The " + name + " library. Everything marked pub is available after `use " +
                                 name + "`.\n\n"
                                 "pub new function greeting() -> string {\n"
                                 "    return \"Hello from " + name + "!\"\n"
                                 "}\n");
    } else {
        ok = ok && writeFile(dir / "main.h2",
                             "use Elemental\n\n"
                             "new function main() -> int {\n"
                             "    print(\"Hello from " + name + "!\")\n"
                             "    return 0\n"
                             "}\n");
    }
    if (!ok) return fail("could not write project files in '" + name + "'");
    std::cout << "Created " << (lib ? "library" : "app") << " '" << name << "'\n\n"
              << "  cd " << name << "\n  hy " << (lib ? "check" : "run") << "\n";
    return 0;
}

struct Loaded {
    h2::Manifest manifest;
    std::map<std::string, fs::path> packages;
};

std::optional<Loaded> loadProject() {
    std::error_code ec;
    auto root = h2::findProjectRoot(fs::current_path(ec));
    if (!root) {
        fail(std::string("no ") + h2::kManifestName +
             " found in this directory or any parent. Create a project with 'hy new <name>'");
        return std::nullopt;
    }
    std::vector<std::string> errors;
    auto manifest = h2::loadManifest(*root, errors);
    std::map<std::string, fs::path> packages;
    if (manifest) packages = h2::resolveDependencies(*manifest, errors);
    for (const auto& e : errors) std::cerr << "error: " << e << '\n';
    if (!errors.empty()) return std::nullopt;
    return Loaded{std::move(*manifest), std::move(packages)};
}

// Runs the front end over the project. Returns the number of modules, or -1 on errors.
int checkProject(const Loaded& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p.manifest.entry, ec)) {
        fail("entry file " + p.manifest.entry.generic_string() + " does not exist");
        return -1;
    }
    h2::Diagnostics diags;
    h2::LoadOptions opts{stdlibPaths(), p.packages};
    auto program = h2::loadProgram(p.manifest.entry, opts, diags);
    if (program->main()) h2::resolveProgram(*program, diags);

    if (!p.manifest.isLibrary && program->main()) {
        auto it = program->main()->defs.find("main");
        if (it == program->main()->defs.end() || it->second->kind != h2::Symbol::Kind::Function)
            diags.error(program->main()->displayFile, {1, 1},
                        "an app needs 'new function main() -> int' (or set type = \"lib\" in hy.toml)");
    }
    diags.print(std::cerr);
    if (diags.hasErrors()) {
        size_t n = diags.errors().size();
        std::cerr << "\n" << p.manifest.name << ": " << n << (n == 1 ? " error\n" : " errors\n");
        return -1;
    }
    return static_cast<int>(program->modules.size());
}

int cmdCheck() {
    auto p = loadProject();
    if (!p) return 1;
    int n = checkProject(*p);
    if (n < 0) return 1;
    std::cout << "Checked " << p->manifest.name << " v" << p->manifest.version << " (" << n
              << (n == 1 ? " module" : " modules") << "), no errors\n";
    return 0;
}

int cmdBuild(bool thenRun) {
    auto p = loadProject();
    if (!p) return 1;
    int n = checkProject(*p);
    if (n < 0) return 1;
    std::cout << "Checked " << p->manifest.name << " (" << n << " modules)\n";
    if (p->manifest.isLibrary)
        return fail(std::string("'") + p->manifest.name + "' is a library; there is nothing to " +
                    (thenRun ? "run" : "build") + ". Use 'hy check', or depend on it from an app");
    return fail("cannot " + std::string(thenRun ? "run" : "build") +
                " yet: native code generation (the LLVM backend) is not implemented. "
                "The program checked without errors");
}

int cmdDeps() {
    auto p = loadProject();
    if (!p) return 1;
    std::cout << p->manifest.name << " v" << p->manifest.version << (p->manifest.isLibrary ? " (lib)" : " (app)")
              << '\n';
    if (p->packages.empty() || (p->packages.size() == 1 && p->manifest.isLibrary)) {
        std::cout << "  (no dependencies)\n";
        return 0;
    }
    for (const auto& [name, dir] : p->packages) {
        if (name == p->manifest.name && p->manifest.isLibrary) continue;
        std::cout << "  " << name << "  " << dir.generic_string() << '\n';
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        std::cout << kUsage;
        return args.empty() ? 2 : 0;
    }
    std::string cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());
    if (cmd == "new") return cmdNew(rest);
    if (cmd == "check") return cmdCheck();
    if (cmd == "build") return cmdBuild(false);
    if (cmd == "run") return cmdBuild(true);
    if (cmd == "deps") return cmdDeps();
    std::cerr << "error: unknown command '" << cmd << "'\n\n" << kUsage;
    return 2;
}
