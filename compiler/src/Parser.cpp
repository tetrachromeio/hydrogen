#include "hydrogen/Lexer.h"
#include "hydrogen/Parser.h"

#include <functional>
#include <optional>

namespace h2 {
namespace {

struct ParseError {}; // thrown after an error is reported; caught by recovery points

struct BinOp {
    const char* op;
    int prec;
    int ntoks;
};

class Parser {
public:
    Parser(std::vector<Token> toks, Diagnostics& diags) : toks_(std::move(toks)), diags_(diags) {}

    Module parseModule() {
        Module m;
        m.fileName = diags_.fileName();
        while (!at(Tok::Eof)) {
            if (accept(Tok::Semi)) continue;
            if (at(Tok::RBrace)) {
                error(peek().loc, "unexpected '}' at top level");
                advance();
                continue;
            }
            try {
                m.decls.push_back(parseTopDecl());
            } catch (const ParseError&) {
                syncTopLevel();
            }
        }
        return m;
    }

private:
    std::vector<Token> toks_;
    Diagnostics& diags_;
    size_t pos_ = 0;
    int speculating_ = 0;
    int nest_ = 0; // > 0 inside (...) or [...]: newlines don't end expressions

    // ------------------------------------------------------------ token helpers

    const Token& peek(size_t n = 0) const {
        size_t i = pos_ + n;
        return i < toks_.size() ? toks_[i] : toks_.back();
    }
    bool at(Tok k) const { return peek().kind == k; }
    bool atIdent(std::string_view s) const { return at(Tok::Ident) && peek().text == s; }
    const Token& advance() {
        const Token& t = toks_[pos_];
        if (pos_ + 1 < toks_.size()) ++pos_;
        return t;
    }
    bool accept(Tok k) {
        if (!at(k)) return false;
        advance();
        return true;
    }
    // True if the current token may continue the expression on its left.
    bool sameLine() const { return nest_ > 0 || !peek().newlineBefore; }

    void error(SourceLoc loc, std::string msg) {
        if (!speculating_) diags_.error(loc, std::move(msg));
    }
    [[noreturn]] void fail(SourceLoc loc, std::string msg) {
        error(loc, std::move(msg));
        throw ParseError{};
    }
    [[noreturn]] void failExpected(std::string_view what) {
        const Token& t = peek();
        std::string found = t.kind == Tok::Ident ? "'" + t.text + "'" : std::string(tokName(t.kind));
        fail(t.loc, "expected " + std::string(what) + ", found " + found);
    }
    const Token& expect(Tok k, std::string_view what = {}) {
        if (!at(k)) failExpected(what.empty() ? tokName(k) : what);
        return advance();
    }
    std::string expectIdent(std::string_view what) {
        if (!at(Tok::Ident)) failExpected(what);
        return advance().text;
    }

    // A statement or member must end at a newline, ';', '}', or end of file.
    void expectTerminator(std::string_view what) {
        if (accept(Tok::Semi) || at(Tok::RBrace) || at(Tok::Eof) || peek().newlineBefore) return;
        fail(peek().loc, "expected end of " + std::string(what) + " (newline or ';') before " +
                             std::string(at(Tok::Ident) ? "'" + peek().text + "'" : tokName(peek().kind)));
    }

    // RAII guard that resets nesting while parsing a {...} body.
    struct NestReset {
        Parser& p;
        int saved;
        explicit NestReset(Parser& p) : p(p), saved(p.nest_) { p.nest_ = 0; }
        ~NestReset() { p.nest_ = saved; }
    };
    struct NestIn {
        Parser& p;
        explicit NestIn(Parser& p) : p(p) { ++p.nest_; }
        ~NestIn() { --p.nest_; }
    };

    // ------------------------------------------------------------ recovery

    // Skips tokens (respecting brace nesting) until `stop` holds at depth 0,
    // or a '}' that closes the enclosing block. Always makes progress.
    void syncUntil(const std::function<bool(const Token&)>& stop) {
        int depth = 0;
        if (!at(Tok::RBrace) && !at(Tok::Eof)) {
            if (at(Tok::LBrace)) ++depth;
            advance();
        }
        while (!at(Tok::Eof)) {
            const Token& t = peek();
            if (depth == 0 && (t.kind == Tok::RBrace || stop(t))) return;
            if (t.kind == Tok::LBrace) ++depth;
            if (t.kind == Tok::RBrace) --depth;
            bool wasSemi = t.kind == Tok::Semi;
            advance();
            if (depth == 0 && wasSemi) return;
        }
    }
    void syncStatement() {
        syncUntil([](const Token& t) { return t.newlineBefore; });
    }
    void syncTopLevel() {
        syncUntil([](const Token& t) {
            return t.newlineBefore && (t.kind == Tok::KwNew || t.kind == Tok::KwUse ||
                                       t.kind == Tok::KwPub || t.kind == Tok::KwInclude);
        });
    }

    // ------------------------------------------------------------ types

    std::string parsePath(std::string_view what) {
        std::string path = expectIdent(what);
        while (at(Tok::Dot) && peek(1).kind == Tok::Ident) {
            advance();
            path += '.';
            path += advance().text;
        }
        return path;
    }

    TypeRefPtr parseType() {
        auto t = std::make_unique<TypeRef>();
        t->loc = peek().loc;
        if (atIdent("any") && peek(1).kind == Tok::Ident) {
            // any Shape: a value of any type that implements the interface
            advance();
            t->kind = TypeRef::Kind::Any;
            t->name = parsePath("an interface name after 'any'");
            return t;
        }
        if (accept(Tok::KwFunction)) {
            // function(int, str) -> bool
            NestIn in(*this);
            t->kind = TypeRef::Kind::Function;
            expect(Tok::LParen, "'(' after 'function' in a function type");
            while (!at(Tok::RParen)) {
                t->args.push_back(parseType());
                if (!accept(Tok::Comma)) break;
            }
            expect(Tok::RParen, "')' to close the function type");
            if (accept(Tok::Arrow)) t->elem = parseType();
            return t;
        }
        if (accept(Tok::LBracket)) {
            NestIn in(*this);
            t->kind = TypeRef::Kind::Array;
            t->elem = parseType();
            if (accept(Tok::Semi)) t->size = parseExpr();
            expect(Tok::RBracket, "']' to close array type");
            return t;
        }
        t->name = parsePath("a type");
        if (at(Tok::Less)) t->args = parseTypeArgs();
        return t;
    }

    std::vector<TypeRefPtr> parseTypeArgs() {
        NestIn in(*this);
        expect(Tok::Less);
        std::vector<TypeRefPtr> args;
        do {
            if (at(Tok::Greater)) break;
            args.push_back(parseType());
        } while (accept(Tok::Comma));
        expect(Tok::Greater, "'>' to close type arguments");
        return args;
    }

    std::vector<GenericParam> parseGenericParams() {
        std::vector<GenericParam> out;
        if (!at(Tok::Less)) return out;
        NestIn in(*this);
        advance();
        do {
            if (at(Tok::Greater)) break;
            GenericParam g;
            g.loc = peek().loc;
            g.name = expectIdent("a generic parameter name");
            if (accept(Tok::Colon)) {
                do g.constraints.push_back(parseType());
                while (accept(Tok::Plus));
            }
            out.push_back(std::move(g));
        } while (accept(Tok::Comma));
        expect(Tok::Greater, "'>' to close generic parameters");
        return out;
    }

    // ------------------------------------------------------------ declarations

    DeclPtr parseTopDecl() {
        if (at(Tok::KwPub) && peek(1).kind == Tok::KwInclude)
            fail(peek().loc, "'pub' is not allowed on 'include': an included file is part of the module");
        bool isPub = accept(Tok::KwPub);
        DeclPtr d = parseTopDeclAfterPub();
        d->isPub = isPub;
        return d;
    }

    // Rejects `pub` where visibility is implied by the enclosing declaration.
    void rejectPub(const char* why) {
        if (at(Tok::KwPub)) fail(peek().loc, std::string("'pub' is not allowed here: ") + why);
    }

    DeclPtr parseTopDeclAfterPub() {
        if (at(Tok::KwUse)) return parseUse();
        if (atIdent("const") && peek(1).kind == Tok::Ident) {
            auto d = std::make_unique<ConstDecl>(advance().loc);
            d->name = expectIdent("a constant name");
            if (accept(Tok::Colon)) d->type = parseType();
            expect(Tok::Assign, "'=' and a value for the constant");
            d->value = parseExpr();
            expectTerminator("constant");
            return d;
        }
        if (at(Tok::KwExtern)) {
            // extern function name(params) -> T     (a C function)
            SourceLoc loc = advance().loc;
            if (!at(Tok::KwFunction)) failExpected("'function' after 'extern'");
            Token fake = peek();
            fake.kind = Tok::KwNew;
            toks_.insert(toks_.begin() + static_cast<long>(pos_), fake);
            auto f = parseFunction(/*bodyOptional=*/true);
            if (f->body) fail(f->body->loc, "an extern function has no body: it is defined in C");
            if (!f->generics.empty()) fail(loc, "an extern function can't be generic");
            f->isExtern = true;
            f->loc = loc;
            return f;
        }
        if (at(Tok::KwInclude)) {
            auto d = std::make_unique<IncludeDecl>(advance().loc);
            if (at(Tok::StringLit))
                fail(peek().loc, "write the file name without quotes or extension: include " +
                                     peek().text.substr(0, peek().text.rfind(".h2")));
            d->name = expectIdent("a file name (without .h2) after 'include'");
            expectTerminator("'include'");
            return d;
        }
        if (!at(Tok::KwNew)) failExpected("'new', 'use', 'include', 'extern' or 'pub' at top level");
        switch (peek(1).kind) {
        case Tok::KwFunction:
        case Tok::KwOperator: return parseFunction(/*bodyOptional=*/false);
        case Tok::KwType: return parseTypeDecl();
        case Tok::Ident:
            if (peek(1).text == "union") {
                // new union Name { fields }: like a type, with all fields in the same storage
                auto d = parseTypeDecl();
                auto& td = static_cast<TypeDecl&>(*d);
                td.isUnion = true;
                if (td.base != TypeDecl::Base::Layout) fail(td.loc, "a union is a set of fields: new union Name { ... }");
                return d;
            }
            advance();
            failExpected("'function', 'operator', 'type', 'class', 'enum', 'union' or 'interface' after 'new'");
        case Tok::KwClass: return parseClass();
        case Tok::KwEnum: return parseEnum();
        case Tok::KwInterface: return parseInterface();
        default:
            advance();
            failExpected("'function', 'operator', 'type', 'class', 'enum' or 'interface' after 'new'");
        }
    }

    DeclPtr parseUse() {
        auto d = std::make_unique<UseDecl>(advance().loc);
        d->path = parsePath("a module name after 'use'");
        if (accept(Tok::KwAs)) d->alias = expectIdent("an alias name after 'as'");
        expectTerminator("'use'");
        return d;
    }

    std::string parseOperatorSymbol() {
        const Token& t = peek();
        switch (t.kind) {
        case Tok::Plus: case Tok::Minus: case Tok::Star: case Tok::Slash: case Tok::Percent:
        case Tok::EqEq: case Tok::NotEq: case Tok::Less: case Tok::LessEq: case Tok::GreaterEq:
        case Tok::Amp: case Tok::Pipe: case Tok::Caret: case Tok::Tilde: case Tok::Shl:
        case Tok::Bang: {
            std::string s(tokName(t.kind));
            advance();
            return s.substr(1, s.size() - 2); // strip quotes
        }
        case Tok::Greater:
            advance();
            if (at(Tok::Greater) && peek().joint) {
                advance();
                return ">>";
            }
            return ">";
        case Tok::LBracket:
            advance();
            expect(Tok::RBracket, "']' in 'operator []'");
            return "[]";
        case Tok::Ident:
            // Conversion operator: how a value becomes text (like Python's __str__).
            if (t.text == "string" || t.text == "hash") {
                std::string name = t.text;
                advance();
                return name;
            }
            fail(t.loc, "'" + t.text + "' is not an operator. Named operators are 'operator string' and 'operator hash'");
        default: failExpected("an overloadable operator");
        }
    }

    void parseParams(FunctionDecl& fd) {
        FunctionDecl* f = &fd;
        {
            NestIn in(*this);
            expect(Tok::LParen, "'(' to start the parameter list");
            while (!at(Tok::RParen)) {
                Param p;
                p.loc = peek().loc;
                p.name = expectIdent("a parameter name");
                expect(Tok::Colon, "':' and a type after parameter name");
                // `own` is contextual: `item: own T` takes ownership of the argument.
                if (atIdent("own") && (peek(1).kind == Tok::Ident || peek(1).kind == Tok::LBracket ||
                                       peek(1).kind == Tok::KwFunction)) {
                    advance();
                    p.isOwn = true;
                } else if (accept(Tok::KwVar)) {
                    p.isVar = true; // `x: var T`: changes the caller's variable in place
                }
                p.type = parseType();
                f->params.push_back(std::move(p));
                if (!accept(Tok::Comma)) break;
            }
            expect(Tok::RParen, "')' to close the parameter list");
        }
    }

    std::unique_ptr<FunctionDecl> parseFunction(bool bodyOptional) {
        auto f = std::make_unique<FunctionDecl>(expect(Tok::KwNew).loc);
        if (atIdent("mut") && peek(1).kind == Tok::KwFunction) {
            advance();
            f->isMut = true;
        }
        if (accept(Tok::KwOperator)) {
            f->isOperator = true;
            f->name = parseOperatorSymbol();
        } else {
            expect(Tok::KwFunction);
            f->name = expectIdent("a function name");
            f->generics = parseGenericParams();
        }
        parseParams(*f);
        if (accept(Tok::Arrow)) f->returnType = parseType();
        if (at(Tok::LBrace))
            f->body = parseBlock();
        else if (!bodyOptional)
            failExpected("'{' to start the function body");
        else
            expectTerminator("function signature");
        return f;
    }

    // Parses `{ member* }`, calling `member` for each entry and recovering
    // from errors per member.
    void parseMembers(const char* what, const std::function<void()>& member) {
        NestReset reset(*this);
        expect(Tok::LBrace, std::string("'{' to start the ") + what + " body");
        while (!at(Tok::RBrace) && !at(Tok::Eof)) {
            if (accept(Tok::Semi) || accept(Tok::Comma)) continue;
            try {
                member();
            } catch (const ParseError&) {
                syncStatement();
            }
        }
        expect(Tok::RBrace, std::string("'}' to close the ") + what);
    }

    Field parseField(const char* what, bool isPub) {
        Field f;
        f.isPub = isPub;
        f.loc = peek().loc;
        f.name = expectIdent(what);
        expect(Tok::Colon, "':' and a type after field name");
        if (atIdent("bits") && peek(1).kind == Tok::LParen) {
            // name: bits(N) -- a packed bitfield, read and written as uint
            SourceLoc at = advance().loc;
            advance();
            const Token& n = expect(Tok::IntLit, "a bit width");
            f.bitWidth = static_cast<uint32_t>(std::stoul(n.text, nullptr, 0));
            expect(Tok::RParen);
            if (f.bitWidth == 0 || f.bitWidth > 64) fail(at, "a bitfield is 1 to 64 bits wide");
            f.type = std::make_unique<TypeRef>();
            f.type->loc = at;
            f.type->name = "uint";
        } else {
            f.type = parseType();
        }
        if (!accept(Tok::Comma)) expectTerminator("field");
        return f;
    }

    DeclPtr parseTypeDecl() {
        auto d = std::make_unique<TypeDecl>(advance().loc);
        advance(); // 'type'
        d->name = expectIdent("a type name");
        d->generics = parseGenericParams();
        if (accept(Tok::Colon)) {
            if ((atIdent("bits") || atIdent("float")) && peek(1).kind == Tok::LParen) {
                d->base = peek().text == "bits" ? TypeDecl::Base::Bits : TypeDecl::Base::Float;
                advance();
                advance();
                const Token& n = expect(Tok::IntLit, "a bit width");
                d->bits = static_cast<uint32_t>(std::stoul(n.text, nullptr, 0));
                expect(Tok::RParen);
                if (d->base == TypeDecl::Base::Bits && atIdent("signed")) {
                    advance();
                    d->isSigned = true;
                }
            } else {
                d->base = TypeDecl::Base::Distinct;
                d->aliasOf = parseType();
            }
        } else if (accept(Tok::Assign)) {
            d->base = TypeDecl::Base::Alias;
            d->aliasOf = parseType();
            if (at(Tok::LBrace))
                fail(peek().loc, "an alias ('new type " + d->name + " = ...') is just another name for an "
                                 "existing type, so it can't add operators. Use ':' to make a new type");
            expectTerminator("type declaration");
            return d;
        }
        if (!at(Tok::LBrace)) {
            if (d->base == TypeDecl::Base::Layout)
                failExpected("':' and a base, '=' and a type, or '{' with fields, after the type name");
            expectTerminator("type declaration");
            return d;
        }
        parseMembers("type", [&] {
            if (at(Tok::KwPub) && peek(1).kind == Tok::KwNew)
                rejectPub("operators are public whenever their type is");
            bool isPub = accept(Tok::KwPub);
            if (at(Tok::KwNew)) {
                if (peek(1).kind == Tok::KwFunction)
                    fail(peek(1).loc, "types cannot have methods, only operators. "
                                      "Use 'new class' for data with behavior");
                if (peek(1).kind != Tok::KwOperator)
                    fail(peek(1).loc, "only 'new operator' is allowed inside a type");
                auto op = parseFunction(false);
                op->isPub = true;
                d->operators.push_back(std::move(op));
                return;
            }
            if (d->base != TypeDecl::Base::Layout)
                fail(peek().loc, "a type built on bits or another type cannot also declare fields");
            d->fields.push_back(parseField("a field name or 'new operator'", isPub));
        });
        return d;
    }

    DeclPtr parseClass() {
        auto d = std::make_unique<ClassDecl>(advance().loc);
        advance(); // 'class'
        d->name = expectIdent("a class name");
        d->generics = parseGenericParams();
        parseMembers("class", [&] {
            if (at(Tok::KwPub) && peek(1).kind == Tok::KwNew && peek(2).kind == Tok::KwOperator)
                rejectPub("operators are public whenever their class is");
            bool isPub = accept(Tok::KwPub);
            if (at(Tok::KwNew)) {
                Tok k = peek(1).kind;
                if (peek(1).kind == Tok::Ident && peek(1).text == "mut" && peek(2).kind == Tok::KwFunction)
                    k = Tok::KwFunction;
                if (k != Tok::KwFunction && k != Tok::KwOperator)
                    fail(peek(1).loc, "only 'new function' and 'new operator' are allowed inside a class");
                auto m = parseFunction(false);
                m->isPub = isPub || m->isOperator;
                d->methods.push_back(std::move(m));
                return;
            }
            d->fields.push_back(parseField("a field name or 'new function'", isPub));
        });
        return d;
    }

    DeclPtr parseEnum() {
        auto d = std::make_unique<EnumDecl>(advance().loc);
        advance(); // 'enum'
        d->name = expectIdent("an enum name");
        d->generics = parseGenericParams();
        parseMembers("enum", [&] {
            rejectPub("variants are public whenever their enum is");
            Variant v;
            v.loc = peek().loc;
            v.name = expectIdent("a variant name");
            if (at(Tok::LParen)) {
                NestIn in(*this);
                advance();
                while (!at(Tok::RParen)) {
                    v.payload.push_back(parseType());
                    if (!accept(Tok::Comma)) break;
                }
                expect(Tok::RParen, "')' to close variant payload");
            }
            d->variants.push_back(std::move(v));
            if (!accept(Tok::Comma)) expectTerminator("variant");
        });
        return d;
    }

    DeclPtr parseInterface() {
        auto d = std::make_unique<InterfaceDecl>(advance().loc);
        advance(); // 'interface'
        d->name = expectIdent("an interface name");
        d->generics = parseGenericParams();
        parseMembers("interface", [&] {
            rejectPub("interface members are always public");
            Tok k = peek(1).kind;
            if (k == Tok::Ident && peek(1).text == "mut" && peek(2).kind == Tok::KwFunction) k = Tok::KwFunction;
            if (!at(Tok::KwNew) || (k != Tok::KwFunction && k != Tok::KwOperator))
                failExpected("'new function' or 'new operator' in interface");
            d->members.push_back(parseFunction(/*bodyOptional=*/true));
        });
        return d;
    }

    // ------------------------------------------------------------ statements

    std::unique_ptr<Block> parseBlock() {
        NestReset reset(*this);
        auto b = std::make_unique<Block>(expect(Tok::LBrace, "'{'").loc);
        while (!at(Tok::RBrace) && !at(Tok::Eof)) {
            if (accept(Tok::Semi)) continue;
            try {
                b->stmts.push_back(parseStmt());
            } catch (const ParseError&) {
                syncStatement();
            }
        }
        expect(Tok::RBrace, "'}' to close the block");
        return b;
    }

    StmtPtr parseStmt() {
        SourceLoc loc = peek().loc;
        switch (peek().kind) {
        case Tok::KwLet:
        case Tok::KwVar: {
            auto s = std::make_unique<LetStmt>(loc);
            s->mutable_ = advance().kind == Tok::KwVar;
            s->name = expectIdent("a variable name");
            if (accept(Tok::Colon)) s->type = parseType();
            if (accept(Tok::Assign))
                s->init = parseExpr();
            else if (!s->mutable_)
                error(loc, "'let' needs a value: let " + s->name + " = ...");
            else if (!s->type)
                error(loc, "'var' needs a type or a value: var " + s->name + ": T");
            expectTerminator("statement");
            return s;
        }
        case Tok::KwIf: return parseIf();
        case Tok::KwWhile: {
            advance();
            auto s = std::make_unique<WhileStmt>(loc);
            s->cond = parseExpr();
            s->body = parseBlock();
            return s;
        }
        case Tok::KwFor: {
            advance();
            auto s = std::make_unique<ForStmt>(loc);
            s->var = expectIdent("a loop variable name");
            expect(Tok::KwIn, "'in'");
            s->iterable = parseExpr();
            s->body = parseBlock();
            return s;
        }
        case Tok::KwReturn: {
            advance();
            auto s = std::make_unique<ReturnStmt>(loc);
            if (!at(Tok::RBrace) && !at(Tok::Semi) && !at(Tok::Eof) && !peek().newlineBefore)
                s->value = parseExpr();
            expectTerminator("statement");
            return s;
        }
        case Tok::KwBreak:
        case Tok::KwContinue: {
            bool isBreak = advance().kind == Tok::KwBreak;
            expectTerminator("statement");
            return std::make_unique<Stmt>(isBreak ? StmtKind::Break : StmtKind::Continue, loc);
        }
        case Tok::KwFree: {
            advance();
            auto s = std::make_unique<FreeStmt>(loc);
            s->target = parseExpr();
            expectTerminator("statement");
            return s;
        }
        case Tok::KwAsm: {
            advance();
            auto s = std::make_unique<AsmStmt>(loc);
            NestReset reset(*this);
            expect(Tok::LBrace, "'{' after 'asm'");
            while (!at(Tok::RBrace) && !at(Tok::Eof)) {
                if (accept(Tok::Semi) || accept(Tok::Comma)) continue;
                if (at(Tok::StringLit)) {
                    s->lines.push_back(advance().text);
                    continue;
                }
                if (atIdent("clobber")) {
                    advance();
                    s->clobbers.push_back(expect(Tok::StringLit, "a register name or \"memory\" after 'clobber'").text);
                    continue;
                }
                AsmOperand op;
                op.loc = peek().loc;
                if (accept(Tok::KwIn)) op.kind = AsmOperand::Kind::In;
                else if (atIdent("out") || atIdent("inout")) {
                    op.kind = advance().text == "out" ? AsmOperand::Kind::Out : AsmOperand::Kind::InOut;
                } else {
                    failExpected("an instruction string, 'in', 'out', 'inout' or 'clobber' in asm");
                }
                if (at(Tok::StringLit)) {
                    op.name = advance().text;
                    op.isRegister = true;
                } else {
                    op.name = expectIdent("an operand name or a register string");
                }
                if (op.kind == AsmOperand::Kind::Out) expect(Tok::Arrow, "'->' and the variable to write");
                else expect(Tok::Assign, "'=' and a value");
                op.expr = parseExpr();
                s->operands.push_back(std::move(op));
            }
            expect(Tok::RBrace, "'}' to close asm");
            return s;
        }
        case Tok::LBrace: return parseBlock();
        case Tok::KwNew:
            fail(loc, "declarations ('new ...') are only allowed at top level or inside a type/class");
        case Tok::KwPub:
            fail(loc, "'pub' is only allowed on top-level declarations and type/class members");
        default: break;
        }

        ExprPtr e = parseExpr();
        if (auto op = peekAssignOp()) {
            auto s = std::make_unique<AssignStmt>(loc);
            s->op = op->op;
            for (int i = 0; i < op->ntoks; ++i) advance();
            s->target = std::move(e);
            s->value = parseExpr();
            expectTerminator("statement");
            return s;
        }
        expectTerminator("statement");
        return std::make_unique<ExprStmt>(loc, std::move(e));
    }

    StmtPtr parseIf() {
        auto s = std::make_unique<IfStmt>(advance().loc);
        s->cond = parseExpr();
        s->then = parseBlock();
        if (accept(Tok::KwElse)) {
            if (at(Tok::KwIf))
                s->otherwise = parseIf();
            else
                s->otherwise = parseBlock();
        }
        return s;
    }

    std::optional<BinOp> peekAssignOp() const {
        if (!sameLine()) return std::nullopt;
        switch (peek().kind) {
        case Tok::Assign: return BinOp{"=", 0, 1};
        case Tok::PlusAssign: return BinOp{"+=", 0, 1};
        case Tok::MinusAssign: return BinOp{"-=", 0, 1};
        case Tok::StarAssign: return BinOp{"*=", 0, 1};
        case Tok::SlashAssign: return BinOp{"/=", 0, 1};
        case Tok::PercentAssign: return BinOp{"%=", 0, 1};
        case Tok::AmpAssign: return BinOp{"&=", 0, 1};
        case Tok::PipeAssign: return BinOp{"|=", 0, 1};
        case Tok::CaretAssign: return BinOp{"^=", 0, 1};
        case Tok::ShlAssign: return BinOp{"<<=", 0, 1};
        case Tok::Greater:
            if (peek(1).kind == Tok::GreaterEq && peek(1).joint) return BinOp{">>=", 0, 2};
            return std::nullopt;
        default: return std::nullopt;
        }
    }

    // ------------------------------------------------------------ expressions

    std::optional<BinOp> peekBinOp() const {
        if (!sameLine()) return std::nullopt;
        switch (peek().kind) {
        case Tok::Star: return BinOp{"*", 10, 1};
        case Tok::Slash: return BinOp{"/", 10, 1};
        case Tok::Percent: return BinOp{"%", 10, 1};
        case Tok::Plus: return BinOp{"+", 9, 1};
        case Tok::Minus: return BinOp{"-", 9, 1};
        case Tok::Shl: return BinOp{"<<", 8, 1};
        case Tok::Amp: return BinOp{"&", 7, 1};
        case Tok::Caret: return BinOp{"^", 6, 1};
        case Tok::Pipe: return BinOp{"|", 5, 1};
        case Tok::EqEq: return BinOp{"==", 4, 1};
        case Tok::NotEq: return BinOp{"!=", 4, 1};
        case Tok::Less: return BinOp{"<", 4, 1};
        case Tok::LessEq: return BinOp{"<=", 4, 1};
        case Tok::GreaterEq: return BinOp{">=", 4, 1};
        case Tok::Greater: {
            const Token& n = peek(1);
            if (n.joint && n.kind == Tok::Greater) return BinOp{">>", 8, 2};
            if (n.joint && n.kind == Tok::GreaterEq) return std::nullopt; // '>>=' assignment
            return BinOp{">", 4, 1};
        }
        case Tok::AmpAmp: return BinOp{"&&", 3, 1};
        case Tok::PipePipe: return BinOp{"||", 2, 1};
        case Tok::DotDot: return BinOp{"..", 1, 1};
        default: return std::nullopt;
        }
    }

    ExprPtr parseExpr() { return parseBinary(1); }

    ExprPtr parseBinary(int minPrec) {
        ExprPtr lhs = parseUnary();
        while (auto op = peekBinOp()) {
            if (op->prec < minPrec) break;
            SourceLoc loc = peek().loc;
            for (int i = 0; i < op->ntoks; ++i) advance();
            ExprPtr rhs = parseBinary(op->prec + 1);
            lhs = std::make_unique<BinaryExpr>(loc, op->op, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }

    ExprPtr parseUnary() {
        SourceLoc loc = peek().loc;
        const char* op = nullptr;
        switch (peek().kind) {
        case Tok::Minus: op = "-"; break;
        case Tok::Bang: op = "!"; break;
        case Tok::Tilde: op = "~"; break;
        case Tok::Amp: op = "&"; break;
        case Tok::Star: op = "*"; break;
        case Tok::KwAlloc: {
            advance();
            auto a = std::make_unique<AllocExpr>(loc);
            if (at(Tok::LBracket))
                a->arrayType = parseType();
            else
                a->init = parsePostfix();
            return a;
        }
        default: return parsePostfix();
        }
        advance();
        return std::make_unique<UnaryExpr>(loc, op, parseUnary());
    }

    ExprPtr parsePostfix() {
        ExprPtr e = parsePrimary();
        for (;;) {
            SourceLoc loc = peek().loc;
            if (at(Tok::LParen) && sameLine()) {
                NestIn in(*this);
                advance();
                auto call = std::make_unique<CallExpr>(loc, std::move(e));
                while (!at(Tok::RParen)) {
                    call->args.push_back(parseExpr());
                    if (!accept(Tok::Comma)) break;
                }
                expect(Tok::RParen, "',' or ')' in argument list");
                e = std::move(call);
            } else if (at(Tok::LBracket) && sameLine()) {
                NestIn in(*this);
                advance();
                ExprPtr idx = parseExpr();
                expect(Tok::RBracket, "']'");
                e = std::make_unique<IndexExpr>(loc, std::move(e), std::move(idx));
            } else if (at(Tok::Question) && sameLine()) {
                advance();
                e = std::make_unique<TryExpr>(loc, std::move(e));
            } else if (at(Tok::Dot)) {
                advance();
                // Keywords are fine as member names: intrinsic.free, x.type
                if (peek().kind >= Tok::KwUse && peek().kind <= Tok::KwPub) {
                    std::string name = advance().text;
                    e = std::make_unique<MemberExpr>(loc, std::move(e), std::move(name));
                    continue;
                }
                std::string name = expectIdent("a member name after '.'");
                e = std::make_unique<MemberExpr>(loc, std::move(e), std::move(name));
            } else {
                return e;
            }
        }
    }

    // After an identifier, tries to read `<T, ...>` followed by '('.
    // If that doesn't parse, rewinds: the '<' is a less-than.
    std::vector<TypeRefPtr> tryCallTypeArgs() {
        size_t saved = pos_;
        ++speculating_;
        try {
            auto args = parseTypeArgs();
            if (at(Tok::LParen) && !peek().newlineBefore) {
                --speculating_;
                return args;
            }
        } catch (const ParseError&) {
        }
        --speculating_;
        pos_ = saved;
        return {};
    }

    ExprPtr parsePrimary() {
        const Token& t = peek();
        SourceLoc loc = t.loc;
        switch (t.kind) {
        case Tok::IntLit: return std::make_unique<LiteralExpr>(ExprKind::IntLit, loc, advance().text);
        case Tok::FloatLit: return std::make_unique<LiteralExpr>(ExprKind::FloatLit, loc, advance().text);
        case Tok::StringLit: {
            std::string text = advance().text;
            if (text.find('{') == std::string::npos && text.find('}') == std::string::npos)
                return std::make_unique<LiteralExpr>(ExprKind::StringLit, loc, text);
            return parseInterpolation(text, loc);
        }
        case Tok::CharLit: return std::make_unique<LiteralExpr>(ExprKind::CharLit, loc, advance().text);
        case Tok::KwTrue:
        case Tok::KwFalse:
            return std::make_unique<LiteralExpr>(ExprKind::BoolLit, loc, advance().text);
        case Tok::KwSelf: advance(); return std::make_unique<SelfExpr>(loc);
        case Tok::KwNull: return std::make_unique<LiteralExpr>(ExprKind::NullLit, loc, advance().text);
        case Tok::Ident: {
            auto id = std::make_unique<IdentExpr>(loc, advance().text);
            if (at(Tok::Less) && !peek().newlineBefore) id->typeArgs = tryCallTypeArgs();
            return id;
        }
        case Tok::LParen: {
            NestIn in(*this);
            advance();
            ExprPtr e = parseExpr();
            expect(Tok::RParen, "')'");
            return e;
        }
        case Tok::LBracket: {
            NestIn in(*this);
            advance();
            auto arr = std::make_unique<ArrayLitExpr>(loc);
            while (!at(Tok::RBracket)) {
                arr->elems.push_back(parseExpr());
                if (!accept(Tok::Comma)) break;
            }
            expect(Tok::RBracket, "',' or ']' in array literal");
            return arr;
        }
        case Tok::KwMatch: return parseMatch();
        case Tok::KwFunction: {
            // function(a: int) -> int { ... }
            auto lam = std::make_unique<LambdaExpr>(advance().loc);
            lam->fn = std::make_unique<FunctionDecl>(loc);
            lam->fn->name = "lambda";
            parseParams(*lam->fn);
            if (accept(Tok::Arrow)) lam->fn->returnType = parseType();
            lam->fn->body = parseBlock();
            return lam;
        }
        default: failExpected("an expression");
        }
    }

    // "a {x} b" becomes "a " + string(x) + " b". {{ and }} are literal braces.
    ExprPtr parseInterpolation(const std::string& text, SourceLoc loc) {
        std::vector<ExprPtr> parts;
        std::string piece;
        auto flushPiece = [&] {
            if (!piece.empty()) parts.push_back(std::make_unique<LiteralExpr>(ExprKind::StringLit, loc, piece));
            piece.clear();
        };
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c == '{' && i + 1 < text.size() && text[i + 1] == '{') { piece += '{'; ++i; continue; }
            if (c == '}' && i + 1 < text.size() && text[i + 1] == '}') { piece += '}'; ++i; continue; }
            if (c == '}') fail(loc, "a lone '}' in a string: write }} for a brace");
            if (c != '{') { piece += c; continue; }
            int depth = 1;
            size_t j = i + 1;
            for (; j < text.size() && depth > 0; ++j) {
                if (text[j] == '{') ++depth;
                if (text[j] == '}') --depth;
            }
            if (depth != 0) fail(loc, "unclosed '{' in a string: write {{ for a brace");
            std::string source = text.substr(i + 1, j - i - 2);
            if (source.find_first_not_of(" \t") == std::string::npos)
                fail(loc, "empty {} in a string: put an expression inside, or write {{}} for braces");
            // Parse the expression with its own tokens, placed inside the string.
            Diagnostics& d = diags_;
            std::vector<Token> toks = lex(source, d);
            for (auto& t : toks) {
                t.loc.col = loc.col + static_cast<uint32_t>(i) + t.loc.col;
                t.loc.line = loc.line;
                t.newlineBefore = false;
            }
            Parser sub(std::move(toks), diags_);
            sub.speculating_ = speculating_;
            sub.nest_ = 1;
            ExprPtr e = sub.parseExpr();
            if (!sub.at(Tok::Eof)) sub.failExpected("the end of the {} expression");
            flushPiece();
            auto call = std::make_unique<CallExpr>(e->loc, std::make_unique<IdentExpr>(e->loc, "string"));
            call->args.push_back(std::move(e));
            parts.push_back(std::move(call));
            i = j - 1;
        }
        flushPiece();
        if (parts.empty()) return std::make_unique<LiteralExpr>(ExprKind::StringLit, loc, "");
        if (parts.size() == 1 && parts[0]->kind == ExprKind::StringLit) return std::move(parts[0]);
        ExprPtr out = std::move(parts[0]);
        if (out->kind == ExprKind::StringLit && parts.size() == 1) return out;
        for (size_t k = 1; k < parts.size(); ++k)
            out = std::make_unique<BinaryExpr>(loc, "+", std::move(out), std::move(parts[k]));
        return out;
    }

    ExprPtr parseMatch() {
        auto m = std::make_unique<MatchExpr>(advance().loc);
        m->subject = parseExpr();
        NestReset reset(*this);
        expect(Tok::LBrace, "'{' after match subject");
        while (!at(Tok::RBrace) && !at(Tok::Eof)) {
            if (accept(Tok::Comma) || accept(Tok::Semi)) continue;
            try {
                MatchArm arm;
                arm.pattern = parsePattern();
                expect(Tok::FatArrow, "'=>' after pattern");
                if (at(Tok::LBrace))
                    arm.block = parseBlock();
                else
                    arm.expr = parseExpr();
                if (!accept(Tok::Comma)) expectTerminator("match arm");
                m->arms.push_back(std::move(arm));
            } catch (const ParseError&) {
                syncStatement();
            }
        }
        expect(Tok::RBrace, "'}' to close match");
        return m;
    }

    PatternPtr parsePattern() {
        auto p = std::make_unique<Pattern>();
        p->loc = peek().loc;
        switch (peek().kind) {
        case Tok::Ident: {
            if (peek().text == "_") {
                advance();
                p->kind = Pattern::Kind::Wildcard;
                return p;
            }
            p->name = parsePath("a pattern");
            bool qualified = p->name.find('.') != std::string::npos;
            if (at(Tok::LParen)) {
                NestIn in(*this);
                advance();
                p->hasParens = true;
                while (!at(Tok::RParen)) {
                    p->fields.push_back(parsePattern());
                    if (!accept(Tok::Comma)) break;
                }
                expect(Tok::RParen, "')' to close pattern");
                p->kind = Pattern::Kind::Variant;
            } else {
                // A bare name is a binding or a unit variant; resolved later.
                p->kind = qualified ? Pattern::Kind::Variant : Pattern::Kind::Binding;
            }
            return p;
        }
        case Tok::IntLit: case Tok::FloatLit: case Tok::StringLit: case Tok::CharLit:
        case Tok::KwTrue: case Tok::KwFalse: case Tok::Minus:
            p->kind = Pattern::Kind::Literal;
            p->literal = parseUnary();
            return p;
        default: failExpected("a pattern");
        }
    }
};

} // namespace

Module parse(std::vector<Token> tokens, Diagnostics& diags) {
    return Parser(std::move(tokens), diags).parseModule();
}

} // namespace h2
