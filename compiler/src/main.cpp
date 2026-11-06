// h2c: the Hydrogen compiler driver.
//
//   h2c --tokens file.h2   dump the token stream
//   h2c --ast file.h2      dump the parsed AST
//   h2c --check file.h2    parse only; report errors
//
// Exit code: 0 on success, 1 on compile errors, 2 on usage/IO errors.

#include "hydrogen/AstPrinter.h"
#include "hydrogen/Lexer.h"
#include "hydrogen/Parser.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

void usage() {
    std::cerr << "usage: h2c [--tokens | --ast | --check] <file.h2>\n";
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
    if (argc != 3) {
        usage();
        return 2;
    }
    std::string mode = argv[1];
    if (mode != "--tokens" && mode != "--ast" && mode != "--check") {
        usage();
        return 2;
    }
    std::ifstream in(argv[2], std::ios::binary);
    if (!in) {
        std::cerr << "h2c: cannot open '" << argv[2] << "'\n";
        return 2;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    std::string src = buf.str();

    h2::Diagnostics diags(argv[2]);
    auto tokens = h2::lex(src, diags);
    if (mode == "--tokens") {
        dumpTokens(tokens);
    } else {
        h2::Module mod = h2::parse(std::move(tokens), diags);
        if (mode == "--ast" && !diags.hasErrors()) h2::printAst(mod, std::cout);
    }
    diags.print(std::cerr);
    return diags.hasErrors() ? 1 : 0;
}
