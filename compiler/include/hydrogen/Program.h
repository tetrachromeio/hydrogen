#pragma once
// A Program is the set of modules reachable from a main file, plus the
// symbols the resolver creates for them.
//
// Module layout: `use A.B` loads `<dir>/A/B/init.h2`, where <dir> is the
// main file's directory, then each library search path in order.
// A *package* (a hy.toml dependency) named P maps `use P` to <P root>/init.h2
// and `use P.X` to <P root>/X/init.h2.
// Every module except `Atomic` itself implicitly does `use Atomic` (the prelude).

#include "hydrogen/AST.h"
#include "hydrogen/Diagnostics.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace h2 {

struct ModuleInfo;

struct Symbol {
    enum class Kind {
        Module,    // a module reached through `use`
        Function,
        Type,      // new type
        Class,
        Enum,
        Variant,   // enum variant
        Interface,
        Generic,   // generic parameter
        Local,     // let/var, parameter, for variable, match binding
        Builtin,   // ptr, Self, intrinsic
    };
    Kind kind;
    std::string name;
    SourceLoc loc;
    bool isPub = false;
    ModuleInfo* module = nullptr;     // defining module (target module for Kind::Module)
    const Decl* decl = nullptr;       // Function/Type/Class/Enum/Interface; parent enum for Variant
    const Variant* variant = nullptr; // Variant
    bool isMutable = false;           // Local declared with var
    const struct SourceFile* file = nullptr; // file that declares it (top-level symbols)
    std::map<std::string, Symbol*> members; // Enum: its variants
};

std::string_view symbolKindName(Symbol::Kind k);

struct Import {
    const UseDecl* decl = nullptr; // null for the implicit prelude
    ModuleInfo* target = nullptr;  // null if the module could not be found
};

// One .h2 file. A module is its root file (init.h2, or an app's main file)
// plus the files the root `include`s. Each file has its own `use` imports.
struct SourceFile {
    std::filesystem::path file;
    std::string displayFile; // path used in diagnostics
    Module ast;
    std::vector<Import> imports;
};

struct ModuleInfo {
    std::string path; // "Atomic.Memory"; the main file's inferred path, or "" for a program
    std::vector<std::unique_ptr<SourceFile>> files; // files[0] is the root file
    Symbol* self = nullptr; // Kind::Module symbol for this module

    SourceFile& root() const { return *files.front(); }
    const std::string& displayFile() const { return files.front()->displayFile; }

    // Filled in by the resolver.
    std::unordered_map<std::string, Symbol*> defs;                 // top-level definitions
    std::unordered_map<std::string, std::vector<Symbol*>> variants; // variants of enums defined here

    bool isAtomic() const { return path == "Atomic" || path.rfind("Atomic.", 0) == 0; }
    std::string displayName() const { return path.empty() ? "main" : path; }
};

struct Program {
    std::vector<std::unique_ptr<ModuleInfo>> modules; // modules[0] is the main file
    std::vector<std::filesystem::path> searchPaths;   // main dir first, then libraries
    std::map<std::string, std::filesystem::path> packages; // package name -> root dir
    std::vector<std::unique_ptr<Symbol>> symbols;     // owns every Symbol

    ModuleInfo* main() const { return modules.empty() ? nullptr : modules.front().get(); }

    Symbol* newSymbol(Symbol::Kind kind, std::string name, SourceLoc loc, ModuleInfo* module) {
        auto s = std::make_unique<Symbol>();
        s->kind = kind;
        s->name = std::move(name);
        s->loc = loc;
        s->module = module;
        symbols.push_back(std::move(s));
        return symbols.back().get();
    }
};

struct LoadOptions {
    std::vector<std::filesystem::path> libPaths;              // searched after the main file's dir
    std::map<std::string, std::filesystem::path> packages;    // package name -> root dir
};

// Parses the main file and every module it (transitively) uses.
std::unique_ptr<Program> loadProgram(const std::filesystem::path& mainFile, const LoadOptions& options,
                                     Diagnostics& diags);

// Binds every name in the program to a Symbol and enforces `pub`.
void resolveProgram(Program& program, Diagnostics& diags);

} // namespace h2
