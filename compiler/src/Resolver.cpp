// Name resolution: binds every identifier, type name and pattern to a Symbol.
//
// Lookup order for an unqualified name:
//   1. lexical scopes (locals, parameters, generic parameters, Self)
//   2. definitions in the current module (including its enum variants)
//   3. built-ins (ptr, intrinsic)
//   4. pub names imported by `use` (and the implicit `use Atomic`)
// A name imported from two different modules is an error only when used.
//
// Qualified names: `M.item` where M is a used module path or a `use ... as`
// alias, and `Enum.Variant`.

#include "hydrogen/Program.h"

#include <algorithm>
#include <functional>
#include <set>

namespace h2 {

std::string_view symbolKindName(Symbol::Kind k) {
    switch (k) {
    case Symbol::Kind::Module: return "module";
    case Symbol::Kind::Function: return "function";
    case Symbol::Kind::Type: return "type";
    case Symbol::Kind::Class: return "class";
    case Symbol::Kind::Enum: return "enum";
    case Symbol::Kind::Variant: return "enum variant";
    case Symbol::Kind::Interface: return "interface";
    case Symbol::Kind::Generic: return "generic parameter";
    case Symbol::Kind::Local: return "variable";
    case Symbol::Kind::Builtin: return "built-in";
    }
    return "?";
}

namespace {
namespace fs = std::filesystem;

using SymbolSet = std::vector<Symbol*>; // small, deduplicated
using NameTable = std::map<std::string, SymbolSet>;

void addUnique(SymbolSet& set, Symbol* s) {
    if (std::find(set.begin(), set.end(), s) == set.end()) set.push_back(s);
}

size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1])});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

struct Seg {
    std::string name;
    SourceLoc loc;
};

class Resolver {
public:
    Resolver(Program& prog, Diagnostics& diags) : prog_(prog), diags_(diags) {
        ptr_ = prog.newSymbol(Symbol::Kind::Builtin, "ptr", {}, nullptr);
        intrinsic_ = prog.newSymbol(Symbol::Kind::Builtin, "intrinsic", {}, nullptr);
        selfType_ = prog.newSymbol(Symbol::Kind::Builtin, "Self", {}, nullptr);
    }

    void run() {
        for (auto& m : prog_.modules) collect(*m);
        for (auto& m : prog_.modules) buildImports(*m);
        for (auto& m : prog_.modules) {
            mod_ = m.get();
            for (auto& f : m->files) {
                file_ = f.get();
                for (auto& d : f->ast.decls) resolveDecl(*d);
            }
        }
    }

private:
    Program& prog_;
    Diagnostics& diags_;
    Symbol* ptr_;
    Symbol* intrinsic_;
    Symbol* selfType_;

    struct Imports {
        NameTable names;                         // unqualified imported names
        std::map<std::string, ModuleInfo*> refs; // "Atomic.Memory", aliases
    };
    std::map<ModuleInfo*, NameTable> exports_;
    std::set<ModuleInfo*> exporting_;
    std::map<const ModuleInfo*, Imports> imports_;

    ModuleInfo* mod_ = nullptr;
    SourceFile* file_ = nullptr;
    std::vector<std::map<std::string, Symbol*>> scopes_;
    bool selfAllowed_ = false; // inside a class method
    size_t captureBoundary_ = 0;
    bool captureReported_ = false; // scopes below this index belong to an enclosing function (lambdas)

    void error(SourceLoc loc, std::string msg) { diags_.error(file_->displayFile, loc, std::move(msg)); }

    // -------------------------------------------------------------- phase 1: definitions

    static std::string declName(const Decl& d) {
        switch (d.kind) {
        case DeclKind::Function: return static_cast<const FunctionDecl&>(d).name;
        case DeclKind::Type: return static_cast<const TypeDecl&>(d).name;
        case DeclKind::Class: return static_cast<const ClassDecl&>(d).name;
        case DeclKind::Enum: return static_cast<const EnumDecl&>(d).name;
        case DeclKind::Interface: return static_cast<const InterfaceDecl&>(d).name;
        case DeclKind::Use:
        case DeclKind::Include: break;
        }
        return "";
    }

    static Symbol::Kind declSymbolKind(DeclKind k) {
        switch (k) {
        case DeclKind::Function: return Symbol::Kind::Function;
        case DeclKind::Type: return Symbol::Kind::Type;
        case DeclKind::Class: return Symbol::Kind::Class;
        case DeclKind::Enum: return Symbol::Kind::Enum;
        default: return Symbol::Kind::Interface;
        }
    }

    void collect(ModuleInfo& m) {
        mod_ = &m;
        for (auto& f : m.files) {
            file_ = f.get();
            collectFile(m, *f);
        }
    }

    void collectFile(ModuleInfo& m, SourceFile& file) {
        for (const auto& d : file.ast.decls) {
            if (d->kind == DeclKind::Use || d->kind == DeclKind::Include) continue;
            if (d->kind == DeclKind::Function && static_cast<const FunctionDecl&>(*d).isOperator) {
                m.extOps.push_back(static_cast<FunctionDecl*>(d.get()));
                continue;
            }
            std::string name = declName(*d);
            Symbol* s = prog_.newSymbol(declSymbolKind(d->kind), name, d->loc, &m);
            s->decl = d.get();
            s->isPub = d->isPub;
            s->file = &file;
            if (auto it = m.defs.find(name); it != m.defs.end()) {
                const Symbol* prev = it->second;
                std::string where = prev->file == &file ? "at line " + std::to_string(prev->loc.line)
                                                        : "in " + prev->file->displayFile + ":" +
                                                              std::to_string(prev->loc.line);
                error(d->loc, "'" + name + "' is already defined " + where);
                continue;
            }
            m.defs[name] = s;
            if (d->kind != DeclKind::Enum) continue;
            for (const auto& v : static_cast<const EnumDecl&>(*d).variants) {
                if (s->members.count(v.name)) {
                    error(v.loc, "variant '" + v.name + "' is defined twice in enum '" + name + "'");
                    continue;
                }
                Symbol* vs = prog_.newSymbol(Symbol::Kind::Variant, v.name, v.loc, &m);
                vs->decl = d.get();
                vs->variant = &v;
                vs->isPub = d->isPub;
                s->members[v.name] = vs;
                m.variants[v.name].push_back(vs);
            }
        }
    }

    // -------------------------------------------------------------- phase 2: imports

    // pub definitions, variants of pub enums, and everything re-exported by `pub use`.
    const NameTable& exportsOf(ModuleInfo* m) {
        if (auto it = exports_.find(m); it != exports_.end()) return it->second;
        static const NameTable empty;
        if (exporting_.count(m)) return empty; // cycle: partial view is fine
        exporting_.insert(m);
        NameTable table;
        for (const auto& [name, s] : m->defs) {
            if (!s->isPub) continue;
            addUnique(table[name], s);
            for (const auto& [vname, v] : s->members) addUnique(table[vname], v);
        }
        for (const auto& imp : m->imports) {
            if (!imp.decl || !imp.decl->isPub || !imp.target) continue;
            if (imp.decl->alias.empty()) {
                for (const auto& [name, syms] : exportsOf(imp.target))
                    for (Symbol* s : syms) addUnique(table[name], s);
            } else {
                Symbol* alias = prog_.newSymbol(Symbol::Kind::Module, imp.decl->alias, imp.decl->loc, imp.target);
                alias->isPub = true;
                addUnique(table[imp.decl->alias], alias);
            }
        }
        exporting_.erase(m);
        return exports_[m] = std::move(table);
    }

    void buildImports(const ModuleInfo& m) {
        Imports& imps = imports_[&m];
        for (const auto& imp : m.imports) {
            if (!imp.target) continue;
            bool aliased = imp.decl && !imp.decl->alias.empty();
            if (aliased) {
                imps.refs[imp.decl->alias] = imp.target;
                continue;
            }
            imps.refs[imp.target->path] = imp.target;
            for (const auto& [name, syms] : exportsOf(imp.target))
                for (Symbol* s : syms) addUnique(imps.names[name], s);
        }
    }

    // -------------------------------------------------------------- lookup

    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }

    void declare(Symbol* s) {
        auto& top = scopes_.back();
        if (auto it = top.find(s->name); it != top.end() && it->second->kind == s->kind) {
            error(s->loc, "'" + s->name + "' is already declared in this scope (line " +
                              std::to_string(it->second->loc.line) + ")");
            return;
        }
        top[s->name] = s;
    }

    Symbol* declareLocal(const std::string& name, SourceLoc loc, bool isMutable) {
        Symbol* s = prog_.newSymbol(Symbol::Kind::Local, name, loc, mod_);
        s->isMutable = isMutable;
        declare(s);
        return s;
    }

    static std::string modulesOf(const SymbolSet& syms) {
        std::string out;
        for (size_t i = 0; i < syms.size(); ++i) {
            if (i) out += i + 1 == syms.size() ? " and " : ", ";
            out += syms[i]->module->displayName();
        }
        return out;
    }

    // Unqualified lookup. Reports ambiguity; returns null (silently) if not found.
    Symbol* lookup(const std::string& name, SourceLoc loc) {
        for (size_t i = scopes_.size(); i > 0; --i) {
            auto f = scopes_[i - 1].find(name);
            if (f == scopes_[i - 1].end()) continue;
            if (i - 1 < captureBoundary_ && f->second->kind == Symbol::Kind::Local) {
                error(loc, "a lambda can't use " + std::string("'") + name +
                               "' from the function around it yet (captures aren't supported); pass it as a parameter");
                captureReported_ = true;
                return nullptr;
            }
            return f->second;
        }
        if (auto it = mod_->defs.find(name); it != mod_->defs.end()) return it->second;
        if (auto it = mod_->variants.find(name); it != mod_->variants.end()) {
            if (it->second.size() > 1) {
                error(loc, "'" + name + "' is a variant of several enums; qualify it, e.g. " +
                               declName(*it->second[0]->decl) + "." + name);
            }
            return it->second[0];
        }
        if (name == "ptr") return ptr_;
        if (name == "intrinsic") return intrinsic_;
        auto& imported = imports_[mod_].names;
        if (auto it = imported.find(name); it != imported.end()) {
            if (it->second.size() > 1) {
                std::string quals;
                for (Symbol* s : it->second) {
                    if (!quals.empty()) quals += " or ";
                    quals += s->module->displayName() + "." + name;
                }
                error(loc, "'" + name + "' is ambiguous: it is exported by " + modulesOf(it->second) +
                               ". Write " + quals);
            }
            return it->second[0];
        }
        return nullptr;
    }

    // Explains why `name` was not found: private, missing `use`, or a typo.
    std::string unknownHint(const std::string& name) {
        for (const auto& imp : mod_->imports) {
            if (!imp.target) continue;
            if (auto it = imp.target->defs.find(name); it != imp.target->defs.end() && !it->second->isPub)
                return ": it exists in module " + imp.target->displayName() + " but is not pub";
        }
        bool isModule = prog_.packages.count(name) > 0;
        for (const auto& dir : prog_.searchPaths) {
            std::error_code ec;
            if (fs::is_regular_file(dir / name / "init.h2", ec)) isModule = true;
        }
        if (isModule) return ". To use the module, add 'use " + name + "'";
        std::string best;
        size_t bestDist = 3;
        auto consider = [&](const std::string& cand) {
            size_t d = editDistance(name, cand);
            if (d < bestDist && d < std::max<size_t>(name.size(), 2)) {
                bestDist = d;
                best = cand;
            }
        };
        for (const auto& sc : scopes_)
            for (const auto& [n, s] : sc) consider(n);
        for (const auto& [n, s] : mod_->defs) consider(n);
        for (const auto& [n, s] : mod_->variants) consider(n);
        for (const auto& [n, s] : imports_[mod_].names) consider(n);
        for (const auto& [n, m] : imports_[mod_].refs) consider(n.substr(0, n.find('.')));
        return best.empty() ? "" : "; did you mean '" + best + "'?";
    }

    struct PathResult {
        Symbol* sym = nullptr;
        size_t used = 0; // number of segments the symbol covers
    };

    Symbol* variantOf(Symbol* e, const Seg& seg) {
        auto it = e->members.find(seg.name);
        if (it != e->members.end()) return it->second;
        error(seg.loc, "enum '" + e->name + "' has no variant '" + seg.name + "'");
        return nullptr;
    }

    // Resolves the longest meaningful prefix of a dotted name. Reports errors;
    // returns {nullptr, 0} on failure.
    PathResult resolvePath(const std::vector<Seg>& segs) {
        const Seg& head = segs[0];
        if (Symbol* s = lookup(head.name, head.loc)) {
            if (s == intrinsic_) {
                if (!mod_->isAtomic()) {
                    error(head.loc, "'intrinsic' is only available inside the Atomic library");
                    return {};
                }
                return {s, segs.size()};
            }
            if (s->kind == Symbol::Kind::Enum && segs.size() > 1) {
                Symbol* v = variantOf(s, segs[1]);
                return v ? PathResult{v, 2} : PathResult{};
            }
            return {s, 1};
        }

        const auto& refs = imports_[mod_].refs;
        std::string prefix;
        std::vector<std::string> prefixes; // prefixes[k] = segs[0..k] joined
        for (const auto& s : segs) {
            prefix += (prefix.empty() ? "" : ".") + s.name;
            prefixes.push_back(prefix);
        }
        for (size_t k = segs.size() - 1; k >= 1; --k) {
            auto it = refs.find(prefixes[k - 1]);
            if (it == refs.end()) continue;
            ModuleInfo* target = it->second;
            const Seg& item = segs[k];
            const auto& exp = exportsOf(target);
            auto e = exp.find(item.name);
            if (e == exp.end()) {
                auto d = target->defs.find(item.name);
                if (d != target->defs.end())
                    error(item.loc, "'" + item.name + "' is not pub in module " + target->displayName());
                else
                    error(item.loc, "module " + target->displayName() + " has no pub item '" + item.name + "'");
                return {};
            }
            if (e->second.size() > 1) {
                error(item.loc, "'" + prefixes[k] + "' is ambiguous: it is re-exported from " +
                                    modulesOf(e->second));
            }
            Symbol* s = e->second[0];
            if (s->kind == Symbol::Kind::Enum && k + 1 < segs.size()) {
                Symbol* v = variantOf(s, segs[k + 1]);
                return v ? PathResult{v, k + 2} : PathResult{};
            }
            return {s, k + 1};
        }
        if (refs.count(head.name) && segs.size() == 1) {
            error(head.loc, "'" + head.name + "' is a module; write " + head.name + ".<name>");
            return {};
        }
        if (captureReported_) {
            captureReported_ = false;
            return {};
        }
        error(head.loc, "unknown name '" + head.name + "'" + unknownHint(head.name));
        return {};
    }

    // -------------------------------------------------------------- types

    size_t genericCount(const Symbol* s) {
        if (s == ptr_) return 1;
        if (!s->decl) return 0;
        switch (s->decl->kind) {
        case DeclKind::Type: return static_cast<const TypeDecl*>(s->decl)->generics.size();
        case DeclKind::Class: return static_cast<const ClassDecl*>(s->decl)->generics.size();
        case DeclKind::Enum: return static_cast<const EnumDecl*>(s->decl)->generics.size();
        case DeclKind::Interface: return static_cast<const InterfaceDecl*>(s->decl)->generics.size();
        case DeclKind::Function: return static_cast<const FunctionDecl*>(s->decl)->generics.size();
        default: return 0;
        }
    }

    static std::string plural(size_t n, const char* word) {
        return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
    }

    void checkArity(const Symbol* s, size_t given, SourceLoc loc, bool allowInferred) {
        if (s->kind == Symbol::Kind::Variant) return; // variants use the enum's arguments
        size_t want = genericCount(s);
        if (given == want || (allowInferred && given == 0)) return;
        error(loc, "'" + s->name + "' takes " + plural(want, "type argument") + ", but " +
                       std::to_string(given) + (given == 1 ? " was" : " were") + " given");
    }

    static std::vector<Seg> splitName(const std::string& name, SourceLoc loc) {
        std::vector<Seg> segs;
        std::string cur;
        for (char c : name) {
            if (c == '.') {
                segs.push_back({cur, loc});
                cur.clear();
            } else {
                cur += c;
            }
        }
        segs.push_back({cur, loc});
        return segs;
    }

    enum class TypeCtx { Normal, Constraint };

    void resolveType(TypeRef& t, TypeCtx ctx = TypeCtx::Normal) {
        if (t.kind == TypeRef::Kind::Function) {
            for (auto& a : t.args) resolveType(*a);
            if (t.elem) resolveType(*t.elem);
            return;
        }
        if (t.kind == TypeRef::Kind::Array) {
            resolveType(*t.elem);
            if (t.size) resolveExpr(*t.size);
            return;
        }
        for (auto& a : t.args) resolveType(*a);
        auto segs = splitName(t.name, t.loc);
        PathResult r = resolvePath(segs);
        if (!r.sym) return;
        if (r.used != segs.size()) {
            error(t.loc, "'" + t.name + "' is not a type");
            return;
        }
        Symbol* s = r.sym;
        using K = Symbol::Kind;
        bool isType = s->kind == K::Type || s->kind == K::Class || s->kind == K::Enum ||
                      s->kind == K::Generic || s == ptr_ || s == selfType_;
        if (ctx == TypeCtx::Constraint) {
            if (s->kind != K::Interface) {
                error(t.loc, "'" + t.name + "' is a " + std::string(symbolKindName(s->kind)) +
                                 "; a generic constraint must be an interface");
                return;
            }
        } else if (s->kind == K::Interface) {
            error(t.loc, "'" + t.name + "' is an interface; interfaces can only be used as generic "
                                        "constraints, e.g. <T: " + t.name + ">");
            return;
        } else if (!isType) {
            error(t.loc, "'" + t.name + "' is a " + std::string(symbolKindName(s->kind)) + ", not a type");
            return;
        }
        checkArity(s, t.args.size(), t.loc, /*allowInferred=*/false);
        t.sym = s;
    }

    // Declares generic parameters into the current scope, then resolves their constraints.
    void declareGenerics(std::vector<GenericParam>& gs) {
        for (auto& g : gs) {
            Symbol* s = prog_.newSymbol(Symbol::Kind::Generic, g.name, g.loc, mod_);
            s->genericParam = &g;
            g.sym = s;
            declare(s);
        }
        for (auto& g : gs)
            for (auto& c : g.constraints) resolveType(*c, TypeCtx::Constraint);
    }

    // -------------------------------------------------------------- expressions

    void checkValue(const Symbol* s, SourceLoc loc) {
        using K = Symbol::Kind;
        if (s->kind == K::Interface)
            error(loc, "'" + s->name + "' is an interface, not a value");
        else if (s->kind == K::Enum)
            error(loc, "'" + s->name + "' is an enum; use one of its variants, e.g. " + s->name + "." +
                           (s->members.empty() ? "Variant" : s->members.begin()->first));
        else if (s->kind == K::Module)
            error(loc, "'" + s->name + "' is a module; write " + s->name + ".<name>");
        else if (s == selfType_)
            error(loc, "'Self' is a type; use 'self' for the current object");
        else if (s == ptr_)
            {} // ptr<T>(x) is a pointer cast
    }

    void resolveExpr(Expr& e) {
        switch (e.kind) {
        case ExprKind::IntLit:
        case ExprKind::FloatLit:
        case ExprKind::StringLit:
        case ExprKind::CharLit:
        case ExprKind::BoolLit:
        case ExprKind::NullLit: return;
        case ExprKind::Self:
            if (!selfAllowed_) error(e.loc, "'self' is only available inside class methods");
            return;
        case ExprKind::Ident: {
            auto& id = static_cast<IdentExpr&>(e);
            for (auto& a : id.typeArgs) resolveType(*a);
            PathResult r = resolvePath({{id.name, id.loc}});
            if (!r.sym) return;
            checkValue(r.sym, id.loc);
            if (!id.typeArgs.empty()) checkArity(r.sym, id.typeArgs.size(), id.loc, true);
            id.sym = r.sym;
            return;
        }
        case ExprKind::Member: resolveMember(static_cast<MemberExpr&>(e)); return;
        case ExprKind::Unary: resolveExpr(*static_cast<UnaryExpr&>(e).operand); return;
        case ExprKind::Binary: {
            auto& b = static_cast<BinaryExpr&>(e);
            resolveExpr(*b.lhs);
            resolveExpr(*b.rhs);
            return;
        }
        case ExprKind::Call: {
            auto& c = static_cast<CallExpr&>(e);
            resolveExpr(*c.callee);
            for (auto& a : c.args) resolveExpr(*a);
            return;
        }
        case ExprKind::Index: {
            auto& i = static_cast<IndexExpr&>(e);
            resolveExpr(*i.object);
            resolveExpr(*i.index);
            return;
        }
        case ExprKind::Try: resolveExpr(*static_cast<TryExpr&>(e).operand); return;
        case ExprKind::Alloc: {
            auto& a = static_cast<AllocExpr&>(e);
            if (a.arrayType) resolveType(*a.arrayType);
            if (a.init) resolveExpr(*a.init);
            return;
        }
        case ExprKind::ArrayLit:
            for (auto& x : static_cast<ArrayLitExpr&>(e).elems) resolveExpr(*x);
            return;
        case ExprKind::Lambda: {
            auto& fn = *static_cast<LambdaExpr&>(e).fn;
            size_t savedBoundary = captureBoundary_;
            captureBoundary_ = scopes_.size();
            resolveFunction(fn, false);
            captureBoundary_ = savedBoundary;
            return;
        }
        case ExprKind::Match: {
            auto& m = static_cast<MatchExpr&>(e);
            resolveExpr(*m.subject);
            for (auto& arm : m.arms) {
                pushScope();
                resolvePattern(*arm.pattern);
                if (arm.block) resolveBlock(*arm.block);
                if (arm.expr) resolveExpr(*arm.expr);
                popScope();
            }
            return;
        }
        }
    }

    // `a.b.c`: if `a` is a plain identifier, the chain may be a qualified name
    // (module path / enum variant). Whatever is left is runtime member access.
    void resolveMember(MemberExpr& m) {
        std::vector<MemberExpr*> chain; // innermost first
        Expr* base = &m;
        while (base->kind == ExprKind::Member) {
            chain.push_back(static_cast<MemberExpr*>(base));
            base = static_cast<MemberExpr*>(base)->object.get();
        }
        std::reverse(chain.begin(), chain.end());
        auto* id = base->kind == ExprKind::Ident ? static_cast<IdentExpr*>(base) : nullptr;
        if (!id || !id->typeArgs.empty()) {
            resolveExpr(*base);
            return;
        }
        std::vector<Seg> segs{{id->name, id->loc}};
        for (MemberExpr* c : chain) segs.push_back({c->member, c->loc});
        PathResult r = resolvePath(segs);
        if (!r.sym) return;
        if (r.used == 1)
            id->sym = r.sym;
        else
            chain[r.used - 2]->sym = r.sym;
        if (r.sym->kind == Symbol::Kind::Enum && r.used == segs.size()) checkValue(r.sym, id->loc);
        if (r.sym->kind == Symbol::Kind::Interface) checkValue(r.sym, id->loc);
    }

    void resolvePattern(Pattern& p) {
        switch (p.kind) {
        case Pattern::Kind::Wildcard: return;
        case Pattern::Kind::Literal: resolveExpr(*p.literal); return;
        case Pattern::Kind::Binding: {
            // A bare name that matches a visible enum variant is that variant;
            // otherwise it introduces a new binding.
            Symbol* s = lookup(p.name, p.loc);
            if (s && s->kind == Symbol::Kind::Variant) {
                p.kind = Pattern::Kind::Variant;
                p.sym = s;
                checkVariantFields(p);
                return;
            }
            p.sym = declareLocal(p.name, p.loc, false);
            return;
        }
        case Pattern::Kind::Variant: {
            for (auto& f : p.fields) resolvePattern(*f);
            PathResult r = resolvePath(splitName(p.name, p.loc));
            if (!r.sym) return;
            if (r.sym->kind != Symbol::Kind::Variant) {
                error(p.loc, "'" + p.name + "' is a " + std::string(symbolKindName(r.sym->kind)) +
                                 ", not an enum variant");
                return;
            }
            p.sym = r.sym;
            checkVariantFields(p);
            return;
        }
        }
    }

    void checkVariantFields(const Pattern& p) {
        size_t want = p.sym->variant->payload.size();
        if (p.fields.size() == want) return;
        error(p.loc, "variant '" + p.sym->name + "' has " + plural(want, "field") + ", but the pattern has " +
                         std::to_string(p.fields.size()));
    }

    // -------------------------------------------------------------- statements

    void resolveBlock(Block& b) {
        pushScope();
        for (auto& s : b.stmts) resolveStmt(*s);
        popScope();
    }

    void resolveStmt(Stmt& s) {
        switch (s.kind) {
        case StmtKind::Block: resolveBlock(static_cast<Block&>(s)); return;
        case StmtKind::Let: {
            auto& l = static_cast<LetStmt&>(s);
            if (l.type) resolveType(*l.type);
            if (l.init) resolveExpr(*l.init); // before declaring: `let x = x` sees the outer x
            l.sym = declareLocal(l.name, l.loc, l.mutable_);
            return;
        }
        case StmtKind::Assign: {
            auto& a = static_cast<AssignStmt&>(s);
            resolveExpr(*a.target);
            resolveExpr(*a.value);
            if (a.target->kind == ExprKind::Ident) {
                const Symbol* t = static_cast<IdentExpr&>(*a.target).sym;
                if (t && t->kind == Symbol::Kind::Local && !t->isMutable)
                    error(a.target->loc, "cannot assign to '" + t->name +
                                             "': it is immutable. Declare it with 'var' to allow changes");
                else if (t && t->kind != Symbol::Kind::Local)
                    error(a.target->loc, "cannot assign to " + std::string(symbolKindName(t->kind)) + " '" +
                                             t->name + "'");
            }
            return;
        }
        case StmtKind::Expr: resolveExpr(*static_cast<ExprStmt&>(s).expr); return;
        case StmtKind::If: {
            auto& i = static_cast<IfStmt&>(s);
            resolveExpr(*i.cond);
            resolveBlock(*i.then);
            if (i.otherwise) resolveStmt(*i.otherwise);
            return;
        }
        case StmtKind::While: {
            auto& w = static_cast<WhileStmt&>(s);
            resolveExpr(*w.cond);
            resolveBlock(*w.body);
            return;
        }
        case StmtKind::For: {
            auto& f = static_cast<ForStmt&>(s);
            resolveExpr(*f.iterable);
            pushScope();
            f.sym = declareLocal(f.var, f.loc, false);
            resolveBlock(*f.body);
            popScope();
            return;
        }
        case StmtKind::Return: {
            auto& r = static_cast<ReturnStmt&>(s);
            if (r.value) resolveExpr(*r.value);
            return;
        }
        case StmtKind::Free: resolveExpr(*static_cast<FreeStmt&>(s).target); return;
        case StmtKind::Break:
        case StmtKind::Continue:
        case StmtKind::Asm: return;
        }
    }

    // -------------------------------------------------------------- declarations

    void resolveFunction(FunctionDecl& f, bool isMethod) {
        bool savedSelf = selfAllowed_;
        selfAllowed_ = isMethod && !f.isOperator;
        pushScope();
        declareGenerics(f.generics);
        for (auto& p : f.params) {
            resolveType(*p.type);
            p.sym = declareLocal(p.name, p.loc, p.isVar);
        }
        if (f.returnType) resolveType(*f.returnType);
        if (f.body) {
            // The body shares the parameter scope's lifetime but gets its own
            // scope, so `let a` inside may shadow a parameter `a`.
            resolveBlock(*f.body);
        }
        popScope();
        selfAllowed_ = savedSelf;
    }

    void checkMemberNames(const std::vector<Field>& fields,
                          const std::vector<std::unique_ptr<FunctionDecl>>& fns, const std::string& owner) {
        std::map<std::string, SourceLoc> seen;
        auto add = [&](const std::string& name, SourceLoc loc) {
            auto [it, inserted] = seen.emplace(name, loc);
            if (!inserted)
                error(loc, "'" + name + "' is already a member of '" + owner + "' (line " +
                               std::to_string(it->second.line) + ")");
        };
        for (const auto& f : fields) add(f.name, f.loc);
        for (const auto& fn : fns)
            if (!fn->isOperator) add(fn->name, fn->loc);
    }

    void resolveDecl(Decl& d) {
        switch (d.kind) {
        case DeclKind::Use:
        case DeclKind::Include: return;
        case DeclKind::Function: {
            resolveFunction(static_cast<FunctionDecl&>(d), false);
            return;
        }
        case DeclKind::Type: {
            auto& t = static_cast<TypeDecl&>(d);
            pushScope();
            scopes_.back()["Self"] = selfType_;
            declareGenerics(t.generics);
            if (t.aliasOf) resolveType(*t.aliasOf);
            for (auto& f : t.fields) resolveType(*f.type);
            for (auto& op : t.operators) resolveFunction(*op, false);
            checkMemberNames(t.fields, {}, t.name);
            popScope();
            return;
        }
        case DeclKind::Class: {
            auto& c = static_cast<ClassDecl&>(d);
            pushScope();
            scopes_.back()["Self"] = selfType_;
            declareGenerics(c.generics);
            for (auto& f : c.fields) resolveType(*f.type);
            for (auto& m : c.methods) resolveFunction(*m, true);
            checkMemberNames(c.fields, c.methods, c.name);
            popScope();
            return;
        }
        case DeclKind::Enum: {
            auto& e = static_cast<EnumDecl&>(d);
            pushScope();
            declareGenerics(e.generics);
            for (auto& v : e.variants)
                for (auto& p : v.payload) resolveType(*p);
            popScope();
            return;
        }
        case DeclKind::Interface: {
            auto& i = static_cast<InterfaceDecl&>(d);
            pushScope();
            scopes_.back()["Self"] = selfType_;
            declareGenerics(i.generics);
            for (auto& m : i.members) resolveFunction(*m, false);
            popScope();
            return;
        }
        }
    }
};

} // namespace

void resolveProgram(Program& program, Diagnostics& diags) { Resolver(program, diags).run(); }

} // namespace h2
