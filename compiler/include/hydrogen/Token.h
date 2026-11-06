#pragma once
#include "hydrogen/Source.h"

#include <string>
#include <string_view>

namespace h2 {

#define H2_KEYWORDS(X)                                                                   \
    X(Use, "use") X(As, "as") X(New, "new") X(Type, "type") X(Class, "class")              \
    X(Function, "function") X(Operator, "operator") X(Enum, "enum")                       \
    X(Interface, "interface") X(Let, "let") X(Var, "var") X(If, "if") X(Else, "else")      \
    X(While, "while") X(For, "for") X(In, "in") X(Return, "return") X(Break, "break")      \
    X(Continue, "continue") X(Match, "match") X(Alloc, "alloc") X(Free, "free")            \
    X(Asm, "asm") X(Self, "self") X(True, "true") X(False, "false") X(Include, "include") X(Extern, "extern") X(Null, "null") X(Pub, "pub")

#define H2_PUNCT(X)                                                                      \
    X(LParen, "(") X(RParen, ")") X(LBrace, "{") X(RBrace, "}") X(LBracket, "[")           \
    X(RBracket, "]") X(Comma, ",") X(Colon, ":") X(Semi, ";") X(Dot, ".")                  \
    X(DotDot, "..") X(Arrow, "->") X(FatArrow, "=>") X(Question, "?") X(Plus, "+")         \
    X(Minus, "-") X(Star, "*") X(Slash, "/") X(Percent, "%") X(EqEq, "==")                 \
    X(NotEq, "!=") X(Less, "<") X(LessEq, "<=") X(Greater, ">") X(GreaterEq, ">=")         \
    X(Amp, "&") X(Pipe, "|") X(Caret, "^") X(Tilde, "~") X(Shl, "<<") X(AmpAmp, "&&")      \
    X(PipePipe, "||") X(Bang, "!") X(Assign, "=") X(PlusAssign, "+=")                      \
    X(MinusAssign, "-=") X(StarAssign, "*=") X(SlashAssign, "/=")                          \
    X(PercentAssign, "%=") X(AmpAssign, "&=") X(PipeAssign, "|=") X(CaretAssign, "^=")     \
    X(ShlAssign, "<<=")

// Note: the lexer never produces ">>" or ">>=". It emits ">" followed by a
// *joint* ">" (or ">="), so `List<List<int>>` closes cleanly. The parser
// rebuilds shift operators from adjacent tokens.

enum class Tok {
    Eof,
    Error,
    Ident,
    IntLit,
    FloatLit,
    StringLit,
    CharLit,
#define X(name, text) Kw##name,
    H2_KEYWORDS(X)
#undef X
#define X(name, text) name,
    H2_PUNCT(X)
#undef X
};

struct Token {
    Tok kind = Tok::Eof;
    std::string text;       // identifier name, decoded literal, or error message
    SourceLoc loc;
    bool newlineBefore = false; // a line break separates this from the previous token
    bool joint = false;         // no whitespace between this and the previous token
};

std::string_view tokName(Tok k);

} // namespace h2
