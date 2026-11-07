// hy: the Hydrogen project tool.
//
//   hy new <name> [--lib]   create a project in ./<name>
//   hy check                parse and resolve the whole project
//   hy build                compile to build/<name>
//   hy run [-- args...]     build, then run
//   hy deps                 list resolved dependencies
//
// hy finds the project by walking up from the current directory to hy.toml.

#include "hydrogen/Checker.h"
#include "hydrogen/Codegen.h"
#include "hydrogen/Lsp.h"
#include "hydrogen/Program.h"
#include "hydrogen/Project.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

#ifndef H2_STDLIB_DIR
#define H2_STDLIB_DIR ""
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kUsage = R"(hy: the Hydrogen project tool

usage:
  hy new <name> [--lib]   create a new project in ./<name>
  hy check                check the project for errors
  hy build [--release]    compile the project to build/<name>
  hy run [--release] [-- args...]   build and run the project
  hy deps                 show resolved dependencies
  hy lsp                  run the language server (for editors)
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
// If `llPath` is given, also writes the program's LLVM IR there.
int checkProject(const Loaded& p, const fs::path* llPath = nullptr, bool release = false) {
    std::error_code ec;
    if (!fs::is_regular_file(p.manifest.entry, ec)) {
        fail("entry file " + p.manifest.entry.generic_string() + " does not exist");
        return -1;
    }
    h2::Diagnostics diags;
    h2::LoadOptions opts{stdlibPaths(), p.packages, {}};
    auto program = h2::loadProgram(p.manifest.entry, opts, diags);
    h2::TypeContext types;
    h2::SemaInfo info;
    if (program->main()) {
        h2::resolveProgram(*program, diags);
        h2::typecheckProgram(*program, types, diags, nullptr, &info);
    }

    if (!p.manifest.isLibrary && program->main()) {
        auto it = program->main()->defs.find("main");
        if (it == program->main()->defs.end() || it->second->kind != h2::Symbol::Kind::Function)
            diags.error(program->main()->displayFile(), {1, 1},
                        "an app needs 'new function main() -> int' (or set type = \"lib\" in hy.toml)");
    }
    if (llPath && !diags.hasErrors()) {
        std::ofstream out(*llPath);
        if (!out) {
            fail("cannot write " + llPath->generic_string());
            return -1;
        }
        h2::emitLLVM(*program, types, info, diags, out, /*checks=*/!release);
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

// Runs a command, returning its exit status (or -1 if it couldn't start).
int runProcess(const std::vector<std::string>& argv) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

int cmdBuild(bool thenRun, const std::vector<std::string>& runArgs, bool release) {
    auto p = loadProject();
    if (!p) return 1;
    if (p->manifest.isLibrary)
        return fail(std::string("'") + p->manifest.name + "' is a library; there is nothing to " +
                    (thenRun ? "run" : "build") + ". Use 'hy check', or depend on it from an app");
    fs::path buildDir = p->manifest.dir / "build";
    std::error_code ec;
    fs::create_directories(buildDir, ec);
    fs::path ll = buildDir / (p->manifest.name + ".ll");
    fs::path exe = buildDir / p->manifest.name;
    if (checkProject(*p, &ll, release) < 0) return 1;

    const char* cc = std::getenv("H2_CC");
    std::string runtime = std::string(H2_STDLIB_DIR) + "/runtime/h2rt.c";
    std::vector<std::string> cmd = {cc && *cc ? cc : "clang", release ? "-O2" : "-O1", "-g", "-w",
                                    ll.string(), runtime, "-lm", "-lpthread", "-o", exe.string()};
    int rc = runProcess(cmd);
    if (rc == 127 || rc < 0)
        return fail("could not run '" + cmd[0] + "'. Hydrogen uses clang to turn LLVM IR into a program; install "
                    "clang, or set H2_CC to its path");
    if (rc != 0) return fail("clang failed to build " + ll.generic_string() + " (exit " + std::to_string(rc) + ")");
    if (!thenRun) {
        std::cout << "Built " << fs::relative(exe, fs::current_path(ec)).generic_string() << '\n';
        return 0;
    }
    std::vector<std::string> argv = {exe.string()};
    argv.insert(argv.end(), runArgs.begin(), runArgs.end());
    std::cout.flush();
    int status = runProcess(argv);
    if (status == 128 + 4 || status == 128 + 5) // SIGILL / SIGTRAP from a runtime check
        std::cerr << "\nerror: " << p->manifest.name
                  << " stopped at a runtime check: integer overflow or division by zero.\n"
#ifdef __APPLE__
                     "To see where, run it under a debugger: lldb -o run -o bt "
#else
                     "To see where, run it under a debugger: gdb -ex run -ex bt "
#endif
                  << fs::relative(exe, fs::current_path(ec)).generic_string()
                  << "\n(Release builds, hy run --release, skip these checks and wrap around instead.)\n";
    return status;
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
    if (cmd == "build" || cmd == "run") {
        bool release = false;
        std::vector<std::string> runArgs;
        for (size_t i = 0; i < rest.size(); ++i) {
            if (rest[i] == "--") {
                runArgs.assign(rest.begin() + i + 1, rest.end());
                break;
            }
            if (rest[i] == "--release") release = true;
            else return fail("unknown option '" + rest[i] + "' (did you mean: hy " + cmd + " -- " + rest[i] + "?)");
        }
        return cmdBuild(cmd == "run", runArgs, release);
    }
    if (cmd == "deps") return cmdDeps();
    if (cmd == "lsp") return h2::runLanguageServer(stdlibPaths());
    std::cerr << "error: unknown command '" << cmd << "'\n\n" << kUsage;
    return 2;
}
