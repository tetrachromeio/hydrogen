// `hy lsp`: a Language Server Protocol server for Hydrogen.
//
// Every edit re-runs the front end (load, resolve, type and move checking)
// on the project containing the file, using the editor's unsaved text, then:
//   - publishes diagnostics for every file of the program
//   - answers textDocument/definition and textDocument/hover from the checked AST

#include "hydrogen/Lsp.h"
#include "hydrogen/Checker.h"
#include "hydrogen/Program.h"
#include "hydrogen/Project.h"
#include "hydrogen/Types.h"

#include <cstring>
#include <iostream>
#include <optional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <variant>

namespace h2 {
namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------------ JSON

struct Json;
using JsonObject = std::map<std::string, Json>;
using JsonArray = std::vector<Json>;

struct Json {
    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<JsonArray>, std::shared_ptr<JsonObject>> v;

    Json() : v(nullptr) {}
    Json(std::nullptr_t) : v(nullptr) {}
    Json(bool b) : v(b) {}
    Json(int n) : v(static_cast<double>(n)) {}
    Json(double n) : v(n) {}
    Json(const char* s) : v(std::string(s)) {}
    Json(std::string s) : v(std::move(s)) {}
    Json(JsonArray a) : v(std::make_shared<JsonArray>(std::move(a))) {}
    Json(JsonObject o) : v(std::make_shared<JsonObject>(std::move(o))) {}

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(v); }
    const JsonObject* obj() const {
        auto p = std::get_if<std::shared_ptr<JsonObject>>(&v);
        return p ? p->get() : nullptr;
    }
    const JsonArray* arr() const {
        auto p = std::get_if<std::shared_ptr<JsonArray>>(&v);
        return p ? p->get() : nullptr;
    }
    std::string str() const {
        auto p = std::get_if<std::string>(&v);
        return p ? *p : "";
    }
    double num() const {
        auto p = std::get_if<double>(&v);
        return p ? *p : 0;
    }
    const Json& operator[](const std::string& k) const {
        static const Json none;
        if (const JsonObject* o = obj())
            if (auto it = o->find(k); it != o->end()) return it->second;
        return none;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    Json parse() {
        Json v = value();
        return v;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
    }
    Json value() {
        ws();
        if (i_ >= s_.size()) return {};
        char c = s_[i_];
        if (c == '{') {
            ++i_;
            JsonObject o;
            ws();
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return o;
            }
            while (i_ < s_.size()) {
                ws();
                std::string k = string();
                ws();
                if (i_ < s_.size() && s_[i_] == ':') ++i_;
                o[k] = value();
                ws();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == '}') ++i_;
                break;
            }
            return o;
        }
        if (c == '[') {
            ++i_;
            JsonArray a;
            ws();
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return a;
            }
            while (i_ < s_.size()) {
                a.push_back(value());
                ws();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == ']') ++i_;
                break;
            }
            return a;
        }
        if (c == '"') return string();
        if (s_.compare(i_, 4, "true") == 0) { i_ += 4; return true; }
        if (s_.compare(i_, 5, "false") == 0) { i_ += 5; return false; }
        if (s_.compare(i_, 4, "null") == 0) { i_ += 4; return nullptr; }
        size_t start = i_;
        while (i_ < s_.size() && (isdigit(static_cast<unsigned char>(s_[i_])) || strchr("+-.eE", s_[i_]))) ++i_;
        return std::stod(s_.substr(start, i_ - start).empty() ? "0" : s_.substr(start, i_ - start));
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    std::string string() {
        std::string out;
        if (i_ >= s_.size() || s_[i_] != '"') return out;
        ++i_;
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) break;
            char e = s_[i_++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned cp = static_cast<unsigned>(std::stoul(s_.substr(i_, 4), nullptr, 16));
                i_ += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(i_, 2, "\\u") == 0) {
                    unsigned lo = static_cast<unsigned>(std::stoul(s_.substr(i_ + 2, 4), nullptr, 16));
                    i_ += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: out += e;
            }
        }
        ++i_;
        return out;
    }
};

void writeJson(std::ostream& os, const Json& j) {
    if (j.isNull()) os << "null";
    else if (auto b = std::get_if<bool>(&j.v)) os << (*b ? "true" : "false");
    else if (auto n = std::get_if<double>(&j.v)) {
        if (*n == static_cast<long long>(*n)) os << static_cast<long long>(*n);
        else os << *n;
    } else if (auto s = std::get_if<std::string>(&j.v)) {
        os << '"';
        for (unsigned char c : *s) {
            switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    os << buf;
                } else {
                    os << c;
                }
            }
        }
        os << '"';
    } else if (const JsonArray* a = j.arr()) {
        os << '[';
        for (size_t i = 0; i < a->size(); ++i) {
            if (i) os << ',';
            writeJson(os, (*a)[i]);
        }
        os << ']';
    } else if (const JsonObject* o = j.obj()) {
        os << '{';
        bool first = true;
        for (const auto& [k, v] : *o) {
            if (!first) os << ',';
            first = false;
            writeJson(os, Json(k));
            os << ':';
            writeJson(os, v);
        }
        os << '}';
    }
}

// ------------------------------------------------------------------ transport

bool readMessage(std::string& body) {
    size_t length = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if (line.rfind("Content-Length:", 0) == 0) length = std::stoul(line.substr(15));
    }
    if (!std::cin || length == 0) return false;
    body.assign(length, '\0');
    std::cin.read(body.data(), static_cast<std::streamsize>(length));
    return static_cast<size_t>(std::cin.gcount()) == length;
}

void sendMessage(const Json& msg) {
    std::ostringstream os;
    writeJson(os, msg);
    std::string body = os.str();
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

std::string uriToPath(const std::string& uri) {
    std::string p = uri.rfind("file://", 0) == 0 ? uri.substr(7) : uri;
    std::string out;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '%' && i + 2 < p.size()) {
            out += static_cast<char>(std::stoi(p.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += p[i];
        }
    }
    return out;
}

std::string pathToUri(const fs::path& path) {
    std::string out = "file://";
    for (unsigned char c : path.generic_string()) {
        if (isalnum(c) || strchr("/-_.~", c)) {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

fs::path normalize(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(fs::absolute(p), ec);
    return ec ? fs::absolute(p).lexically_normal() : c;
}

Json position(SourceLoc loc) { return JsonObject{{"line", static_cast<int>(loc.line) - 1}, {"character", static_cast<int>(loc.col) - 1}}; }

Json range(SourceLoc loc, size_t len) {
    SourceLoc end{loc.line, static_cast<uint32_t>(loc.col + std::max<size_t>(len, 1))};
    return JsonObject{{"start", position(loc)}, {"end", position(end)}};
}

// ------------------------------------------------------------------ AST index

// A name in the source that refers to something: what it is and where it's defined.
struct Ref {
    SourceLoc loc;
    size_t len = 0;
    std::string name;
    const Symbol* sym = nullptr;   // the symbol, if any
    const Type* type = nullptr;    // the expression's type, if known
    SourceLoc defLoc;              // for fields and methods (no symbol)
    const SourceFile* defFile = nullptr;
    std::string hover;             // precomputed hover text, if any
};

std::string typeRefStr(const TypeRef& t) {
    switch (t.kind) {
    case TypeRef::Kind::Any: return "any " + t.name;
    case TypeRef::Kind::Function: {
        std::string s = "function(";
        for (size_t i = 0; i < t.args.size(); ++i) s += (i ? ", " : "") + typeRefStr(*t.args[i]);
        s += ")";
        if (t.elem) s += " -> " + typeRefStr(*t.elem);
        return s;
    }
    case TypeRef::Kind::Array: return "[" + typeRefStr(*t.elem) + (t.size ? "; N]" : "]");
    case TypeRef::Kind::Named: {
        std::string s = t.name;
        if (!t.args.empty()) {
            s += "<";
            for (size_t i = 0; i < t.args.size(); ++i) s += (i ? ", " : "") + typeRefStr(*t.args[i]);
            s += ">";
        }
        return s;
    }
    }
    return "?";
}

std::string signature(const FunctionDecl& f) {
    std::string s = std::string(f.isMut ? "mut " : "") + (f.isOperator ? "operator " : "function ") + f.name + "(";
    for (size_t i = 0; i < f.params.size(); ++i) {
        const Param& p = f.params[i];
        s += (i ? ", " : "") + p.name + ": " + (p.isOwn ? "own " : p.isVar ? "var " : "") + typeRefStr(*p.type);
    }
    s += ")";
    if (f.returnType) s += " -> " + typeRefStr(*f.returnType);
    return s;
}

class Indexer {
public:
    Indexer(const Program& prog, const SourceFile& file) : prog_(prog), file_(file) {
        for (const auto& m : prog.modules)
            for (const auto& [name, s] : m->defs) declSym_[s->decl] = s;
    }

    std::vector<Ref> run() {
        for (const auto& d : file_.ast.decls) decl(*d);
        return std::move(refs_);
    }

private:
    const Program& prog_;
    const SourceFile& file_;
    std::vector<Ref> refs_;
    std::map<const Decl*, const Symbol*> declSym_;

    void add(SourceLoc loc, const std::string& name, const Symbol* sym, const Type* type) {
        if (loc.line == 0 || name.empty()) return;
        refs_.push_back({loc, name.size(), name, sym, type, {}, nullptr, ""});
    }

    void type(const TypeRef& t) {
        if (t.kind == TypeRef::Kind::Named || t.kind == TypeRef::Kind::Any) {
            std::string last = t.name.substr(t.name.rfind('.') == std::string::npos ? 0 : t.name.rfind('.') + 1);
            SourceLoc at = t.loc;
            if (t.kind == TypeRef::Kind::Any) at.col += 4; // after "any "
            if (t.name.find('.') != std::string::npos) at.col += static_cast<uint32_t>(t.name.rfind('.') + 1);
            add(at, last, t.sym, nullptr);
        }
        for (const auto& a : t.args) type(*a);
        if (t.elem) type(*t.elem);
    }

    // Fields and methods have no symbol: find their declaration from the object's type.
    void member(const MemberExpr& m) {
        const Type* ot = m.object->type;
        if (!ot) return;
        if (ot->kind == Type::Kind::Ptr) ot = ot->elem();
        if (ot->kind != Type::Kind::Named) return;
        const Symbol* owner = ot->sym;
        SourceLoc at{m.loc.line, m.loc.col + 1};
        Ref r{at, m.member.size(), m.member, nullptr, m.type, {}, owner->file, ""};
        auto fields = [&](const std::vector<Field>& fs) {
            for (const auto& f : fs)
                if (f.name == m.member) {
                    r.defLoc = f.loc;
                    r.hover = "field " + f.name + ": " + typeRefStr(*f.type) + "    (in " + owner->name + ")";
                }
        };
        if (owner->kind == Symbol::Kind::Class) {
            const auto* cd = static_cast<const ClassDecl*>(owner->decl);
            fields(cd->fields);
            for (const auto& fn : cd->methods)
                if (!fn->isOperator && fn->name == m.member) {
                    r.defLoc = fn->loc;
                    r.hover = signature(*fn) + "    (in " + owner->name + ")";
                }
        } else if (owner->kind == Symbol::Kind::Type) {
            fields(static_cast<const TypeDecl*>(owner->decl)->fields);
        }
        if (r.defLoc.line) refs_.push_back(r);
    }

    void expr(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Ident: {
            const auto& id = static_cast<const IdentExpr&>(e);
            add(id.loc, id.name, id.sym, e.type);
            for (const auto& t : id.typeArgs) type(*t);
            return;
        }
        case ExprKind::Member: {
            const auto& m = static_cast<const MemberExpr&>(e);
            expr(*m.object);
            if (m.sym) add({m.loc.line, m.loc.col + 1}, m.member, m.sym, e.type);
            else member(m);
            return;
        }
        case ExprKind::Unary: expr(*static_cast<const UnaryExpr&>(e).operand); return;
        case ExprKind::Binary: {
            const auto& b = static_cast<const BinaryExpr&>(e);
            expr(*b.lhs);
            expr(*b.rhs);
            return;
        }
        case ExprKind::Call: {
            const auto& c = static_cast<const CallExpr&>(e);
            expr(*c.callee);
            for (const auto& a : c.args) expr(*a);
            return;
        }
        case ExprKind::Index: {
            const auto& i = static_cast<const IndexExpr&>(e);
            expr(*i.object);
            expr(*i.index);
            return;
        }
        case ExprKind::Try: expr(*static_cast<const TryExpr&>(e).operand); return;
        case ExprKind::Alloc: {
            const auto& a = static_cast<const AllocExpr&>(e);
            if (a.init) expr(*a.init);
            if (a.arrayType) type(*a.arrayType);
            return;
        }
        case ExprKind::ArrayLit:
            for (const auto& x : static_cast<const ArrayLitExpr&>(e).elems) expr(*x);
            return;
        case ExprKind::Match: {
            const auto& m = static_cast<const MatchExpr&>(e);
            expr(*m.subject);
            for (const auto& arm : m.arms) {
                pattern(*arm.pattern);
                if (arm.block) stmt(*arm.block);
                if (arm.expr) expr(*arm.expr);
            }
            return;
        }
        case ExprKind::Lambda: function(*static_cast<const LambdaExpr&>(e).fn); return;
        default: return;
        }
    }

    void pattern(const Pattern& p) {
        if (p.sym) add(p.loc, p.name.substr(p.name.rfind('.') == std::string::npos ? 0 : p.name.rfind('.') + 1), p.sym, nullptr);
        for (const auto& f : p.fields) pattern(*f);
        if (p.literal) expr(*p.literal);
    }

    void stmt(const Stmt& s) {
        switch (s.kind) {
        case StmtKind::Block:
            for (const auto& c : static_cast<const Block&>(s).stmts) stmt(*c);
            return;
        case StmtKind::Let: {
            const auto& l = static_cast<const LetStmt&>(s);
            if (l.type) type(*l.type);
            if (l.init) expr(*l.init);
            return;
        }
        case StmtKind::Assign: {
            const auto& a = static_cast<const AssignStmt&>(s);
            expr(*a.target);
            expr(*a.value);
            return;
        }
        case StmtKind::Expr: expr(*static_cast<const ExprStmt&>(s).expr); return;
        case StmtKind::If: {
            const auto& i = static_cast<const IfStmt&>(s);
            expr(*i.cond);
            stmt(*i.then);
            if (i.otherwise) stmt(*i.otherwise);
            return;
        }
        case StmtKind::While: {
            const auto& w = static_cast<const WhileStmt&>(s);
            expr(*w.cond);
            stmt(*w.body);
            return;
        }
        case StmtKind::For: {
            const auto& f = static_cast<const ForStmt&>(s);
            expr(*f.iterable);
            stmt(*f.body);
            return;
        }
        case StmtKind::Return:
            if (const auto& v = static_cast<const ReturnStmt&>(s).value) expr(*v);
            return;
        case StmtKind::Free: expr(*static_cast<const FreeStmt&>(s).target); return;
        default: return;
        }
    }

    void function(const FunctionDecl& f) {
        for (const auto& g : f.generics)
            for (const auto& c : g.constraints) type(*c);
        for (const auto& p : f.params) {
            type(*p.type);
            if (p.sym) add(p.loc, p.name, p.sym, nullptr);
        }
        if (f.returnType) type(*f.returnType);
        if (f.body) stmt(*f.body);
    }

    void decl(const Decl& d) {
        switch (d.kind) {
        case DeclKind::Function: function(static_cast<const FunctionDecl&>(d)); return;
        case DeclKind::Type: {
            const auto& t = static_cast<const TypeDecl&>(d);
            if (t.aliasOf) type(*t.aliasOf);
            for (const auto& f : t.fields) type(*f.type);
            for (const auto& op : t.operators) function(*op);
            return;
        }
        case DeclKind::Class: {
            const auto& c = static_cast<const ClassDecl&>(d);
            for (const auto& f : c.fields) type(*f.type);
            for (const auto& m : c.methods) function(*m);
            return;
        }
        case DeclKind::Enum:
            for (const auto& v : static_cast<const EnumDecl&>(d).variants)
                for (const auto& p : v.payload) type(*p);
            return;
        case DeclKind::Interface:
            for (const auto& m : static_cast<const InterfaceDecl&>(d).members) function(*m);
            return;
        case DeclKind::Const: {
            const auto& c = static_cast<const ConstDecl&>(d);
            if (c.type) type(*c.type);
            expr(*c.value);
            return;
        }
        default: return;
        }
    }
};

// ------------------------------------------------------------------ the server

struct Analysis {
    std::unique_ptr<Program> program;
    std::unique_ptr<TypeContext> types;
    std::map<fs::path, const SourceFile*> files; // absolute path -> file in the program
};

class Server {
public:
    explicit Server(std::vector<fs::path> stdlib) : stdlib_(std::move(stdlib)) {}

    int run() {
        std::string body;
        while (readMessage(body)) {
            Json msg = JsonParser(body).parse();
            std::string method = msg["method"].str();
            const Json& id = msg["id"];
            const Json& params = msg["params"];
            if (method == "initialize") {
                reply(id, JsonObject{{"capabilities",
                                      JsonObject{{"textDocumentSync", 1},
                                                 {"definitionProvider", true},
                                                 {"hoverProvider", true}}},
                                     {"serverInfo", JsonObject{{"name", "hy lsp"}}}});
            } else if (method == "shutdown") {
                reply(id, nullptr);
            } else if (method == "exit") {
                return 0;
            } else if (method == "textDocument/didOpen") {
                open(params["textDocument"]["uri"].str(), params["textDocument"]["text"].str());
            } else if (method == "textDocument/didChange") {
                const JsonArray* changes = params["contentChanges"].arr();
                if (changes && !changes->empty())
                    open(params["textDocument"]["uri"].str(), changes->back()["text"].str());
            } else if (method == "textDocument/didSave") {
                analyze(fs::path(uriToPath(params["textDocument"]["uri"].str())));
            } else if (method == "textDocument/didClose") {
                overlays_.erase(normalize(uriToPath(params["textDocument"]["uri"].str())));
            } else if (method == "textDocument/definition") {
                reply(id, definition(params));
            } else if (method == "textDocument/hover") {
                reply(id, hover(params));
            } else if (!id.isNull()) {
                sendMessage(JsonObject{{"jsonrpc", "2.0"}, {"id", id},
                                       {"error", JsonObject{{"code", -32601}, {"message", "method not supported"}}}});
            }
        }
        return 0;
    }

private:
    std::vector<fs::path> stdlib_;
    std::map<fs::path, std::string> overlays_;
    std::map<fs::path, Analysis> analyses_;          // by main file
    std::map<fs::path, std::set<std::string>> published_; // main file -> URIs with diagnostics

    void reply(const Json& id, Json result) {
        sendMessage(JsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
    }

    void open(const std::string& uri, const std::string& text) {
        fs::path path = normalize(uriToPath(uri));
        overlays_[path] = text;
        analyze(path);
    }

    // The program a file belongs to: its hy project, or the file on its own.
    void analyze(const fs::path& file) {
        fs::path path = normalize(file);
        LoadOptions opts{stdlib_, {}, overlays_};
        fs::path mainFile = path;
        if (auto root = findProjectRoot(path.parent_path())) {
            std::vector<std::string> errors;
            if (auto m = loadManifest(*root, errors)) {
                opts.packages = resolveDependencies(*m, errors);
                mainFile = m->entry;
            }
        }
        Analysis a = check(mainFile, opts);
        if (!a.files.count(path) && mainFile != path) {
            // Not part of the project's program: check the file by itself.
            mainFile = path;
            a = check(mainFile, opts);
        }
        publish(mainFile, a);
        analyses_[mainFile] = std::move(a);
    }

    Diagnostics lastDiags_;

    Analysis check(const fs::path& mainFile, const LoadOptions& opts) {
        Analysis a;
        lastDiags_ = Diagnostics();
        a.types = std::make_unique<TypeContext>();
        a.program = loadProgram(mainFile, opts, lastDiags_);
        if (a.program->main()) {
            resolveProgram(*a.program, lastDiags_);
            typecheckProgram(*a.program, *a.types, lastDiags_);
        }
        for (const auto& m : a.program->modules)
            for (const auto& f : m->files) a.files[normalize(f->file)] = f.get();
        return a;
    }

    void publish(const fs::path& mainFile, const Analysis& a) {
        std::map<std::string, JsonArray> byUri;
        std::map<std::string, std::string> displayToUri;
        for (const auto& [path, f] : a.files) {
            displayToUri[f->displayFile] = pathToUri(path);
            byUri[pathToUri(path)]; // every file gets a (possibly empty) list
        }
        for (const auto& d : lastDiags_.errors()) {
            auto it = displayToUri.find(d.file);
            if (it == displayToUri.end()) continue;
            byUri[it->second].push_back(JsonObject{{"range", range(d.loc, 1)},
                                                   {"severity", 1},
                                                   {"source", "hydrogen"},
                                                   {"message", d.message}});
        }
        std::set<std::string>& before = published_[mainFile];
        for (const auto& uri : before)
            if (!byUri.count(uri)) byUri[uri]; // clear files no longer in the program
        before.clear();
        for (auto& [uri, list] : byUri) {
            if (!list.empty()) before.insert(uri);
            sendMessage(JsonObject{{"jsonrpc", "2.0"},
                                   {"method", "textDocument/publishDiagnostics"},
                                   {"params", JsonObject{{"uri", uri}, {"diagnostics", std::move(list)}}}});
        }
    }

    // Finds the checked file and the name under the cursor.
    std::optional<std::pair<const Analysis*, Ref>> lookup(const Json& params, const SourceFile** fileOut) {
        fs::path path = normalize(uriToPath(params["textDocument"]["uri"].str()));
        uint32_t line = static_cast<uint32_t>(params["position"]["line"].num()) + 1;
        uint32_t col = static_cast<uint32_t>(params["position"]["character"].num()) + 1;
        for (const auto& [main, a] : analyses_) {
            auto it = a.files.find(path);
            if (it == a.files.end()) continue;
            *fileOut = it->second;
            for (const Ref& r : Indexer(*a.program, *it->second).run())
                if (r.loc.line == line && col >= r.loc.col && col < r.loc.col + r.len) return std::make_pair(&a, r);
        }
        return std::nullopt;
    }

    const SourceFile* fileOfSymbol(const Analysis& a, const Symbol* s, const SourceFile* current) {
        if (s->file) return s->file;
        if (s->kind == Symbol::Kind::Variant) // variants live in their enum's file
            for (const auto& m : a.program->modules)
                for (const auto& [name, e] : m->defs)
                    if (e->decl == s->decl) return e->file;
        return current; // locals and parameters: the file being edited
    }

    Json definition(const Json& params) {
        const SourceFile* file = nullptr;
        auto found = lookup(params, &file);
        if (!found) return nullptr;
        const auto& [a, r] = *found;
        SourceLoc loc;
        const SourceFile* target = nullptr;
        if (r.sym && r.sym->kind != Symbol::Kind::Builtin && r.sym->kind != Symbol::Kind::Module) {
            loc = r.sym->loc;
            target = fileOfSymbol(*a, r.sym, file);
        } else if (r.defLoc.line) {
            loc = r.defLoc;
            target = r.defFile ? r.defFile : file;
        }
        if (!target || !loc.line) return nullptr;
        return JsonObject{{"uri", pathToUri(normalize(target->file))}, {"range", range(loc, 1)}};
    }

    Json hover(const Json& params) {
        const SourceFile* file = nullptr;
        auto found = lookup(params, &file);
        if (!found) return nullptr;
        const auto& [a, r] = *found;
        std::string text = r.hover;
        if (text.empty() && r.sym) {
            const Symbol* s = r.sym;
            std::string where = s->module && s->module->path.size() ? "    (" + s->module->path + ")" : "";
            switch (s->kind) {
            case Symbol::Kind::Local:
                text = s->name + ": " + (r.type ? TypeContext::str(r.type) : std::string("?"));
                if (s->isMutable) text = "var " + text;
                break;
            case Symbol::Kind::Function: text = signature(*static_cast<const FunctionDecl*>(s->decl)) + where; break;
            case Symbol::Kind::Const: {
                const auto* c = static_cast<const ConstDecl*>(s->decl);
                text = "const " + c->name + (r.type ? ": " + TypeContext::str(r.type) : std::string()) + where;
                break;
            }
            case Symbol::Kind::Variant: text = "variant " + s->name + where; break;
            case Symbol::Kind::Generic: text = "generic parameter " + s->name; break;
            default: text = std::string(symbolKindName(s->kind)) + " " + s->name + where;
            }
        }
        if (text.empty()) return nullptr;
        return JsonObject{{"contents", JsonObject{{"kind", "markdown"}, {"value", "```hydrogen\n" + text + "\n```"}}},
                          {"range", range(r.loc, r.len)}};
    }
};

} // namespace

int runLanguageServer(const std::vector<fs::path>& stdlibPaths) {
    std::ios::sync_with_stdio(false);
    return Server(stdlibPaths).run();
}

} // namespace h2
