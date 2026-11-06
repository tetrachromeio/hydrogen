// h2c: the Hydrogen compiler driver.
//
//   h2c --tokens file.h2   dump the token stream (single file)
//   h2c --ast file.h2      dump the parsed AST (single file)
//   h2c --check file.h2    load every used module, resolve names, report errors
//
// Options:
//   -L <dir>   add a library search path (searched after the main file's directory)
//
// Library search order: the main file's directory, each -L in order, each
// entry of $H2_PATH (':'-separated), then the bundled standard library.
//
// Exit code: 0 on success, 1 on compile errors, 2 on usage/IO errors.

#include "hydrogen/AstPrinter.h"
#include "hydrogen/Lexer.h"
#include "hydrogen/Parser.h"
#include "hydrogen/Program.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#ifndef H2_STDLIB_DIR
#define H2_STDLIB_DIR ""
#endif

namespace {

int usage() {
    std::cerr << "usage: h2c (--tokens | --ast | --check) [-L <dir>]... <file.h2>\n";
    return 2;
}

void dumpTokens(const std::vector<h2::Token>& toks) {
    for (const auto& t : toks) {
        std::cout << t.loc.line << ':' << t.loc.col << ' ' << h2::tokName(t.kind);
        if (t.kind == h2::Tok::Ident || t.kind == h2::Tok::IntLit || t.kind == h2::Tok::FloatLit ||
            t.kind == h2::Tok::StringLit || t.kind == h2::Tok::CharLit)
            std::cout << " `" << t.text << '`';
        if (t.newlineBefore) std::cout << " [nl]";
        if (t.joint) std::cout << " [joint]";
        std::cout << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string mode, file;
    std::vector<std::filesystem::path> libs;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--tokens" || a == "--ast" || a == "--check") {
            if (!mode.empty()) return usage();
            mode = a;
        } else if (a == "-L") {
            if (++i >= argc) return usage();
            libs.emplace_back(argv[i]);
        } else if (!a.empty() && a[0] == '-') {
            return usage();
        } else {
            if (!file.empty()) return usage();
            file = a;
        }
    }
    if (mode.empty() || file.empty()) return usage();

    h2::Diagnostics diags(file);
    if (mode == "--check") {
        if (const char* env = std::getenv("H2_PATH")) {
            std::stringstream ss(env);
            std::string dir;
            while (std::getline(ss, dir, ':'))
                if (!dir.empty()) libs.emplace_back(dir);
        }
        if (*H2_STDLIB_DIR) libs.emplace_back(H2_STDLIB_DIR);
        auto program = h2::loadProgram(file, {libs, {}}, diags);
        if (program->main()) h2::resolveProgram(*program, diags);
        diags.print(std::cerr);
        return diags.hasErrors() ? 1 : 0;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        std::cerr << "h2c: cannot open '" << file << "'\n";
        return 2;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    std::string src = buf.str();
    auto tokens = h2::lex(src, diags);
    if (mode == "--tokens") {
        dumpTokens(tokens);
    } else {
        h2::Module mod = h2::parse(std::move(tokens), diags);
        if (!diags.hasErrors()) h2::printAst(mod, std::cout);
    }
    diags.print(std::cerr);
    return diags.hasErrors() ? 1 : 0;
}
