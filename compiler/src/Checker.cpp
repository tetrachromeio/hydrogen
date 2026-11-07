// Type checker.
//
// Bidirectional: checkExpr(e, expected) uses the expected type, when known,
// to decide literal types and generic arguments (`let x: u8 = 200`,
// `return Err(e)`). Integer and float literals stay "untyped" until something
// fixes their type; with nothing to go on they become int / float.
//
// Operators are ordinary declarations found on the operand types
// (`new operator +` inside a `new type` or `new class`). Derived forms:
//   a != b  is  !(a == b)       a > b   is  b < a
//   a <= b  is  !(b < a)        a >= b  is  !(a < b)

#include "hydrogen/Checker.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>

namespace h2 {
namespace {

using Subst = std::map<const Symbol*, const Type*>;
using K = Type::Kind;

struct Sig {
    std::vector<const Type*> params;
    const Type* ret = nullptr;
};

struct Scalar {
    enum Kind { None, Int, Float } kind = None;
    unsigned bits = 0;
    bool isSigned = false;
};

std::string q(const std::string& s) { return "'" + s + "'"; }

// Parses an integer literal's digits (decimal, 0x, 0b, 0o). Returns false on overflow.
bool parseIntLiteral(const std::string& text, uint64_t& out) {
    int base = 10;
    size_t i = 0;
    if (text.size() > 2 && text[0] == '0') {
        if (text[1] == 'x') base = 16, i = 2;
        else if (text[1] == 'b') base = 2, i = 2;
        else if (text[1] == 'o') base = 8, i = 2;
    }
    out = 0;
    for (; i < text.size(); ++i) {
        char c = text[i];
        unsigned d = c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
        if (out > (UINT64_MAX - d) / base) return false;
        out = out * base + d;
    }
    return true;
}

class Checker {
public:
    Checker(Program& p, TypeContext& tc, Diagnostics& d, std::vector<LocalTypeNote>* notes)
        : prog_(p), tc_(tc), diags_(d), notes_(notes) {}

    void run() {
        for (auto& m : prog_.modules)
            for (auto& [name, s] : m->defs) declSym_[s->decl] = s;
        findLangItems();
        for (auto& m : prog_.modules) {
            mod_ = m.get();
            for (auto& f : m->files) {
                file_ = f.get();
                for (auto& d : f->ast.decls) checkDecl(*d);
            }
        }
    }

private:
    Program& prog_;
    TypeContext& tc_;
    Diagnostics& diags_;
    std::vector<LocalTypeNote>* notes_;

    std::map<const Decl*, const Symbol*> declSym_;
    std::map<const Symbol*, const Type*> localTypes_;
    std::map<std::pair<const FunctionDecl*, const Type*>, Sig> sigCache_; // keyed by (fn, Self)

    // What each call (or operator use) invokes, for the move checker.
    struct CallInfo {
        enum Kind { Fn, ConsumeAll, BorrowAll } kind = BorrowAll;
        const FunctionDecl* fn = nullptr; // Fn: the callee, whose `own` params consume
        Subst subst;                      // generic bindings at this call
    };
    std::map<const Expr*, CallInfo> calls_;
    std::set<const Expr*> viewCoerced_; // string expressions used as str (a borrow, not a move)
    struct LambdaSite {
        FunctionDecl* fn;
        ModuleInfo* mod;
        SourceFile* file;
    };
    std::vector<LambdaSite> lambdas_;

    std::map<const Expr*, const Type*> dynCoerced_; // ptr<T> used as any I: the concrete T

    void noteCoercion(const Expr& e, const Type* from, const Type* to) {
        if (from == lang_.stringT && to == lang_.strT) viewCoerced_.insert(&e);
        if (from->kind == K::Ptr && to->kind == K::Dyn) dynCoerced_[&e] = from->elem();
    }

    struct Lang {
        const Type *boolT, *charT, *strT, *stringT, *intT, *uintT, *floatT;
        const Symbol *result = nullptr, *option = nullptr;
    } lang_{};

    ModuleInfo* mod_ = nullptr;
    SourceFile* file_ = nullptr;
    Pattern wildcard_{Pattern::Kind::Wildcard, {}, {}, nullptr, {}, false, nullptr};
    const Type* retType_ = nullptr;   // current function's return type
    const Type* selfType_ = nullptr;  // type of `self` / `Self`
    int loopDepth_ = 0;
    int aliasDepth_ = 0;
    bool selfMutable_ = false;              // inside init, drop or a `mut` method
    const FunctionDecl* currentFn_ = nullptr;

    void error(SourceLoc loc, const std::string& msg) { diags_.error(file_->displayFile, loc, msg); }
    static std::string str(const Type* t) { return TypeContext::str(t); }

    // -------------------------------------------------------------- Sendable

    const Symbol* sendableSym_ = nullptr;

    // Can a value of this type move to another thread? Not if it points at memory
    // that the sending thread could still use (raw pointers, str views, any I,
    // slices). Classes with `drop` own what they point to, so they qualify when
    // their contents do (string, List<T>, Map<K, V>, ...).
    bool sendable(const Type* t, std::string& why, int depth) {
        if (depth > 16) return true;
        switch (t->kind) {
        case K::Ptr: why = "it contains a raw pointer (" + str(t) + ")"; return false;
        case K::Dyn: why = "it contains a borrowed reference (" + str(t) + ")"; return false;
        case K::Slice: why = "it contains a slice, which borrows memory (" + str(t) + ")"; return false;
        case K::Array:
        case K::Range: return sendable(t->elem(), why, depth + 1);
        case K::Generic:
            if (t->sym->genericParam)
                for (const auto& c : t->sym->genericParam->constraints)
                    if (c->sym == sendableSym_) return true;
            why = str(t) + " isn't known to be Sendable; add the constraint <" + str(t) + ": Sendable>";
            return false;
        case K::Named: break;
        default: return true;
        }
        if (t == lang_.strT) {
            why = "str is a borrowed view; send an owned string instead (string(text))";
            return false;
        }
        Subst s = substFor(t);
        const Type* selfT = selfTypeOf(t->sym);
        bool owns = false;
        if (t->sym->kind == Symbol::Kind::Class)
            for (const auto& m : declAs<ClassDecl>(t->sym)->methods)
                owns = owns || (!m->isOperator && m->name == "drop");
        for (const Type* a : t->args)
            if (!sendable(a, why, depth + 1)) return false;
        auto fieldsOk = [&](const std::vector<Field>& fields) {
            for (const auto& f : fields) {
                const Type* ft = subst(convert(*f.type, selfT), s);
                if (owns && ft->kind == K::Ptr) continue; // owned memory, freed in drop
                if (!sendable(ft, why, depth + 1)) {
                    why = "field " + q(f.name) + " of " + t->sym->name + ": " + why;
                    return false;
                }
            }
            return true;
        };
        if (t->sym->kind == Symbol::Kind::Class) return fieldsOk(declAs<ClassDecl>(t->sym)->fields);
        if (t->sym->kind == Symbol::Kind::Type) return fieldsOk(declAs<TypeDecl>(t->sym)->fields);
        if (t->sym->kind == Symbol::Kind::Enum)
            for (const auto& v : declAs<EnumDecl>(t->sym)->variants)
                for (const auto& p : v.payload)
                    if (!sendable(subst(convert(*p, nullptr), s), why, depth + 1)) return false;
        return true;
    }

    // -------------------------------------------------------------- language items

    void findLangItems() {
        ModuleInfo* atomic = nullptr;
        for (auto& m : prog_.modules)
            if (m->path == "Atomic") atomic = m.get();
        auto get = [&](const char* name) -> const Type* {
            if (!atomic) return tc_.error();
            auto it = atomic->defs.find(name);
            if (it == atomic->defs.end()) return tc_.error();
            return typeOfTypeSymbol(it->second, {});
        };
        lang_.boolT = get("bool");
        lang_.charT = get("char");
        lang_.strT = get("str");
        // The owned string lives in Elemental (Atomic is freestanding).
        lang_.stringT = tc_.error();
        for (auto& m : prog_.modules)
            if (m->path == "Elemental")
                if (auto it = m->defs.find("string"); it != m->defs.end())
                    lang_.stringT = typeOfTypeSymbol(it->second, {});
        lang_.intT = get("int");
        lang_.uintT = get("uint");
        lang_.floatT = get("float");
        if (atomic) {
            if (auto it = atomic->defs.find("Sendable"); it != atomic->defs.end()) sendableSym_ = it->second;
            if (auto it = atomic->defs.find("Result"); it != atomic->defs.end()) lang_.result = it->second;
            if (auto it = atomic->defs.find("Option"); it != atomic->defs.end()) lang_.option = it->second;
        }
    }

    // -------------------------------------------------------------- type conversion

    template <class D>
    static const D* declAs(const Symbol* s) {
        return static_cast<const D*>(s->decl);
    }

    const std::vector<GenericParam>* genericsOf(const Symbol* s) {
        if (!s || !s->decl) return nullptr;
        switch (s->decl->kind) {
        case DeclKind::Type: return &declAs<TypeDecl>(s)->generics;
        case DeclKind::Class: return &declAs<ClassDecl>(s)->generics;
        case DeclKind::Enum: return &declAs<EnumDecl>(s)->generics;
        case DeclKind::Interface: return &declAs<InterfaceDecl>(s)->generics;
        case DeclKind::Function: return &declAs<FunctionDecl>(s)->generics;
        default: return nullptr;
        }
    }

    // Maps a named type's generic parameters to its arguments.
    Subst substFor(const Type* t) {
        Subst s;
        if (!t || t->kind != K::Named) return s;
        const auto* gs = genericsOf(t->sym);
        if (!gs) return s;
        for (size_t i = 0; i < gs->size() && i < t->args.size(); ++i) s[(*gs)[i].sym] = t->args[i];
        return s;
    }

    // The type a type/class/enum symbol stands for (following `=` aliases).
    const Type* typeOfTypeSymbol(const Symbol* s, std::vector<const Type*> args) {
        if (s->kind == Symbol::Kind::Type) {
            const auto* td = declAs<TypeDecl>(s);
            if (td->base == TypeDecl::Base::Alias) {
                if (aliasDepth_ > 32) return tc_.error();
                ++aliasDepth_;
                const Type* t = convert(*td->aliasOf, nullptr);
                --aliasDepth_;
                return t;
            }
        }
        return tc_.named(s, std::move(args));
    }

    uint64_t constantSize(const Expr& e) {
        if (e.kind == ExprKind::IntLit) {
            uint64_t v;
            if (parseIntLiteral(static_cast<const LiteralExpr&>(e).value, v)) return v;
        }
        error(e.loc, "array size must be an integer literal");
        return 0;
    }

    const Type* convert(const TypeRef& t, const Type* selfT) {
        if (t.kind == TypeRef::Kind::Any) {
            if (!t.sym) return tc_.error();
            if (!dynUsable(t.sym, t.loc)) return tc_.error();
            return tc_.dyn(t.sym);
        }
        if (t.kind == TypeRef::Kind::Function) {
            std::vector<const Type*> ps;
            for (const auto& a : t.args) ps.push_back(convert(*a, selfT));
            return tc_.function(t.elem ? convert(*t.elem, selfT) : tc_.voidT(), ps);
        }
        if (t.kind == TypeRef::Kind::Array) {
            const Type* elem = convert(*t.elem, selfT);
            if (!t.size) return tc_.slice(elem);
            return tc_.array(elem, constantSize(*t.size));
        }
        const Symbol* s = t.sym;
        if (!s) return tc_.error();
        std::vector<const Type*> args;
        for (const auto& a : t.args) args.push_back(convert(*a, selfT));
        switch (s->kind) {
        case Symbol::Kind::Builtin:
            if (s->name == "ptr") return args.size() == 1 ? tc_.ptr(args[0]) : tc_.error();
            if (s->name == "Self") return selfT ? selfT : tc_.error();
            return tc_.error();
        case Symbol::Kind::Generic: return tc_.generic(s);
        case Symbol::Kind::Type:
        case Symbol::Kind::Class:
        case Symbol::Kind::Enum: {
            const auto* gs = genericsOf(s);
            if (gs && gs->size() != args.size()) return tc_.error(); // reported by the resolver
            return typeOfTypeSymbol(s, std::move(args));
        }
        default: return tc_.error();
        }
    }

    const Type* subst(const Type* t, const Subst& s) {
        if (s.empty()) return t;
        switch (t->kind) {
        case K::Generic: {
            auto it = s.find(t->sym);
            return it != s.end() && it->second ? it->second : t;
        }
        case K::Named: {
            std::vector<const Type*> args;
            for (const Type* a : t->args) args.push_back(subst(a, s));
            return tc_.named(t->sym, std::move(args));
        }
        case K::Ptr: return tc_.ptr(subst(t->elem(), s));
        case K::Array: return tc_.array(subst(t->elem(), s), t->size);
        case K::Slice: return tc_.slice(subst(t->elem(), s));
        case K::Range: return tc_.range(subst(t->elem(), s));
        case K::Function: {
            std::vector<const Type*> ps;
            for (size_t i = 1; i < t->args.size(); ++i) ps.push_back(subst(t->args[i], s));
            return tc_.function(subst(t->args[0], s), ps);
        }
        default: return t;
        }
    }

    // The self type of a class/type/interface: Name<T, U> with its own parameters.
    const Type* selfTypeOf(const Symbol* s) {
        std::vector<const Type*> args;
        if (const auto* gs = genericsOf(s))
            for (const auto& g : *gs) args.push_back(tc_.generic(g.sym));
        return tc_.named(s, std::move(args));
    }

    // Signature of a function, relative to its owner's self type (if any).
    const Sig& sigOf(const FunctionDecl& f, const Type* ownerSelf) {
        auto key = std::make_pair(&f, ownerSelf);
        auto it = sigCache_.find(key);
        if (it != sigCache_.end()) return it->second;
        Sig sig;
        for (const auto& p : f.params) sig.params.push_back(convert(*p.type, ownerSelf));
        sig.ret = f.returnType ? convert(*f.returnType, ownerSelf) : tc_.voidT();
        return sigCache_[key] = std::move(sig);
    }

    // -------------------------------------------------------------- classification

    Scalar scalarOf(const Type* t, int depth = 0) {
        if (t->kind != K::Named || t->sym->kind != Symbol::Kind::Type || depth > 32) return {};
        const auto* td = declAs<TypeDecl>(t->sym);
        switch (td->base) {
        case TypeDecl::Base::Bits: return {Scalar::Int, td->bits, td->isSigned};
        case TypeDecl::Base::Float: return {Scalar::Float, td->bits, true};
        case TypeDecl::Base::Distinct: return scalarOf(convert(*td->aliasOf, nullptr), depth + 1);
        default: return {};
        }
    }
    bool isNumeric(const Type* t) {
        return t != lang_.boolT && t != lang_.charT && scalarOf(t).kind != Scalar::None;
    }
    bool isIntegerType(const Type* t) {
        return t != lang_.boolT && t != lang_.charT && scalarOf(t).kind == Scalar::Int;
    }
    bool isFloatType(const Type* t) { return scalarOf(t).kind == Scalar::Float; }

    bool assignable(const Type* from, const Type* to) {
        if (from == to || from->isError() || to->isError()) return true;
        if (from->kind == K::Ptr && to->kind == K::Dyn) return implements(from->elem(), to->sym);
        if (from->kind == K::IntLit) return isNumeric(to);
        if (from->kind == K::FloatLit) return isFloatType(to);
        if (from->kind == K::Array && to->kind == K::Slice) return from->elem() == to->elem();
        if (from == lang_.stringT && to == lang_.strT) return true; // borrowing a view is free
        return false;
    }

    // Gives untyped literals in `e` the type `t`, checking that values fit.
    void setLiteralType(Expr& e, const Type* t) {
        if (!e.type || !e.type->isLiteral()) return;
        e.type = t;
        if (e.kind == ExprKind::IntLit) {
            checkIntFits(static_cast<LiteralExpr&>(e), false, t);
        } else if (e.kind == ExprKind::Unary) {
            auto& u = static_cast<UnaryExpr&>(e);
            if (u.op == "-" && u.operand->kind == ExprKind::IntLit && u.operand->type && u.operand->type->isLiteral()) {
                u.operand->type = t;
                checkIntFits(static_cast<LiteralExpr&>(*u.operand), true, t);
            } else {
                setLiteralType(*u.operand, t);
            }
        } else if (e.kind == ExprKind::Binary) {
            auto& b = static_cast<BinaryExpr&>(e);
            setLiteralType(*b.lhs, t);
            setLiteralType(*b.rhs, t);
        }
    }

    void checkIntFits(const LiteralExpr& lit, bool negative, const Type* t) {
        Scalar s = scalarOf(t);
        if (s.kind != Scalar::Int || s.bits == 0 || s.bits > 64) return;
        uint64_t v;
        if (!parseIntLiteral(lit.value, v)) return; // reported when the literal was checked
        bool fits;
        if (s.isSigned) {
            uint64_t limit = s.bits == 64 ? (uint64_t(1) << 63) : (uint64_t(1) << (s.bits - 1));
            fits = negative ? v <= limit : v < limit;
        } else {
            uint64_t max = s.bits == 64 ? UINT64_MAX : (uint64_t(1) << s.bits) - 1;
            fits = !negative ? v <= max : v == 0;
        }
        if (!fits)
            error(lit.loc, std::string(negative ? "-" : "") + lit.value + " does not fit in " + str(t) + " (" +
                               (s.isSigned ? "signed" : "unsigned") + " " + std::to_string(s.bits) + "-bit)");
    }

    const Type* defaultLiteral(const Type* t) {
        if (t->kind == K::IntLit) return lang_.intT;
        if (t->kind == K::FloatLit) return lang_.floatT;
        return t;
    }

    // Can `any I` dispatch every member at run time? (No operators, no Self, no generics.)
    std::map<const Symbol*, bool> dynOk_;
    bool dynUsable(const Symbol* iface, SourceLoc loc) {
        auto it = dynOk_.find(iface);
        if (it != dynOk_.end()) return it->second;
        bool ok = true;
        for (const auto& m : declAs<InterfaceDecl>(iface)->members) {
            std::string why;
            if (m->isOperator) why = "operator " + q(m->name) + " needs both values to have the same type";
            else if (!m->generics.empty()) why = q(m->name) + " is generic";
            else {
                const Sig& sig = sigOf(*m, tc_.generic(iface)); // Self -> a marker type
                for (const Type* p : sig.params)
                    if (p == tc_.generic(iface)) why = q(m->name) + " takes Self";
                if (sig.ret == tc_.generic(iface)) why = q(m->name) + " returns Self";
            }
            if (!why.empty()) {
                error(loc, "'any " + iface->name + "' isn't possible: " + why +
                               ", which can't be dispatched at run time");
                ok = false;
                break;
            }
        }
        return dynOk_[iface] = ok;
    }

    // Does `t` provide every method of the interface? (Quiet; used for `any I`.)
    bool implements(const Type* t, const Symbol* iface) {
        if (t->kind != K::Named || t->sym->kind != Symbol::Kind::Class) return false;
        const auto* cd = declAs<ClassDecl>(t->sym);
        Subst s = substFor(t);
        for (const auto& m : declAs<InterfaceDecl>(iface)->members) {
            const Sig& want = sigOf(*m, t);
            bool found = false;
            for (const auto& meth : cd->methods) {
                if (meth->isOperator || meth->name != m->name || meth->params.size() != m->params.size()) continue;
                const Sig& have = sigOf(*meth, selfTypeOf(t->sym));
                bool same = subst(have.ret, s) == want.ret;
                for (size_t i = 0; same && i < have.params.size(); ++i)
                    same = subst(have.params[i], s) == want.params[i];
                found = found || same;
            }
            if (!found) return false;
        }
        return true;
    }

    // Fixes the type of `e` when it is still an untyped literal.
    const Type* finalize(Expr& e) {
        if (e.type && e.type->isLiteral()) setLiteralType(e, defaultLiteral(e.type));
        return e.type ? e.type : tc_.error();
    }

    // Checks `e` against `target`. Returns false (after reporting) on mismatch.
    bool coerce(Expr& e, const Type* target, const std::string& what) {
        const Type* t = checkExpr(e, target);
        if (!assignable(t, target)) {
            error(e.loc, what + ": expected " + str(target) + ", found " + str(t) + conversionHint(t, target));
            return false;
        }
        if (t->isLiteral()) setLiteralType(e, target);
        noteCoercion(e, t, target);
        return true;
    }

    std::string conversionHint(const Type* from, const Type* to) {
        if (from->isError() || to->isError()) return "";
        if (to->kind == K::Dyn && from->kind == K::Ptr) {
            std::string missing;
            if (from->elem()->kind == K::Named && from->elem()->sym->kind == Symbol::Kind::Class)
                for (const auto& m : declAs<InterfaceDecl>(to->sym)->members) {
                    bool has = false;
                    for (const auto& meth : declAs<ClassDecl>(from->elem()->sym)->methods)
                        has = has || (!meth->isOperator && meth->name == m->name);
                    if (!has) missing += (missing.empty() ? "" : ", ") + m->name + "()";
                }
            return ". " + str(from->elem()) + " doesn't implement " + to->sym->name +
                   (missing.empty() ? " (a method's signature differs)" : " (missing " + missing + ")");
        }
        if (to->kind == K::Dyn) return ". Pass a pointer: &value";
        if (from == lang_.strT && to == lang_.stringT) return ". Make an owned copy with string(...)";
        bool fromNum = from->isLiteral() || scalarOf(from).kind != Scalar::None;
        if (fromNum && scalarOf(to).kind != Scalar::None && to->kind == K::Named)
            return ". Convert explicitly with " + str(to) + "(...)";
        return "";
    }

    // -------------------------------------------------------------- operators

    struct OpFound {
        const FunctionDecl* fn = nullptr;
        const Type* result = nullptr;
        std::vector<const Type*> params;
    };

    // Operator declarations of a named type (type operators, class operators).
    std::vector<const FunctionDecl*> operatorsOf(const Type* t) {
        std::vector<const FunctionDecl*> out;
        if (t->kind != K::Named) return out;
        if (t->sym->kind == Symbol::Kind::Type)
            for (const auto& op : declAs<TypeDecl>(t->sym)->operators) out.push_back(op.get());
        else if (t->sym->kind == Symbol::Kind::Class)
            for (const auto& m : declAs<ClassDecl>(t->sym)->methods)
                if (m->isOperator) out.push_back(m.get());
        return out;
    }

    // Operators available on a generic parameter through its constraints.
    std::vector<std::pair<const FunctionDecl*, Subst>> constraintOperators(const Type* t) {
        std::vector<std::pair<const FunctionDecl*, Subst>> out;
        if (t->kind != K::Generic || !t->sym->genericParam) return out;
        for (const auto& c : t->sym->genericParam->constraints) {
            if (!c->sym || c->sym->kind != Symbol::Kind::Interface) continue;
            const auto* iface = declAs<InterfaceDecl>(c->sym);
            Subst s;
            for (size_t i = 0; i < iface->generics.size() && i < c->args.size(); ++i)
                s[iface->generics[i].sym] = convert(*c->args[i], nullptr);
            for (const auto& m : iface->members)
                if (m->isOperator) out.push_back({m.get(), s});
        }
        return out;
    }

    // Finds `op` with `arity` parameters on `owner` accepting `args`.
    std::optional<OpFound> findOperatorOn(const Type* owner, const std::string& op,
                                          const std::vector<const Type*>& args) {
        auto tryFn = [&](const FunctionDecl* fn, const Subst& extra, const Type* selfT) -> std::optional<OpFound> {
            if (fn->name != op || fn->params.size() != args.size()) return std::nullopt;
            const Sig& sig = sigOf(*fn, selfT);
            Subst s = substFor(owner);
            s.insert(extra.begin(), extra.end());
            OpFound f{fn, nullptr, {}};
            for (size_t i = 0; i < args.size(); ++i) {
                const Type* p = subst(sig.params[i], s);
                if (!assignable(args[i], p)) return std::nullopt;
                f.params.push_back(p);
            }
            f.result = subst(sig.ret, s);
            return f;
        };
        if (owner->kind == K::Named) {
            const Type* selfT = selfTypeOf(owner->sym);
            for (const FunctionDecl* fn : operatorsOf(owner))
                if (auto f = tryFn(fn, {}, selfT)) return f;
        }
        if (owner->kind != K::Generic) {
            // Extension operators from this module and the modules it uses.
            for (const ModuleInfo* m : visibleModules()) {
                for (const FunctionDecl* fn : m->extOps) {
                    if (fn->name != op || fn->params.size() != args.size()) continue;
                    if (!fn->isPub && m != mod_) continue;
                    const Sig& sig = sigOf(*fn, nullptr);
                    OpFound f{fn, sig.ret, {}};
                    bool ok = sig.params.size() == args.size();
                    for (size_t i = 0; ok && i < args.size(); ++i) {
                        ok = assignable(args[i], sig.params[i]);
                        f.params.push_back(sig.params[i]);
                    }
                    // Only consider it if it's about this operand's type.
                    bool aboutOwner = std::find(sig.params.begin(), sig.params.end(), owner) != sig.params.end();
                    if (ok && aboutOwner) return f;
                }
            }
        } else {
            // Interface signatures use Self, which stands for the generic parameter.
            for (auto& [fn, s] : constraintOperators(owner))
                if (auto f = tryFn(fn, s, owner)) return f;
        }
        return std::nullopt;
    }

    // This module, plus every module it uses (following `pub use` re-exports).
    std::vector<const ModuleInfo*> visibleModules() {
        auto cached = visibleCache_.find(mod_);
        if (cached != visibleCache_.end()) return cached->second;
        std::vector<const ModuleInfo*> out{mod_};
        std::set<const ModuleInfo*> seen{mod_};
        std::vector<const ModuleInfo*> work;
        for (const auto& imp : mod_->imports)
            if (imp.target) work.push_back(imp.target);
        while (!work.empty()) {
            const ModuleInfo* m = work.back();
            work.pop_back();
            if (!seen.insert(m).second) continue;
            out.push_back(m);
            for (const auto& imp : m->imports)
                if (imp.target && imp.decl && imp.decl->isPub) work.push_back(imp.target);
        }
        return visibleCache_[mod_] = out;
    }
    std::map<const ModuleInfo*, std::vector<const ModuleInfo*>> visibleCache_;

    std::optional<OpFound> findBinary(const std::string& op, const Type* l, const Type* r) {
        if (auto f = findOperatorOn(l, op, {l, r})) return f;
        if (r != l)
            if (auto f = findOperatorOn(r, op, {l, r})) return f;
        return std::nullopt;
    }

    // -------------------------------------------------------------- expressions

    const Type* checkExpr(Expr& e, const Type* expected) {
        const Type* t = checkExprInner(e, expected);
        e.type = t ? t : tc_.error();
        return e.type;
    }

    const Type* checkExprInner(Expr& e, const Type* expected) {
        switch (e.kind) {
        case ExprKind::IntLit: {
            uint64_t v;
            if (!parseIntLiteral(static_cast<LiteralExpr&>(e).value, v)) {
                error(e.loc, "integer literal is too large (the maximum is 18446744073709551615)");
                return tc_.error();
            }
            return tc_.intLit();
        }
        case ExprKind::FloatLit: return tc_.floatLit();
        case ExprKind::StringLit: return lang_.strT;
        case ExprKind::CharLit: return lang_.charT;
        case ExprKind::BoolLit: return lang_.boolT;
        case ExprKind::NullLit:
            if (expected && expected->kind == K::Ptr) return expected;
            error(e.loc, "'null' needs a pointer type from context, e.g. let p: ptr<u8> = null");
            return tc_.error();
        case ExprKind::Self: return selfType_ ? selfType_ : tc_.error();
        case ExprKind::Ident: {
            auto& id = static_cast<IdentExpr&>(e);
            return valueOfSymbol(id.sym, id.loc, expected);
        }
        case ExprKind::Member: {
            auto& m = static_cast<MemberExpr&>(e);
            if (m.sym) return valueOfSymbol(m.sym, m.loc, expected);
            if (isQualifiedPrefix(*m.object)) return tc_.error(); // e.g. intrinsic.x without a call
            return checkField(m);
        }
        case ExprKind::Unary: return checkUnary(static_cast<UnaryExpr&>(e), expected);
        case ExprKind::Binary: return checkBinary(static_cast<BinaryExpr&>(e), expected);
        case ExprKind::Call: return checkCall(static_cast<CallExpr&>(e), expected);
        case ExprKind::Index: return checkIndex(static_cast<IndexExpr&>(e));
        case ExprKind::Try: return checkTry(static_cast<TryExpr&>(e));
        case ExprKind::Alloc: {
            auto& a = static_cast<AllocExpr&>(e);
            if (a.arrayType) {
                const Type* t = convert(*a.arrayType, selfType_);
                if (t->kind == K::Slice) {
                    error(a.loc, "alloc needs a fixed size: alloc [T; N]");
                    return tc_.error();
                }
                return t->kind == K::Array ? tc_.ptr(t->elem()) : tc_.error();
            }
            const Type* inner = expected && expected->kind == K::Ptr ? expected->elem() : nullptr;
            checkExpr(*a.init, inner);
            return tc_.ptr(finalize(*a.init));
        }
        case ExprKind::ArrayLit: return checkArrayLit(static_cast<ArrayLitExpr&>(e), expected);
        case ExprKind::Match: return checkMatch(static_cast<MatchExpr&>(e), expected);
        case ExprKind::Lambda: {
            auto& fn = *static_cast<LambdaExpr&>(e).fn;
            for (const auto& p : fn.params)
                if (p.isOwn || p.isVar) error(p.loc, "lambda parameters can't be 'own' or 'var' yet");
            int savedLoops = loopDepth_;
            loopDepth_ = 0;
            checkFunction(fn, selfType_);
            loopDepth_ = savedLoops;
            lambdas_.push_back({&fn, mod_, file_});
            const Sig& sig = sigOf(fn, selfType_);
            return tc_.function(sig.ret, sig.params);
        }
        }
        return tc_.error();
    }

    // True for `intrinsic` and other names that only make sense as call targets.
    bool isQualifiedPrefix(const Expr& e) {
        const Expr* base = &e;
        while (base->kind == ExprKind::Member) base = static_cast<const MemberExpr*>(base)->object.get();
        if (base->kind != ExprKind::Ident) return false;
        const Symbol* s = static_cast<const IdentExpr*>(base)->sym;
        return s && s->kind == Symbol::Kind::Builtin && s->name == "intrinsic";
    }

    const Type* enumTypeFor(const Symbol* variant, const Type* expected, Subst* out = nullptr) {
        const Symbol* enumSym = declSym_[variant->decl];
        Subst s;
        const auto* gs = genericsOf(enumSym);
        if (expected && expected->kind == K::Named && expected->sym == enumSym && expected->args.size() == gs->size())
            for (size_t i = 0; i < gs->size(); ++i) s[(*gs)[i].sym] = expected->args[i];
        if (out) *out = s;
        std::vector<const Type*> args;
        for (const auto& g : *gs) {
            auto it = s.find(g.sym);
            if (it == s.end() || !it->second) return nullptr;
            args.push_back(it->second);
        }
        return tc_.named(enumSym, std::move(args));
    }

    const Type* valueOfSymbol(const Symbol* s, SourceLoc loc, const Type* expected) {
        if (!s) return tc_.error();
        switch (s->kind) {
        case Symbol::Kind::Local: {
            auto it = localTypes_.find(s);
            return it != localTypes_.end() ? it->second : tc_.error();
        }
        case Symbol::Kind::Variant: {
            if (!s->variant->payload.empty()) {
                error(loc, q(s->name) + " holds a value; write " + s->name + "(...)");
                return tc_.error();
            }
            const Type* t = enumTypeFor(s, expected);
            if (!t) {
                const Symbol* e = declSym_[s->decl];
                error(loc, "cannot tell which " + e->name + " this " + q(s->name) +
                               " is; add a type annotation, e.g. let x: " + e->name + "<...> = " + s->name);
                return tc_.error();
            }
            return t;
        }
        case Symbol::Kind::Const: return constType(s);
        case Symbol::Kind::Function: {
            const auto* fn = declAs<FunctionDecl>(s);
            if (!fn->generics.empty()) {
                error(loc, q(s->name) + " is generic, so it can't be used as a value directly; wrap it in a lambda "
                               "with concrete types");
                return tc_.error();
            }
            for (const auto& p : fn->params)
                if (p.isOwn || p.isVar) {
                    error(loc, q(s->name) + " has 'own' or 'var' parameters, so it can't be used as a value yet");
                    return tc_.error();
                }
            const Sig& sig = sigOf(*fn, nullptr);
            return tc_.function(sig.ret, sig.params);
        }
        case Symbol::Kind::Type:
        case Symbol::Kind::Class:
            error(loc, q(s->name) + " is a type; to make a value, call it: " + s->name + "(...)");
            return tc_.error();
        case Symbol::Kind::Generic:
            error(loc, q(s->name) + " is a generic type, not a value");
            return tc_.error();
        default: return tc_.error(); // already reported by the resolver
        }
    }

    // Runtime member access: obj.field (auto-dereferences one pointer).
    const Type* checkField(MemberExpr& m) {
        checkExpr(*m.object, nullptr);
        const Type* obj = finalize(*m.object);
        if (obj->isError()) return obj;
        if (obj->kind == K::Ptr) obj = obj->elem();
        if ((obj->kind == K::Array || obj->kind == K::Slice) && m.member == "len") return lang_.uintT;
        if (obj->kind == K::Named) {
            Subst s = substFor(obj);
            const Type* selfT = selfTypeOf(obj->sym);
            const std::vector<Field>* fields = nullptr;
            const ClassDecl* cls = nullptr;
            if (obj->sym->kind == Symbol::Kind::Class) {
                cls = declAs<ClassDecl>(obj->sym);
                fields = &cls->fields;
            } else if (obj->sym->kind == Symbol::Kind::Type) {
                fields = &declAs<TypeDecl>(obj->sym)->fields;
            }
            if (fields)
                for (const auto& f : *fields)
                    if (f.name == m.member) {
                        checkMemberAccess(obj->sym, f.isPub, "field", f.name, m.loc);
                        return subst(convert(*f.type, selfT), s);
                    }
            if (cls)
                for (const auto& fn : cls->methods)
                    if (!fn->isOperator && fn->name == m.member) {
                        error(m.loc, q(m.member) + " is a method; call it: ." + m.member + "(...)");
                        return tc_.error();
                    }
        }
        if (obj->kind == K::Generic)
            error(m.loc, "cannot access " + q(m.member) + " on generic type " + str(obj) +
                             ": generic code can only use what its constraints provide");
        else
            error(m.loc, str(obj) + " has no field " + q(m.member));
        return tc_.error();
    }

    void checkMemberAccess(const Symbol* owner, bool isPub, const char* what, const std::string& name,
                           SourceLoc loc) {
        if (isPub || owner->module == mod_) return;
        error(loc, std::string(what) + " " + q(name) + " of " + owner->name + " is not pub (it belongs to module " +
                       owner->module->displayName() + ")");
    }

    bool isPlace(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Ident: {
            const Symbol* s = static_cast<const IdentExpr&>(e).sym;
            return s && s->kind == Symbol::Kind::Local;
        }
        case ExprKind::Self:
        case ExprKind::Index: return true;
        case ExprKind::Member: return !static_cast<const MemberExpr&>(e).sym;
        case ExprKind::Unary: return static_cast<const UnaryExpr&>(e).op == "*";
        default: return false;
        }
    }

    // For `a.b[i].c = v`: is the place rooted in something mutable?
    // Returns the immutable local responsible, or null if mutable.
    const Symbol* immutableRoot(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Ident: {
            const Symbol* s = static_cast<const IdentExpr&>(e).sym;
            return s && s->kind == Symbol::Kind::Local && !s->isMutable ? s : nullptr;
        }
        case ExprKind::Member: {
            const auto& m = static_cast<const MemberExpr&>(e);
            if (m.object->type && m.object->type->kind == K::Ptr) return nullptr;
            return immutableRoot(*m.object);
        }
        case ExprKind::Index: {
            const auto& i = static_cast<const IndexExpr&>(e);
            if (i.object->type && (i.object->type->kind == K::Ptr || i.object->type->kind == K::Slice)) return nullptr;
            return immutableRoot(*i.object);
        }
        default: return nullptr; // self, *p
        }
    }

    const Type* checkUnary(UnaryExpr& u, const Type* expected) {
        if (u.op == "&") {
            checkExpr(*u.operand, nullptr);
            const Type* t = finalize(*u.operand);
            if (!isPlace(*u.operand)) {
                error(u.loc, "'&' needs a variable, field or element to point to");
                return tc_.error();
            }
            return tc_.ptr(t);
        }
        if (u.op == "*") {
            const Type* t = checkExpr(*u.operand, nullptr);
            if (t->isError()) return t;
            if (t->kind != K::Ptr) {
                error(u.loc, "'*' needs a pointer, found " + str(t));
                return tc_.error();
            }
            return t->elem();
        }
        const Type* t = checkExpr(*u.operand, u.op == "-" ? expected : nullptr);
        if (t->isError()) return t;
        if (u.op == "-" && t->isLiteral()) return t;
        t = finalize(*u.operand);
        if (auto f = findOperatorOn(t, u.op, {t})) return f->result;
        error(u.loc, str(t) + " has no unary operator " + q(u.op) + operatorHint(t, u.op));
        return tc_.error();
    }

    std::string operatorHint(const Type* t, const std::string& op) {
        if (t->kind == K::Named && t->sym->kind == Symbol::Kind::Type && t->sym->module == mod_)
            return ". Define it inside the type: new operator " + op + " (...)";
        if (t->kind == K::Generic) return ". Add a constraint whose interface provides it";
        return "";
    }

    const Type* checkBinary(BinaryExpr& b, const Type* expected) {
        const std::string& op = b.op;
        if (op == "&&" || op == "||") {
            coerce(*b.lhs, lang_.boolT, "left side of " + q(op));
            coerce(*b.rhs, lang_.boolT, "right side of " + q(op));
            return lang_.boolT;
        }
        bool comparison = op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
        bool arithmetic = !comparison && op != "..";
        const Type* l;
        const Type* r;
        if (b.lhs->kind == ExprKind::NullLit && b.rhs->kind != ExprKind::NullLit) {
            r = checkExpr(*b.rhs, nullptr);
            l = checkExpr(*b.lhs, r);
        } else {
            l = checkExpr(*b.lhs, arithmetic ? expected : nullptr);
            r = checkExpr(*b.rhs, b.rhs->kind == ExprKind::NullLit ? l : (arithmetic ? expected : nullptr));
        }
        if (l->isError() || r->isError()) return tc_.error();

        if (l->isLiteral() && r->isLiteral()) {
            const Type* lit = (l->kind == K::FloatLit || r->kind == K::FloatLit) ? tc_.floatLit() : tc_.intLit();
            if (arithmetic && op != "<<" && op != ">>" && op != "&" && op != "|" && op != "^") {
                if (expected && assignable(lit, expected)) {
                    setLiteralType(*b.lhs, expected);
                    setLiteralType(*b.rhs, expected);
                    l = r = expected;
                } else {
                    return lit; // stays untyped until something fixes it
                }
            } else {
                l = r = defaultLiteral(lit);
                setLiteralType(*b.lhs, l);
                setLiteralType(*b.rhs, r);
            }
        } else if (l->isLiteral()) {
            if (!assignable(l, r)) return mismatch(b, l, r);
            setLiteralType(*b.lhs, r);
            l = r;
        } else if (r->isLiteral()) {
            if (!assignable(r, l)) return mismatch(b, l, r);
            setLiteralType(*b.rhs, l);
            r = l;
        }

        if (op == "..") {
            if (l != r || !isIntegerType(l)) {
                error(b.loc, "a range needs two integers of the same type, found " + str(l) + " and " + str(r));
                return tc_.error();
            }
            return tc_.range(l);
        }
        if (comparison && l->kind == K::Ptr && l == r && (op == "==" || op == "!=")) return lang_.boolT;

        // Direct operator, then the derived forms.
        if (auto f = findBinary(op, l, r)) {
            calls_[&b] = {CallInfo::Fn, f->fn, {}};
            return f->result;
        }
        if (op == "!=" && findBinary("==", l, r)) return lang_.boolT;
        if ((op == ">" || op == "<=") && findBinary("<", r, l)) return lang_.boolT;
        if (op == ">=" && findBinary("<", l, r)) return lang_.boolT;
        return mismatch(b, l, r);
    }

    const Type* mismatch(BinaryExpr& b, const Type* l, const Type* r) {
        if (l == r || r->isLiteral())
            error(b.loc, str(l) + " has no operator " + q(b.op) + operatorHint(l, b.op));
        else
            error(b.loc, "no operator " + q(b.op) + " for " + str(l) + " and " + str(r) + conversionHint(r, l));
        return tc_.error();
    }

    const Type* checkIndex(IndexExpr& ix) {
        const Type* obj = checkExpr(*ix.object, nullptr);
        if (obj->isError()) {
            checkExpr(*ix.index, nullptr);
            return obj;
        }
        if (obj->kind == K::Array || obj->kind == K::Slice || obj->kind == K::Ptr) {
            const Type* i = checkExpr(*ix.index, lang_.uintT);
            if (i->isLiteral()) setLiteralType(*ix.index, lang_.uintT);
            else if (!i->isError() && !isIntegerType(i))
                error(ix.index->loc, "an index must be an integer, found " + str(i));
            return obj->elem();
        }
        const Type* i = checkExpr(*ix.index, nullptr);
        if (i->isLiteral()) {
            // Look for operator [] and type the literal from its parameter.
            for (const FunctionDecl* fn : operatorsOf(obj))
                if (fn->name == "[]" && fn->params.size() == 2) {
                    const Type* p = subst(sigOf(*fn, selfTypeOf(obj->sym)).params[1], substFor(obj));
                    if (assignable(i, p)) setLiteralType(*ix.index, p);
                    break;
                }
            i = finalize(*ix.index);
        }
        if (auto f = findOperatorOn(obj, "[]", {obj, i})) return f->result;
        if (const FunctionDecl* at = seqMethod(obj, "at")) {
            // Sequence protocol: seq[i] is *seq.at(i), a place you can read or assign.
            if (i->isLiteral()) setLiteralType(*ix.index, lang_.uintT);
            else if (!i->isError() && !isIntegerType(i))
                error(ix.index->loc, "an index must be an integer, found " + str(i));
            const Type* r = subst(sigOf(*at, selfTypeOf(obj->sym)).ret, substFor(obj));
            return r->kind == K::Ptr ? r->elem() : tc_.error();
        }
        error(ix.loc, str(obj) + " cannot be indexed" + operatorHint(obj, "[]") +
                          (obj->kind == K::Named ? " (or give it len() and at(i: uint) -> ptr<T> methods)" : ""));
        return tc_.error();
    }

    // A class's `len() -> uint` or `at(i: uint) -> ptr<T>` method, for the sequence protocol.
    const FunctionDecl* seqMethod(const Type* t, const char* name) {
        if (t->kind != K::Named || t->sym->kind != Symbol::Kind::Class) return nullptr;
        for (const auto& m : declAs<ClassDecl>(t->sym)->methods) {
            if (m->isOperator || m->name != name || !m->generics.empty()) continue;
            const Sig& sig = sigOf(*m, selfTypeOf(t->sym));
            if (std::string(name) == "at" && sig.params.size() == 1 && sig.ret->kind == K::Ptr) return m.get();
            if (std::string(name) == "len" && sig.params.empty() && sig.ret == lang_.uintT) return m.get();
        }
        return nullptr;
    }

    const Type* checkTry(TryExpr& t) {
        const Type* v = checkExpr(*t.operand, nullptr);
        if (v->isError()) return v;
        bool isResult = v->kind == K::Named && v->sym == lang_.result;
        bool isOption = v->kind == K::Named && v->sym == lang_.option;
        if (!isResult && !isOption) {
            error(t.loc, "'?' needs a Result or Option, found " + str(v));
            return tc_.error();
        }
        const Type* fr = retType_;
        if (!fr || fr->isError()) return v->args[0];
        bool fnResult = fr->kind == K::Named && fr->sym == lang_.result;
        bool fnOption = fr->kind == K::Named && fr->sym == lang_.option;
        if (isResult && !fnResult) {
            error(t.loc, "'?' on a Result can only be used in a function that returns Result<_, " + str(v->args[1]) +
                             ">; this one returns " + str(fr));
        } else if (isOption && !fnOption) {
            error(t.loc, "'?' on an Option can only be used in a function that returns Option; this one returns " +
                             str(fr));
        } else if (isResult && fr->args[1] != v->args[1] && !fr->args[1]->isError()) {
            error(t.loc, "error types differ: this is Result<_, " + str(v->args[1]) +
                             "> but the function returns Result<_, " + str(fr->args[1]) + ">");
        }
        return v->args[0];
    }

    const Type* checkArrayLit(ArrayLitExpr& a, const Type* expected) {
        const Type* elem = nullptr;
        if (expected && (expected->kind == K::Array || expected->kind == K::Slice)) elem = expected->elem();
        if (expected && expected->kind == K::Array && a.elems.size() > expected->size) {
            error(a.loc, "too many elements: " + str(expected) + " holds " + std::to_string(expected->size) +
                             ", but the literal has " + std::to_string(a.elems.size()));
        }
        if (a.elems.empty() && !elem) {
            error(a.loc, "cannot tell the element type of an empty array; add a type annotation");
            return tc_.error();
        }
        for (size_t i = 0; i < a.elems.size(); ++i) {
            if (!elem) {
                checkExpr(*a.elems[i], nullptr);
                elem = finalize(*a.elems[i]);
                continue;
            }
            coerce(*a.elems[i], elem, "array element");
        }
        if (expected && expected->kind == K::Array) return tc_.array(elem, expected->size);
        return tc_.array(elem, a.elems.size());
    }

    // -------------------------------------------------------------- calls

    static bool mentions(const Type* t, const std::set<const Symbol*>& vars) {
        if (!t) return false;
        if (t->kind == K::Generic) return vars.count(t->sym) > 0;
        for (const Type* a : t->args)
            if (mentions(a, vars)) return true;
        return false;
    }

    // Infers generic bindings by matching a parameter type against an argument type.
    void unify(const Type* p, const Type* a, Subst& s, const std::set<const Symbol*>& vars) {
        if (!p || !a || a->isError()) return;
        if (p->kind == K::Generic && vars.count(p->sym)) {
            auto it = s.find(p->sym);
            if (it == s.end() || !it->second) s[p->sym] = a;
            else if (it->second->isLiteral() && !a->isLiteral() && assignable(it->second, a)) it->second = a;
            else if (it->second->kind == K::IntLit && a->kind == K::FloatLit) it->second = a;
            return;
        }
        if (p->kind != a->kind) return;
        if (p->kind == K::Named && p->sym != a->sym) return;
        for (size_t i = 0; i < p->args.size() && i < a->args.size(); ++i) unify(p->args[i], a->args[i], s, vars);
    }

    // Checks arguments against parameters that may mention generic `vars`,
    // inferring the missing bindings. Returns false if inference failed.
    bool checkArgs(CallExpr& c, const std::vector<const Type*>& params, Subst& s,
                   const std::set<const Symbol*>& vars, const Type* ret, const Type* expected,
                   const std::string& calleeName, const std::vector<GenericParam>* generics,
                   bool isVariant = false) {
        if (c.args.size() != params.size()) {
            error(c.loc, calleeName + " takes " + std::to_string(params.size()) + " argument" +
                             (params.size() == 1 ? "" : "s") + ", but " + std::to_string(c.args.size()) +
                             (c.args.size() == 1 ? " was" : " were") + " given");
            for (auto& a : c.args) checkExpr(*a, nullptr);
            return false;
        }
        if (expected && ret) unify(ret, expected, s, vars);
        std::vector<const Type*> argTypes;
        for (size_t i = 0; i < c.args.size(); ++i) {
            const Type* p = subst(params[i], s);
            // A parameter type still mentioning our unbound generics is no real expectation.
            if (mentions(p, vars)) p = nullptr;
            argTypes.push_back(checkExpr(*c.args[i], p));
            unify(params[i], argTypes.back(), s, vars);
        }
        bool ok = true;
        if (generics) {
            for (const auto& g : *generics) {
                auto it = s.find(g.sym);
                bool argError = std::any_of(argTypes.begin(), argTypes.end(), [](const Type* t) { return t->isError(); });
                if (it == s.end() || !it->second) {
                    if ((expected && expected->isError()) || argError) { // already reported
                        s[g.sym] = tc_.error();
                        ok = false;
                        continue;
                    }
                    if (isVariant)
                        error(c.loc, "cannot infer " + g.name + " for " + calleeName +
                                         "; give the value a type, e.g. let r: " + declSym_[ret ? ret->sym->decl : nullptr]->name +
                                         "<...> = " + calleeName + "(...)");
                    else
                        error(c.loc, "cannot infer " + g.name + " for " + calleeName + "; write it explicitly: " +
                                         calleeName + "<...>(...)");
                    s[g.sym] = tc_.error();
                    ok = false;
                    continue;
                }
                it->second = defaultLiteral(it->second);
            }
            if (ok)
                for (const auto& g : *generics) checkConstraints(g, s[g.sym], c.loc);
        }
        for (size_t i = 0; i < c.args.size(); ++i) {
            const Type* p = subst(params[i], s);
            if (!assignable(argTypes[i], p)) {
                error(c.args[i]->loc, "argument " + std::to_string(i + 1) + " of " + calleeName + ": expected " +
                                          str(p) + ", found " + str(argTypes[i]) + conversionHint(argTypes[i], p));
                ok = false;
            } else if (argTypes[i]->isLiteral()) {
                setLiteralType(*c.args[i], p);
            } else {
                noteCoercion(*c.args[i], argTypes[i], p);
            }
        }
        return ok;
    }

    // Structural interface check: does `t` provide every member of each constraint?
    void checkConstraints(const GenericParam& g, const Type* t, SourceLoc loc) {
        if (t->isError()) return;
        for (const auto& c : g.constraints) {
            if (!c->sym || c->sym->kind != Symbol::Kind::Interface) continue;
            if (c->sym == sendableSym_ && t->kind != K::Generic) {
                std::string why;
                if (!sendable(t, why, 0))
                    error(loc, str(t) + " can't be sent to another thread: " + why);
                continue;
            }
            if (t->kind == K::Generic) {
                bool has = false;
                if (t->sym->genericParam)
                    for (const auto& own : t->sym->genericParam->constraints)
                        if (own->sym == c->sym) has = true;
                if (!has)
                    error(loc, str(t) + " is not known to satisfy " + c->sym->name + "; add the constraint: <" +
                                   str(t) + ": " + c->sym->name + ">");
                continue;
            }
            const auto* iface = declAs<InterfaceDecl>(c->sym);
            Subst s;
            for (size_t i = 0; i < iface->generics.size() && i < c->args.size(); ++i)
                s[iface->generics[i].sym] = convert(*c->args[i], nullptr);
            for (const auto& m : iface->members) {
                const Sig& sig = sigOf(*m, t);
                std::vector<const Type*> ps;
                for (const Type* p : sig.params) ps.push_back(subst(p, s));
                bool found = false;
                if (m->isOperator) {
                    auto f = findOperatorOn(t, m->name, ps);
                    found = f && f->result == subst(sig.ret, s);
                } else if (t->kind == K::Named && t->sym->kind == Symbol::Kind::Class) {
                    for (const auto& meth : declAs<ClassDecl>(t->sym)->methods)
                        if (!meth->isOperator && meth->name == m->name && meth->params.size() == m->params.size())
                            found = true;
                }
                if (!found) {
                    std::string sigText = m->name + " (";
                    for (size_t i = 0; i < ps.size(); ++i)
                        sigText += (i ? ", " : "") + m->params[i].name + ": " + str(ps[i]);
                    sigText += ")";
                    const Type* r = subst(sig.ret, s);
                    if (r != tc_.voidT()) sigText += " -> " + str(r);
                    error(loc, str(t) + " does not satisfy " + c->sym->name + ": it needs " +
                                   (m->isOperator ? "new operator " : "new function ") + sigText);
                }
            }
        }
    }

    const Type* checkCall(CallExpr& c, const Type* expected) {
        // Method call: obj.method(...)
        if (c.callee->kind == ExprKind::Member && !static_cast<MemberExpr&>(*c.callee).sym &&
            !isQualifiedPrefix(*c.callee)) {
            auto& m = static_cast<MemberExpr&>(*c.callee);
            const Type* obj = checkExpr(*m.object, nullptr);
            finalize(*m.object);
            if (obj->isError()) return argsUnchecked(c);
            const Type* base = obj->kind == K::Ptr ? obj->elem() : obj;
            if (base->kind == K::Dyn) {
                // Dynamic dispatch through `any I`.
                for (const auto& fn : declAs<InterfaceDecl>(base->sym)->members) {
                    if (fn->isOperator || fn->name != m.member) continue;
                    const Sig& sig = sigOf(*fn, base);
                    Subst s;
                    checkArgs(c, sig.params, s, {}, sig.ret, expected, m.member, nullptr);
                    calls_[&c] = {CallInfo::Fn, fn.get(), {}};
                    m.type = tc_.voidT();
                    return sig.ret;
                }
                error(m.loc, str(base) + " has no method " + q(m.member));
                return argsUnchecked(c);
            }
            if (base->kind == K::Named && base->sym->kind == Symbol::Kind::Class) {
                for (const auto& fn : declAs<ClassDecl>(base->sym)->methods) {
                    if (fn->isOperator || fn->name != m.member) continue;
                    checkMemberAccess(base->sym, fn->isPub, "method", fn->name, m.loc);
                    if (fn->isMut && obj->kind != K::Ptr && isPlace(*m.object)) {
                        if (const Symbol* root = immutableRoot(*m.object))
                            error(m.loc, q(fn->name) + " changes the value, but " + q(root->name) +
                                             " can't be changed (it's a let or a parameter). Declare it with var");
                        else if (selfRooted(*m.object) && !selfMutable_)
                            error(m.loc, q(fn->name) + " changes self, but this method can't" + mutHint());
                    }
                    const Sig& sig = sigOf(*fn, selfTypeOf(base->sym));
                    Subst s = substFor(base);
                    std::set<const Symbol*> vars;
                    for (const auto& g : fn->generics) vars.insert(g.sym);
                    checkArgs(c, sig.params, s, vars, sig.ret, expected, m.member, &fn->generics);
                    checkVarArgs(c, *fn);
                    calls_[&c] = {CallInfo::Fn, fn.get(), s};
                    m.type = tc_.voidT();
                    return subst(sig.ret, s);
                }
            }
            // A field holding a function: obj.callback(...)
            bool hasField = false;
            if (base->kind == K::Named && (base->sym->kind == Symbol::Kind::Class || base->sym->kind == Symbol::Kind::Type)) {
                const auto& fields = base->sym->kind == Symbol::Kind::Class ? declAs<ClassDecl>(base->sym)->fields
                                                                           : declAs<TypeDecl>(base->sym)->fields;
                for (const auto& f : fields) hasField = hasField || f.name == m.member;
            }
            if (hasField) {
                const Type* ft = checkField(m);
                if (ft->kind == K::Function) {
                    m.type = ft;
                    return callValue(c, ft);
                }
                if (!ft->isError()) error(m.loc, str(base) + " has no method " + q(m.member));
                return argsUnchecked(c);
            }
            error(m.loc, str(base) + " has no method " + q(m.member));
            return argsUnchecked(c);
        }

        const Symbol* s = nullptr;
        std::vector<TypeRefPtr>* typeArgs = nullptr;
        if (c.callee->kind == ExprKind::Ident) {
            auto& id = static_cast<IdentExpr&>(*c.callee);
            s = id.sym;
            typeArgs = &id.typeArgs;
        } else if (c.callee->kind == ExprKind::Member) {
            s = static_cast<MemberExpr&>(*c.callee).sym;
        }
        if (isQualifiedPrefix(*c.callee)) {
            // intrinsic.*: unchecked; the compiler provides these.
            for (auto& a : c.args) {
                checkExpr(*a, nullptr);
                finalize(*a);
            }
            return tc_.error();
        }
        if (!s) {
            const Type* t = checkExpr(*c.callee, nullptr);
            if (t->kind == K::Function) return callValue(c, t);
            if (!t->isError()) error(c.loc, "cannot call a value of type " + str(t));
            return argsUnchecked(c);
        }
        c.callee->type = tc_.voidT();

        std::vector<const Type*> explicitArgs;
        if (typeArgs)
            for (auto& ta : *typeArgs) explicitArgs.push_back(convert(*ta, selfType_));

        switch (s->kind) {
        case Symbol::Kind::Function: {
            const auto* fn = declAs<FunctionDecl>(s);
            const Sig& sig = sigOf(*fn, nullptr);
            Subst sub;
            std::set<const Symbol*> vars;
            for (size_t i = 0; i < fn->generics.size(); ++i) {
                vars.insert(fn->generics[i].sym);
                if (i < explicitArgs.size()) sub[fn->generics[i].sym] = explicitArgs[i];
            }
            checkArgs(c, sig.params, sub, vars, sig.ret, expected, s->name, &fn->generics);
            checkVarArgs(c, *fn);
            calls_[&c] = {CallInfo::Fn, fn, sub};
            return subst(sig.ret, sub);
        }
        case Symbol::Kind::Variant: {
            const auto& payload = s->variant->payload;
            const Symbol* enumSym = declSym_[s->decl];
            const auto* gs = genericsOf(enumSym);
            if (payload.empty()) {
                error(c.loc, q(s->name) + " holds no value; write it without parentheses");
                return argsUnchecked(c);
            }
            Subst sub;
            enumTypeFor(s, expected, &sub);
            std::set<const Symbol*> vars;
            for (const auto& g : *gs) vars.insert(g.sym);
            std::vector<const Type*> params;
            for (const auto& p : payload) params.push_back(convert(*p, nullptr));
            checkArgs(c, params, sub, vars, selfTypeOf(enumSym), expected && expected->isError() ? expected : nullptr,
                      s->name, gs, /*isVariant=*/true);
            calls_[&c] = {CallInfo::ConsumeAll, nullptr, sub};
            std::vector<const Type*> args;
            for (const auto& g : *gs) args.push_back(sub[g.sym]);
            return tc_.named(enumSym, std::move(args));
        }
        case Symbol::Kind::Class:
        case Symbol::Kind::Type: return checkConstruct(c, s, explicitArgs, expected);
        case Symbol::Kind::Local: {
            auto it = localTypes_.find(s);
            if (it != localTypes_.end() && it->second->kind == K::Function) {
                c.callee->type = it->second;
                return callValue(c, it->second);
            }
            if (it != localTypes_.end() && !it->second->isError())
                error(c.loc, q(s->name) + " is a variable of type " + str(it->second) + ", not a function");
            return argsUnchecked(c);
        }
        case Symbol::Kind::Generic:
            error(c.loc, "cannot construct generic type " + s->name);
            return argsUnchecked(c);
        case Symbol::Kind::Builtin:
            if (s->name == "ptr" && explicitArgs.size() == 1 && c.args.size() == 1) {
                // ptr<T>(p): reinterpret a pointer (or an address-sized uint) as ptr<T>
                const Type* from = checkExpr(*c.args[0], nullptr);
                if (from->isLiteral()) from = finalize(*c.args[0]);
                if (!from->isError() && from->kind != K::Ptr && from != lang_.uintT && from != lang_.intT)
                    error(c.loc, "ptr<T>(x) converts a pointer or an int/uint address, not " + str(from));
                return tc_.ptr(explicitArgs[0]);
            }
            if (s->name == "ptr") error(c.loc, "write a pointer cast as ptr<T>(value)");
            for (auto& a : c.args) { // intrinsic.*: unchecked, provided by the compiler
                checkExpr(*a, nullptr);
                finalize(*a);
            }
            return tc_.error();
        default: return argsUnchecked(c);
        }
    }

    // Calls through a function value: f(x), self.compare(a, b), function(...){...}(x)
    const Type* callValue(CallExpr& c, const Type* ft) {
        size_t n = ft->args.size() - 1;
        if (c.args.size() != n) {
            error(c.loc, "this function takes " + std::to_string(n) + " argument" + (n == 1 ? "" : "s") + ", but " +
                             std::to_string(c.args.size()) + (c.args.size() == 1 ? " was" : " were") + " given");
            return argsUnchecked(c);
        }
        for (size_t i = 0; i < n; ++i) coerce(*c.args[i], ft->args[i + 1], "argument " + std::to_string(i + 1));
        return ft->args[0];
    }

    const Type* argsUnchecked(CallExpr& c) {
        for (auto& a : c.args) checkExpr(*a, nullptr);
        return tc_.error();
    }

    // Point(1.0, 2.0), List<int>(), int(c), Meters(5.0)
    const Type* checkConstruct(CallExpr& c, const Symbol* s, const std::vector<const Type*>& explicitArgs,
                               const Type* expected) {
        if (lang_.stringT->kind == K::Named && s == lang_.stringT->sym && c.args.size() == 1)
            return checkToString(c);
        if (s->kind == Symbol::Kind::Type) {
            const auto* td = declAs<TypeDecl>(s);
            if (td->base != TypeDecl::Base::Layout) {
                const Type* target = typeOfTypeSymbol(s, {});
                if (target->isError()) return argsUnchecked(c);
                if (target->kind == K::Named && target->sym != s && target->sym->kind == Symbol::Kind::Type &&
                    declAs<TypeDecl>(target->sym)->base == TypeDecl::Base::Layout)
                    return checkConstruct(c, target->sym, explicitArgs, expected); // alias of a layout type
                return checkConversion(c, target);
            }
        }
        const auto* gs = genericsOf(s);
        Subst sub;
        std::set<const Symbol*> vars;
        for (size_t i = 0; i < gs->size(); ++i) {
            vars.insert((*gs)[i].sym);
            if (i < explicitArgs.size()) sub[(*gs)[i].sym] = explicitArgs[i];
        }
        const Type* selfT = selfTypeOf(s);
        std::vector<const Type*> params;
        std::string what = s->name;
        bool usesInit = false;
        const FunctionDecl* initFn = nullptr;
        if (s->kind == Symbol::Kind::Class) {
            for (const auto& fn : declAs<ClassDecl>(s)->methods)
                if (!fn->isOperator && fn->name == "init") {
                    params = sigOf(*fn, selfT).params;
                    usesInit = true;
                    initFn = fn.get();
                    checkMemberAccess(s, fn->isPub, "function", "init", c.loc);
                }
        }
        if (!usesInit) {
            const auto& fields = s->kind == Symbol::Kind::Class ? declAs<ClassDecl>(s)->fields
                                                                : declAs<TypeDecl>(s)->fields;
            for (const auto& f : fields) {
                params.push_back(convert(*f.type, selfT));
                if (!f.isPub && s->module != mod_) {
                    error(c.loc, "cannot construct " + s->name + " here: field " + q(f.name) +
                                     " is not pub. Give " + s->name + " a pub 'init' function");
                    return argsUnchecked(c);
                }
            }
            what += " (fields in order)";
        }
        checkArgs(c, params, sub, vars, selfT, expected, what, gs);
        if (initFn) checkVarArgs(c, *initFn);
        calls_[&c] = usesInit ? CallInfo{CallInfo::Fn, initFn, sub} : CallInfo{CallInfo::ConsumeAll, nullptr, sub};
        std::vector<const Type*> args;
        for (const auto& g : *gs) args.push_back(sub[g.sym] ? sub[g.sym] : tc_.error());
        return tc_.named(s, std::move(args));
    }

    // string(x): text from a str (copy) or from any type with `operator string`.
    const Type* checkToString(CallExpr& c) {
        checkExpr(*c.args[0], nullptr);
        const Type* t = finalize(*c.args[0]);
        if (t->isError() || t == lang_.strT) return lang_.stringT;
        if (findOperatorOn(t, "string", {t})) return lang_.stringT;
        if (t->kind == K::Generic)
            error(c.loc, "cannot convert " + str(t) + " to string; add the constraint <" + str(t) + ": Printable>");
        else
            error(c.loc, str(t) + " can't be converted to string. Give it a conversion: new operator string (v: " +
                             str(t) + ") -> string");
        return lang_.stringT;
    }

    // int(c), Meters(5.0): explicit conversion between scalar types.
    const Type* checkConversion(CallExpr& c, const Type* target) {
        if (c.args.size() != 1) {
            error(c.loc, "a conversion to " + str(target) + " takes exactly 1 argument");
            return argsUnchecked(c);
        }
        const Type* from = checkExpr(*c.args[0], target);
        if (from->isError()) return target;
        if (from->isLiteral()) {
            if (assignable(from, target)) setLiteralType(*c.args[0], target);
            else finalize(*c.args[0]);
            from = c.args[0]->type;
        }
        if (from == target) return target;
        if (scalarOf(from).kind == Scalar::None || scalarOf(target).kind == Scalar::None) {
            error(c.loc, "cannot convert " + str(from) + " to " + str(target) +
                             ": conversions work between number, bool and char types");
            return tc_.error();
        }
        return target;
    }

    // -------------------------------------------------------------- match

    const Type* checkMatch(MatchExpr& m, const Type* expected) {
        checkExpr(*m.subject, nullptr);
        const Type* subj = finalize(*m.subject);
        const Type* result = nullptr;
        bool allExprs = true;
        for (auto& arm : m.arms) {
            checkPattern(*arm.pattern, subj);
            if (arm.block) {
                allExprs = false;
                checkBlock(*arm.block);
            } else if (expected) {
                coerce(*arm.expr, expected, "match arm");
            } else if (!result) {
                checkExpr(*arm.expr, nullptr);
                result = finalize(*arm.expr);
            } else {
                checkExpr(*arm.expr, result);
                if (!assignable(arm.expr->type, result)) result = tc_.voidT(); // arms disagree: no value
                else setLiteralType(*arm.expr, result);
            }
        }
        checkExhaustive(m, subj);
        if (!allExprs) return tc_.voidT();
        return expected ? expected : (result ? result : tc_.voidT());
    }

    void checkPattern(Pattern& p, const Type* subj) {
        switch (p.kind) {
        case Pattern::Kind::Wildcard: return;
        case Pattern::Kind::Binding:
            if (p.sym) localTypes_[p.sym] = subj;
            return;
        case Pattern::Kind::Literal:
            coerce(*p.literal, subj, "pattern");
            return;
        case Pattern::Kind::Variant: {
            if (!p.sym || subj->isError()) {
                for (auto& f : p.fields) checkPattern(*f, tc_.error());
                return;
            }
            const Symbol* enumSym = declSym_[p.sym->decl];
            if (subj->kind != K::Named || subj->sym != enumSym) {
                error(p.loc, q(p.sym->name) + " is a variant of " + enumSym->name + ", but the value is " + str(subj));
                for (auto& f : p.fields) checkPattern(*f, tc_.error());
                return;
            }
            Subst s = substFor(subj);
            const auto& payload = p.sym->variant->payload;
            for (size_t i = 0; i < p.fields.size(); ++i)
                checkPattern(*p.fields[i], i < payload.size() ? subst(convert(*payload[i], nullptr), s) : tc_.error());
            return;
        }
        }
    }

    static bool irrefutable(const Pattern& p) {
        return p.kind == Pattern::Kind::Wildcard || p.kind == Pattern::Kind::Binding;
    }

    // Exhaustiveness via the classic pattern-matrix algorithm: a row is one
    // arm's patterns for the remaining columns. Returns true if every value
    // of `types` is matched by some row.
    using Row = std::vector<const Pattern*>;

    bool exhaustive(const std::vector<Row>& rows, const std::vector<const Type*>& types) {
        if (types.empty()) return !rows.empty();
        const Type* t = types[0];
        std::vector<const Type*> rest(types.begin() + 1, types.end());

        auto specialize = [&](auto matches, size_t arity, const std::vector<const Type*>& fieldTypes) {
            std::vector<Row> out;
            for (const Row& r : rows) {
                const Pattern* p = r[0];
                Row nr;
                if (irrefutable(*p)) {
                    nr.assign(arity, &wildcard_);
                } else if (matches(*p)) {
                    for (const auto& f : p->fields) nr.push_back(f.get());
                    nr.resize(arity, &wildcard_);
                } else {
                    continue;
                }
                nr.insert(nr.end(), r.begin() + 1, r.end());
                out.push_back(std::move(nr));
            }
            std::vector<const Type*> ts = fieldTypes;
            ts.insert(ts.end(), rest.begin(), rest.end());
            return exhaustive(out, ts);
        };

        if (t->kind == K::Named && t->sym->kind == Symbol::Kind::Enum) {
            Subst s = substFor(t);
            for (const auto& [name, v] : t->sym->members) {
                std::vector<const Type*> fts;
                for (const auto& p : v->variant->payload) fts.push_back(subst(convert(*p, nullptr), s));
                auto isV = [&](const Pattern& p) { return p.kind == Pattern::Kind::Variant && p.sym == v; };
                if (!specialize(isV, fts.size(), fts)) return false;
            }
            return true;
        }
        if (t == lang_.boolT) {
            for (const char* lit : {"true", "false"}) {
                auto isLit = [&](const Pattern& p) {
                    return p.kind == Pattern::Kind::Literal && p.literal->kind == ExprKind::BoolLit &&
                           static_cast<const LiteralExpr&>(*p.literal).value == lit;
                };
                if (!specialize(isLit, 0, {})) return false;
            }
            return true;
        }
        // Infinite or opaque domain: only catch-all rows help.
        std::vector<Row> out;
        for (const Row& r : rows)
            if (irrefutable(*r[0])) out.emplace_back(r.begin() + 1, r.end());
        return exhaustive(out, rest);
    }

    void checkExhaustive(const MatchExpr& m, const Type* subj) {
        if (subj->isError()) return;
        std::vector<Row> rows;
        for (const auto& arm : m.arms) rows.push_back({arm.pattern.get()});
        if (exhaustive(rows, {subj})) return;
        if (subj->kind == K::Named && subj->sym->kind == Symbol::Kind::Enum) {
            // Name the top-level variants that are not fully covered.
            Subst s = substFor(subj);
            std::string missing;
            for (const auto& [name, v] : subj->sym->members) {
                std::vector<Row> vr;
                std::vector<const Type*> fts;
                for (const auto& p : v->variant->payload) fts.push_back(subst(convert(*p, nullptr), s));
                for (const auto& arm : m.arms) {
                    const Pattern* p = arm.pattern.get();
                    if (irrefutable(*p)) vr.emplace_back(fts.size(), &wildcard_);
                    else if (p->kind == Pattern::Kind::Variant && p->sym == v) {
                        Row r;
                        for (const auto& f : p->fields) r.push_back(f.get());
                        r.resize(fts.size(), &wildcard_);
                        vr.push_back(std::move(r));
                    }
                }
                if (!exhaustive(vr, fts)) missing += (missing.empty() ? "" : ", ") + name;
            }
            error(m.loc, "match does not cover every case of " + str(subj) + ": missing " + missing +
                             ". Add those arms or a '_' arm");
            return;
        }
        error(m.loc, "match on " + str(subj) + " needs a '_' arm to cover the remaining values");
    }

    // -------------------------------------------------------------- statements

    void checkBlock(Block& b) {
        for (auto& s : b.stmts) checkStmt(*s);
    }

    void noteLocal(const std::string& name, SourceLoc loc, const Type* t) {
        if (notes_ && mod_ == prog_.main()) notes_->push_back({file_->displayFile, loc, name, str(t)});
    }

    void checkStmt(Stmt& s) {
        switch (s.kind) {
        case StmtKind::Block: checkBlock(static_cast<Block&>(s)); return;
        case StmtKind::Let: {
            auto& l = static_cast<LetStmt&>(s);
            const Type* declared = l.type ? convert(*l.type, selfType_) : nullptr;
            const Type* t = declared;
            if (l.init) {
                if (declared) {
                    coerce(*l.init, declared, "value of " + q(l.name));
                } else {
                    checkExpr(*l.init, nullptr);
                    t = finalize(*l.init);
                    if (t == tc_.voidT()) {
                        error(l.init->loc, "this has no value to store in " + q(l.name));
                        t = tc_.error();
                    }
                }
            }
            if (!t) t = tc_.error();
            if (l.sym) localTypes_[l.sym] = t;
            noteLocal(l.name, l.loc, t);
            return;
        }
        case StmtKind::Assign: {
            auto& a = static_cast<AssignStmt&>(s);
            if (a.target->kind == ExprKind::Ident) {
                const Symbol* ts = static_cast<IdentExpr&>(*a.target).sym;
                if (ts && ts->kind != Symbol::Kind::Local) { // reported by the resolver
                    checkExpr(*a.value, nullptr);
                    return;
                }
            }
            const Type* target = checkExpr(*a.target, nullptr);
            if (a.target->kind != ExprKind::Ident && !target->isError()) {
                bool bad = false;
                if (!isPlace(*a.target)) {
                    error(a.target->loc, "cannot assign to this expression");
                    bad = true;
                } else if (const Symbol* root = immutableRoot(*a.target)) {
                    error(a.target->loc, "cannot change " + q(root->name) +
                                             ": it is immutable. Declare it with 'var' to allow changes");
                } else if (selfRooted(*a.target) && !selfMutable_) {
                    error(a.target->loc, "this method can't change self" + mutHint());
                }
                if (bad) target = tc_.error();
            }
            if (target->isError()) {
                checkExpr(*a.value, nullptr);
                return;
            }
            if (a.op == "=") {
                coerce(*a.value, target, "assigned value");
                return;
            }
            std::string op = a.op.substr(0, a.op.size() - 1);
            const Type* v = checkExpr(*a.value, target);
            if (v->isLiteral() && assignable(v, target)) {
                setLiteralType(*a.value, target);
                v = target;
            }
            auto f = findBinary(op, target, v);
            if (!f) error(a.loc, "no operator " + q(op) + " for " + str(target) + " and " + str(v));
            else if (!assignable(f->result, target))
                error(a.loc, q(a.op) + " would produce " + str(f->result) + ", which can't be stored in " + str(target));
            return;
        }
        case StmtKind::Expr: {
            auto& e = static_cast<ExprStmt&>(s);
            checkExpr(*e.expr, nullptr);
            finalize(*e.expr);
            return;
        }
        case StmtKind::If: {
            auto& i = static_cast<IfStmt&>(s);
            coerce(*i.cond, lang_.boolT, "condition of 'if'");
            checkBlock(*i.then);
            if (i.otherwise) checkStmt(*i.otherwise);
            return;
        }
        case StmtKind::While: {
            auto& w = static_cast<WhileStmt&>(s);
            coerce(*w.cond, lang_.boolT, "condition of 'while'");
            ++loopDepth_;
            checkBlock(*w.body);
            --loopDepth_;
            return;
        }
        case StmtKind::For: {
            auto& f = static_cast<ForStmt&>(s);
            checkExpr(*f.iterable, nullptr);
            const Type* it = finalize(*f.iterable);
            const Type* elem = tc_.error();
            if (it->kind == K::Range || it->kind == K::Array || it->kind == K::Slice) elem = it->elem();
            else if (it == lang_.strT || it == lang_.stringT) elem = lang_.charT;
            else if (const FunctionDecl* at = seqMethod(it, "at"); at && seqMethod(it, "len")) {
                const Type* r = subst(sigOf(*at, selfTypeOf(it->sym)).ret, substFor(it));
                elem = r->elem();
            }
            else if (!it->isError()) error(f.iterable->loc, "cannot loop over " + str(it) +
                                                             "; 'for' works with ranges (a..b), arrays, strings, and "
                                                             "classes with len() and at(i) methods");
            if (f.sym) localTypes_[f.sym] = elem;
            ++loopDepth_;
            checkBlock(*f.body);
            --loopDepth_;
            return;
        }
        case StmtKind::Return: {
            auto& r = static_cast<ReturnStmt&>(s);
            if (!retType_) return;
            if (r.value) {
                if (retType_ == tc_.voidT()) {
                    checkExpr(*r.value, nullptr);
                    error(r.value->loc, "this function doesn't return a value; remove it or add '-> T' to the signature");
                } else {
                    coerce(*r.value, retType_, "return value");
                }
            } else if (retType_ != tc_.voidT() && !retType_->isError()) {
                error(r.loc, "missing return value of type " + str(retType_));
            }
            return;
        }
        case StmtKind::Break:
        case StmtKind::Continue:
            if (loopDepth_ == 0)
                error(s.loc, std::string(s.kind == StmtKind::Break ? "'break'" : "'continue'") + " outside of a loop");
            return;
        case StmtKind::Free: {
            auto& f = static_cast<FreeStmt&>(s);
            const Type* t = checkExpr(*f.target, nullptr);
            if (!t->isError() && t->kind != K::Ptr)
                error(f.target->loc, "'free' needs a pointer from 'alloc', found " + str(t));
            return;
        }
        case StmtKind::Asm: return;
        }
    }

    // -------------------------------------------------------------- control flow

    static bool containsBreak(const Stmt& s) {
        switch (s.kind) {
        case StmtKind::Break: return true;
        case StmtKind::Block:
            for (const auto& c : static_cast<const Block&>(s).stmts)
                if (containsBreak(*c)) return true;
            return false;
        case StmtKind::If: {
            const auto& i = static_cast<const IfStmt&>(s);
            return containsBreak(*i.then) || (i.otherwise && containsBreak(*i.otherwise));
        }
        default: return false; // nested loops own their breaks
        }
    }

    static bool alwaysReturns(const Stmt& s) {
        switch (s.kind) {
        case StmtKind::Return: return true;
        case StmtKind::Block:
            for (const auto& c : static_cast<const Block&>(s).stmts)
                if (alwaysReturns(*c)) return true;
            return false;
        case StmtKind::If: {
            const auto& i = static_cast<const IfStmt&>(s);
            return i.otherwise && alwaysReturns(*i.then) && alwaysReturns(*i.otherwise);
        }
        case StmtKind::While: {
            const auto& w = static_cast<const WhileStmt&>(s);
            return w.cond->kind == ExprKind::BoolLit && static_cast<const LiteralExpr&>(*w.cond).value == "true" &&
                   !containsBreak(*w.body);
        }
        case StmtKind::Expr: {
            const Expr& e = *static_cast<const ExprStmt&>(s).expr;
            if (e.kind != ExprKind::Match) return false;
            const auto& m = static_cast<const MatchExpr&>(e);
            if (m.arms.empty()) return false;
            for (const auto& arm : m.arms)
                if (!arm.block || !alwaysReturns(*arm.block)) return false;
            return true;
        }
        default: return false;
        }
    }

    // -------------------------------------------------------------- declarations

    void checkFunction(FunctionDecl& f, const Type* ownerSelf) {
        if (f.isExtern) {
            const Sig& sig = sigOf(f, ownerSelf);
            auto ok = [&](const Type* t) {
                return t->isError() || t == tc_.voidT() || t->kind == K::Ptr || t->kind == K::Function ||
                       scalarOf(t).kind != Scalar::None;
            };
            for (size_t i = 0; i < sig.params.size(); ++i) {
                if (!ok(sig.params[i]))
                    error(f.params[i].loc, "extern functions take numbers, bools, chars and pointers; pass " +
                                               str(sig.params[i]) + " by pointer instead");
                if (f.params[i].isOwn) error(f.params[i].loc, "'own' has no meaning for an extern function");
                if (f.params[i].isVar)
                    error(f.params[i].loc, "extern functions can't take var parameters; pass a pointer (&x) instead");
            }
            if (!ok(sig.ret)) error(f.loc, "an extern function must return a number, bool, char, pointer or nothing");
            return;
        }
        if (!f.body) return;
        const Sig& sig = sigOf(f, ownerSelf);
        const Type* savedRet = retType_;
        const Type* savedSelf = selfType_;
        bool savedMut = selfMutable_;
        const FunctionDecl* savedFn = currentFn_;
        retType_ = sig.ret;
        selfType_ = ownerSelf;
        currentFn_ = &f;
        selfMutable_ = f.isMut || f.name == "init" || f.name == "drop";
        for (size_t i = 0; i < f.params.size(); ++i)
            if (f.params[i].sym) localTypes_[f.params[i].sym] = sig.params[i];
        checkBlock(*f.body);
        if (sig.ret != tc_.voidT() && !sig.ret->isError() && !alwaysReturns(*f.body))
            error(f.body->loc, "function " + q(f.name) + " must return a value of type " + str(sig.ret) +
                                   " on every path; add a return at the end");
        retType_ = savedRet;
        selfType_ = savedSelf;
        selfMutable_ = savedMut;
        currentFn_ = savedFn;
    }

    // Is this place part of `self` (not reached through a pointer)?
    bool selfRooted(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Self: return true;
        case ExprKind::Member: {
            const auto& m = static_cast<const MemberExpr&>(e);
            if (m.sym || (m.object->type && m.object->type->kind == K::Ptr)) return false;
            return selfRooted(*m.object);
        }
        case ExprKind::Index: {
            const auto& i = static_cast<const IndexExpr&>(e);
            if (i.object->type && (i.object->type->kind == K::Ptr || i.object->type->kind == K::Slice)) return false;
            return selfRooted(*i.object);
        }
        default: return false;
        }
    }

    // Arguments for `var` parameters must be variables that can be changed.
    void checkVarArgs(CallExpr& c, const FunctionDecl& fn) {
        for (size_t i = 0; i < fn.params.size() && i < c.args.size(); ++i) {
            if (!fn.params[i].isVar) continue;
            Expr& a = *c.args[i];
            std::string p = q(fn.params[i].name);
            if (!a.type || a.type->isError()) continue;
            if (!isPlace(a) || a.type->isLiteral() || viewCoerced_.count(&a))
                error(a.loc, p + " is a var parameter: pass a variable that can be changed, not a value");
            else if (const Symbol* root = immutableRoot(a))
                error(a.loc, p + " is a var parameter, but " + q(root->name) +
                                 " can't be changed (it's a let or a parameter). Declare it with var");
            else if (selfRooted(a) && !selfMutable_)
                error(a.loc, p + " is a var parameter, and passing self's data changes self" + mutHint());
        }
    }

    std::string mutHint() {
        std::string n = currentFn_ ? currentFn_->name : "this method";
        return "; mark the method 'new mut function " + n + "' to allow it";
    }

    // ---------------------------------------------------------------- constants

    std::map<const Symbol*, const Type*> constTypes_;
    std::set<const Symbol*> constsInProgress_;

    // The type of a constant, checking its declaration the first time it's needed.
    const Type* constType(const Symbol* s) {
        if (auto it = constTypes_.find(s); it != constTypes_.end()) return it->second;
        const auto* cd = declAs<ConstDecl>(s);
        if (!constsInProgress_.insert(s).second) {
            error(cd->loc, "constant " + q(cd->name) + " is defined in terms of itself");
            return constTypes_[s] = tc_.error();
        }
        ModuleInfo* savedMod = mod_;
        SourceFile* savedFile = file_;
        mod_ = s->module;
        file_ = const_cast<SourceFile*>(s->file);
        auto& value = *const_cast<ConstDecl*>(cd)->value;
        const Type* t = cd->type ? convert(*cd->type, nullptr) : nullptr;
        if (t) {
            coerce(value, t, "value of constant " + q(cd->name));
        } else {
            checkExpr(value, nullptr);
            t = finalize(value);
        }
        if (!t->isError() && scalarOf(t).kind == Scalar::None && t != lang_.strT)
            error(cd->loc, "constants can be numbers, bool, char or str, not " + str(t));
        else if (!t->isError() && !isConstExpr(value))
            error(value.loc, "a constant's value must be computable at compile time: literals, other constants, "
                             "and operators or conversions on them");
        mod_ = savedMod;
        file_ = savedFile;
        constsInProgress_.erase(s);
        return constTypes_[s] = t;
    }

    bool isConstExpr(const Expr& e) {
        switch (e.kind) {
        case ExprKind::IntLit:
        case ExprKind::FloatLit:
        case ExprKind::StringLit:
        case ExprKind::CharLit:
        case ExprKind::BoolLit: return true;
        case ExprKind::Ident: {
            const Symbol* s = static_cast<const IdentExpr&>(e).sym;
            return s && s->kind == Symbol::Kind::Const;
        }
        case ExprKind::Member: {
            const Symbol* s = static_cast<const MemberExpr&>(e).sym;
            return s && s->kind == Symbol::Kind::Const;
        }
        case ExprKind::Unary: {
            const auto& u = static_cast<const UnaryExpr&>(e);
            return u.op != "&" && u.op != "*" && isConstExpr(*u.operand);
        }
        case ExprKind::Binary: {
            const auto& b = static_cast<const BinaryExpr&>(e);
            return b.op != ".." && isConstExpr(*b.lhs) && isConstExpr(*b.rhs);
        }
        case ExprKind::Call: { // conversions like u8(300 / 2)
            const auto& c = static_cast<const CallExpr&>(e);
            const Symbol* s = c.callee->kind == ExprKind::Ident ? static_cast<const IdentExpr&>(*c.callee).sym : nullptr;
            return s && s->kind == Symbol::Kind::Type && c.args.size() == 1 && e.type && scalarOf(e.type).kind != Scalar::None &&
                   isConstExpr(*c.args[0]);
        }
        default: return false;
        }
    }

    void checkDecl(Decl& d) {
        switch (d.kind) {
        case DeclKind::Const:
            if (const Symbol* s = declSym_[&d]) constType(s);
            return;
        case DeclKind::Function: {
            auto& f = static_cast<FunctionDecl&>(d);
            if (f.isOperator) checkExtensionOperator(f);
            checkFunction(f, nullptr);
            if (!f.isOperator && f.name == "main" && mod_ == prog_.main() && f.generics.empty()) {
                const Sig& sig = sigOf(f, nullptr);
                if (!f.params.empty() || (sig.ret != lang_.intT && sig.ret != tc_.voidT() && !sig.ret->isError()))
                    error(f.loc, "main must be 'new function main() -> int'");
            }
            return;
        }
        case DeclKind::Type: {
            auto& t = static_cast<TypeDecl&>(d);
            const Symbol* s = declSym_[&t];
            if (!s) return;
            const Type* selfT = selfTypeOf(s);
            for (auto& f : t.fields) convert(*f.type, selfT);
            for (auto& op : t.operators) checkOperatorShape(*op, selfT);
            for (auto& op : t.operators) checkFunction(*op, selfT);
            return;
        }
        case DeclKind::Class: {
            auto& c = static_cast<ClassDecl&>(d);
            const Symbol* s = declSym_[&c];
            if (!s) return;
            const Type* selfT = selfTypeOf(s);
            for (auto& m : c.methods) {
                if (m->isOperator) checkOperatorShape(*m, selfT);
                if (!m->isOperator && (m->name == "init" || m->name == "drop") &&
                    m->returnType)
                    error(m->loc, q(m->name) + " cannot return a value");
                if (!m->isOperator && m->name == "drop" && !m->params.empty())
                    error(m->loc, "'drop' takes no parameters");
                checkFunction(*m, selfT);
            }
            return;
        }
        default: return;
        }
    }

    // A top-level operator must be about a named type, so it can be found.
    void checkExtensionOperator(const FunctionDecl& op) {
        const Sig& sig = sigOf(op, nullptr);
        bool named = op.name == "string" || op.name == "hash";
        bool unaryOk = op.name == "-" || op.name == "!" || op.name == "~" || named;
        if (op.params.empty() || op.params.size() > 2 || (op.params.size() == 1 && !unaryOk) ||
            (named && op.params.size() != 1)) {
            error(op.loc, "operator " + q(op.name) + " has the wrong number of parameters");
            return;
        }
        bool hasNamed = std::any_of(sig.params.begin(), sig.params.end(),
                                    [](const Type* t) { return t->kind == K::Named || t->isError(); });
        if (!hasNamed) error(op.loc, "an operator outside a type must take at least one parameter of a named type");
        if (op.name == "string" && sig.ret != lang_.stringT && !sig.ret->isError())
            error(op.loc, "operator string must return string");
        if (op.name == "hash" && sig.ret != lang_.uintT && !sig.ret->isError())
            error(op.loc, "operator hash must return uint");
    }

    // Operators: 1 or 2 parameters, at least one of the owner's type.
    void checkOperatorShape(const FunctionDecl& op, const Type* owner) {
        size_t n = op.params.size();
        if (op.name == "hash") {
            const Sig& sig = sigOf(op, owner);
            if (n != 1 || sig.params[0] != owner)
                error(op.loc, "operator hash takes exactly one parameter of type " + str(owner));
            else if (sig.ret != lang_.uintT && !sig.ret->isError())
                error(op.loc, "operator hash must return uint");
            return;
        }
        if (op.name == "string") {
            const Sig& sig = sigOf(op, owner);
            if (n != 1 || sig.params[0] != owner)
                error(op.loc, "operator string takes exactly one parameter of type " + str(owner));
            else if (sig.ret != lang_.stringT && !sig.ret->isError())
                error(op.loc, "operator string must return string");
            return;
        }
        bool unaryOk = op.name == "-" || op.name == "!" || op.name == "~";
        if (n == 0 || n > 2 || (n == 1 && !unaryOk)) {
            error(op.loc, "operator " + q(op.name) + (unaryOk ? " takes 1 or 2 parameters" : " takes 2 parameters"));
            return;
        }
        const Sig& sig = sigOf(op, owner);
        bool mentionsOwner = false;
        for (const Type* p : sig.params) mentionsOwner |= p == owner || p->isError();
        if (!mentionsOwner)
            error(op.loc, "operator " + q(op.name) + " must take at least one " + str(owner) + " parameter");
        bool comparison = op.name == "==" || op.name == "<";
        if (comparison && sig.ret != lang_.boolT && !sig.ret->isError())
            error(op.loc, "operator " + q(op.name) + " must return bool");
    }

    // ================================================================== moves
    //
    // Values whose type needs cleanup ("resources": classes with `drop`, and
    // anything containing one) are *moved*, never implicitly copied:
    //   let b = a, b = a, return a, Some(a), Point(a, ...), f(a) where f takes `own`
    // After a move, the old name can't be used. Parameters, match bindings,
    // for-loop variables and self are *borrowed*: usable, but not movable.
    // Generic bodies are re-checked for each concrete instantiation.

public:
    void exportInfo(SemaInfo& info) const {
        for (const auto& [e, ci] : calls_)
            if (e->kind == ExprKind::Call) info.calls[e] = {ci.fn, ci.subst};
        info.viewCoerced = viewCoerced_;
        info.dynCoerced = dynCoerced_;
        info.consumingMatches = consumingMatches_;
    }

    void runMoveChecks() {
        // Baseline: every function body once, with generic parameters unknown.
        for (auto& m : prog_.modules) {
            mod_ = m.get();
            for (auto& f : m->files) {
                file_ = f.get();
                for (auto& d : f->ast.decls) forEachFunction(*d, [&](const FunctionDecl& fn) {
                    moveCheckFunction(fn, {}, "");
                });
            }
        }
        // Lambda bodies are functions of their own.
        for (const auto& l : lambdas_) {
            mod_ = l.mod;
            file_ = l.file;
            fnLocation_[l.fn] = {l.mod, l.file};
            moveCheckFunction(*l.fn, {}, "");
        }
        // Instantiations discovered along the way (worklist grows while we go).
        while (!pending_.empty()) {
            auto [fn, sub, origin] = pending_.back();
            pending_.pop_back();
            auto loc = fnLocation_.find(fn);
            if (loc == fnLocation_.end()) continue;
            mod_ = loc->second.first;
            file_ = loc->second.second;
            moveCheckFunction(*fn, sub, origin);
        }
    }

private:
    enum class VarState { Live, Moved, MaybeMoved };
    struct VarInfo {
        VarState state = VarState::Live;
        SourceLoc movedAt;
        bool borrowed = false;
        int loopDepth = 0;
    };
    using MoveState = std::map<const Symbol*, VarInfo>;

    struct Pending {
        const FunctionDecl* fn;
        Subst subst;
        std::string origin; // "max<string> (used at main.h2:12)"
    };
    std::vector<Pending> pending_;
    std::set<std::pair<const FunctionDecl*, Subst>> instantiated_;
    std::map<const FunctionDecl*, std::pair<ModuleInfo*, SourceFile*>> fnLocation_;
    std::map<const FunctionDecl*, const Symbol*> fnOwner_; // methods/operators -> their class/type
    std::set<std::string> reportedMoves_;
    std::map<const Type*, bool> resourceCache_;

    // Function values that may point at local variables (capturing lambdas,
    // function parameters, locals holding those): call or pass them, never store them.
    std::set<const Symbol*> nonEscaping_;

    bool isNonEscaping(const Expr& e) {
        if (e.kind == ExprKind::Lambda) return !static_cast<const LambdaExpr&>(e).fn->captures.empty();
        if (e.kind == ExprKind::Ident) return nonEscaping_.count(static_cast<const IdentExpr&>(e).sym) > 0;
        return false;
    }

    void escapeError(const Expr& e) {
        if (e.kind == ExprKind::Ident && moveFn_ &&
            std::any_of(moveFn_->params.begin(), moveFn_->params.end(),
                        [&](const Param& p) { return p.sym == static_cast<const IdentExpr&>(e).sym; })) {
            const std::string& n = static_cast<const IdentExpr&>(e).name;
            moveError(e.loc, q(n) + " is a function parameter, which may use its caller's local variables, so it "
                                    "can only be called or passed on. To store it, declare '" + n +
                                    ": own function(...)' (callers then can't pass lambdas that capture)");
            return;
        }
        moveError(e.loc, "this function value uses local variables from the code around it, so it can only be called "
                         "or passed to a function parameter: it can't be stored, returned, or given to an 'own' "
                         "parameter");
    }

    // Move-check state for the function being walked.
    Subst moveSubst_;
    std::string moveContext_;
    MoveState vars_;
    int moveLoop_ = 0;
    std::vector<MoveState> breakStates_;
    std::vector<MoveState> continueStates_;
    std::map<const Symbol*, const Type*> movedTypes_; // for hints at loop back-edges
    const FunctionDecl* moveFn_ = nullptr;

    template <class F>
    void forEachFunction(const Decl& d, F&& f) {
        auto visit = [&](const FunctionDecl& fn) {
            fnLocation_[&fn] = {mod_, file_};
            if (fn.body) f(fn);
        };
        switch (d.kind) {
        case DeclKind::Function: visit(static_cast<const FunctionDecl&>(d)); break;
        case DeclKind::Type:
            for (const auto& op : static_cast<const TypeDecl&>(d).operators) {
                fnOwner_[op.get()] = declSym_[&d];
                visit(*op);
            }
            break;
        case DeclKind::Class:
            for (const auto& m : static_cast<const ClassDecl&>(d).methods) {
                fnOwner_[m.get()] = declSym_[&d];
                visit(*m);
            }
            break;
        default: break;
        }
    }

    void moveError(SourceLoc loc, const std::string& msg) {
        std::string full = moveContext_.empty() ? msg : "in " + moveContext_ + ": " + msg;
        std::string key = file_->displayFile + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.col) + full;
        if (reportedMoves_.insert(key).second) error(loc, full);
    }

    // Does this type need cleanup, so that it must be moved rather than copied?
    bool isResource(const Type* t) {
        t = subst(t, moveSubst_);
        auto it = resourceCache_.find(t);
        if (it != resourceCache_.end()) return it->second;
        resourceCache_[t] = false; // breaks cycles (recursive types through ptr are fine)
        bool r = false;
        switch (t->kind) {
        case K::Array: r = isResource(t->elem()); break;
        case K::Named: {
            Subst s = substFor(t);
            const Type* selfT = selfTypeOf(t->sym);
            auto fieldsResource = [&](const std::vector<Field>& fields) {
                for (const auto& f : fields)
                    if (isResource(subst(convert(*f.type, selfT), s))) return true;
                return false;
            };
            if (t->sym->kind == Symbol::Kind::Class) {
                const auto* cd = declAs<ClassDecl>(t->sym);
                for (const auto& m : cd->methods)
                    if (!m->isOperator && m->name == "drop") r = true;
                r = r || fieldsResource(cd->fields);
            } else if (t->sym->kind == Symbol::Kind::Type) {
                r = fieldsResource(declAs<TypeDecl>(t->sym)->fields);
            } else if (t->sym->kind == Symbol::Kind::Enum) {
                for (const auto& v : declAs<EnumDecl>(t->sym)->variants)
                    for (const auto& p : v.payload)
                        if (isResource(subst(convert(*p, nullptr), s))) r = true;
            }
            break;
        }
        default: break;
        }
        return resourceCache_[t] = r;
    }

    bool hasCopyMethod(const Type* t) {
        t = subst(t, moveSubst_);
        if (t->kind != K::Named || t->sym->kind != Symbol::Kind::Class) return false;
        for (const auto& m : declAs<ClassDecl>(t->sym)->methods)
            if (!m->isOperator && m->name == "copy" && m->params.empty()) return true;
        return false;
    }

    // " Use x.copy() to make a copy." or a note that there's no copy().
    std::string copyHint(const std::string& expr, const Type* t) {
        if (hasCopyMethod(t)) return " Use " + expr + ".copy() to make a copy";
        return " (" + str(subst(t, moveSubst_)) + " has no copy() method)";
    }

    static std::string exprText(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Ident: return static_cast<const IdentExpr&>(e).name;
        case ExprKind::Self: return "self";
        case ExprKind::Member: {
            const auto& m = static_cast<const MemberExpr&>(e);
            return exprText(*m.object) + "." + m.member;
        }
        case ExprKind::Index: return exprText(*static_cast<const IndexExpr&>(e).object) + "[...]";
        default: return "the value";
        }
    }

    void moveCheckFunction(const FunctionDecl& fn, const Subst& sub, const std::string& origin) {
        moveSubst_ = sub;
        moveContext_ = origin;
        moveFn_ = &fn;
        vars_.clear();
        nonEscaping_.clear();
        moveLoop_ = 0;
        breakStates_.clear();
        // A lambda's captured variables belong to the enclosing function: borrowed here.
        for (const Symbol* c : fn.captures) vars_[c] = VarInfo{VarState::Live, {}, true, 0};
        for (const auto& p : fn.params) {
            if (!p.sym) continue;
            vars_[p.sym] = VarInfo{VarState::Live, {}, !p.isOwn, 0};
            // A function parameter may capture its caller's variables: it must not escape.
            auto t = localTypes_.find(p.sym);
            if (!p.isOwn && t != localTypes_.end() && t->second->kind == K::Function) nonEscaping_.insert(p.sym);
        }
        moveBlock(*fn.body);
    }

    // Composes a call's recorded bindings with the current instantiation and
    // queues the callee for checking with concrete resource types.
    void noteInstantiation(const CallInfo& ci, SourceLoc at) {
        if (ci.kind != CallInfo::Fn || !ci.fn || ci.subst.empty()) return;
        Subst composed;
        bool anyResource = false;
        for (const auto& [k, v] : ci.subst) {
            const Type* t = v ? subst(v, moveSubst_) : tc_.error();
            composed[k] = t;
            if (isResource(t)) anyResource = true;
        }
        if (!anyResource) return; // same as the baseline check
        if (!instantiated_.insert({ci.fn, composed}).second) return;
        // Name it like the user would: max<string>, or Box<string>.get
        auto argsOf = [&](const std::vector<GenericParam>& gs) {
            std::string a;
            for (const auto& g : gs) {
                auto it = composed.find(g.sym);
                a += (a.empty() ? "" : ", ") + (it != composed.end() ? str(it->second) : g.name);
            }
            return gs.empty() ? std::string() : "<" + a + ">";
        };
        std::string name = ci.fn->name + argsOf(ci.fn->generics);
        if (auto ow = fnOwner_.find(ci.fn); ow != fnOwner_.end() && ow->second)
            if (const auto* gs = genericsOf(ow->second))
                name = ow->second->name + argsOf(*gs) + "." + name;
        std::string origin = name + " (used at " + file_->displayFile + ":" + std::to_string(at.line) + ")";
        pending_.push_back({ci.fn, composed, origin});
    }

    static VarState mergeState(VarState a, VarState b) {
        if (a == b) return a;
        return VarState::MaybeMoved;
    }

    static void mergeInto(MoveState& into, const MoveState& other) {
        for (auto& [sym, info] : into) {
            auto it = other.find(sym);
            if (it == other.end()) continue;
            VarState merged = mergeState(info.state, it->second.state);
            if (merged != info.state && info.state == VarState::Live) info.movedAt = it->second.movedAt;
            info.state = merged;
        }
    }

    // Reads `sym`: an error if it has been moved.
    void useVar(const Symbol* sym, SourceLoc loc) {
        auto it = vars_.find(sym);
        if (it == vars_.end()) return;
        const VarInfo& v = it->second;
        if (v.state == VarState::Moved)
            moveError(loc, q(sym->name) + " was moved at line " + std::to_string(v.movedAt.line) +
                               ", so it can't be used here");
        else if (v.state == VarState::MaybeMoved)
            moveError(loc, q(sym->name) + " may have been moved (at line " + std::to_string(v.movedAt.line) +
                               ") on some paths, so it can't be used here");
    }

    // The root variable of a place expression (a, a.b, a[i]), if any.
    const Expr* placeRoot(const Expr& e) {
        const Expr* cur = &e;
        while (true) {
            if (cur->kind == ExprKind::Member && !static_cast<const MemberExpr*>(cur)->sym)
                cur = static_cast<const MemberExpr*>(cur)->object.get();
            else if (cur->kind == ExprKind::Index)
                cur = static_cast<const IndexExpr*>(cur)->object.get();
            else
                return cur;
        }
    }

    // Visits an expression. `consume`: the value is moved out of this position.
    void moveExpr(const Expr& e, bool consume) {
        if (consume && viewCoerced_.count(&e)) consume = false; // used as a str view: borrowed
        if (consume && isNonEscaping(e)) {
            escapeError(e);
            consume = false;
        }
        switch (e.kind) {
        case ExprKind::Ident: {
            const auto& id = static_cast<const IdentExpr&>(e);
            const Symbol* s = id.sym;
            if (!s || s->kind != Symbol::Kind::Local) return;
            useVar(s, e.loc);
            if (!consume || !e.type || !isResource(e.type)) return;
            auto it = vars_.find(s);
            if (it == vars_.end()) return;
            VarInfo& v = it->second;
            if (v.state != VarState::Live) return; // already reported
            if (v.borrowed) {
                bool isParam = std::any_of(moveFn_->params.begin(), moveFn_->params.end(),
                                           [&](const Param& p) { return p.sym == s; });
                std::string t = str(subst(e.type, moveSubst_));
                if (isParam)
                    moveError(e.loc, "cannot move " + q(s->name) + ": parameters are borrowed. Write '" + s->name +
                                         ": own " + t + "' to take ownership" +
                                         (hasCopyMethod(e.type) ? ", or use " + s->name + ".copy()" : ""));
                else if (matchBindings_.count(s))
                    moveError(e.loc, "cannot move " + q(s->name) + ": the match only borrows the value it matches "
                                     "(it isn't owned here). Match on a value you own or a temporary to take it." +
                                         copyHint(s->name, e.type));
                else
                    moveError(e.loc, "cannot move " + q(s->name) + ": it is borrowed, not owned." +
                                         copyHint(s->name, e.type));
                return;
            }
            v.state = VarState::Moved;
            v.movedAt = e.loc;
            movedTypes_[s] = e.type;
            return;
        }
        case ExprKind::Self:
            if (consume && e.type && isResource(e.type))
                moveError(e.loc, "cannot move 'self' out of a method." + copyHint("self", e.type));
            return;
        case ExprKind::Member: {
            const auto& m = static_cast<const MemberExpr&>(e);
            if (m.sym) return; // qualified name
            moveExpr(*m.object, false);
            if (consume && e.type && isResource(e.type) && isPlaceExpr(*placeRoot(e)))
                moveError(e.loc, "cannot move " + exprText(e) + " out of its owner." + copyHint(exprText(e), e.type));
            return;
        }
        case ExprKind::Index: {
            const auto& ix = static_cast<const IndexExpr&>(e);
            moveExpr(*ix.object, false);
            moveExpr(*ix.index, false);
            noteOperatorCall(e);
            if (consume && e.type && isResource(e.type) && isPlaceExpr(*placeRoot(e)))
                moveError(e.loc, "cannot move an element out of " + exprText(*ix.object) + "." +
                                     copyHint(exprText(e), e.type));
            return;
        }
        case ExprKind::Unary: {
            const auto& u = static_cast<const UnaryExpr&>(e);
            moveOperands({u.operand.get()}, e);
            if (u.op == "*" && consume && e.type && isResource(e.type))
                moveError(e.loc, "cannot move a value out through a pointer." + copyHint("the value", e.type));
            return;
        }
        case ExprKind::Binary: {
            const auto& b = static_cast<const BinaryExpr&>(e);
            moveOperands({b.lhs.get(), b.rhs.get()}, e);
            return;
        }
        case ExprKind::Call: moveCall(static_cast<const CallExpr&>(e)); return;
        case ExprKind::Try: moveExpr(*static_cast<const TryExpr&>(e).operand, true); return;
        case ExprKind::Alloc: {
            const auto& a = static_cast<const AllocExpr&>(e);
            if (a.init) moveExpr(*a.init, true);
            return;
        }
        case ExprKind::ArrayLit:
            for (const auto& x : static_cast<const ArrayLitExpr&>(e).elems) moveExpr(*x, true);
            return;
        case ExprKind::Match: {
            const auto& m = static_cast<const MatchExpr&>(e);
            // A match takes ownership of its subject (and its bindings own their
            // values) when the subject is a temporary, or an owned variable that
            // some arm binds a value out of. Otherwise bindings are borrowed.
            bool consuming = false;
            if (isTemporary(*m.subject)) {
                consuming = true;
                moveExpr(*m.subject, true);
            } else if (m.subject->kind == ExprKind::Ident && armsBind(m)) {
                auto it = vars_.find(static_cast<const IdentExpr&>(*m.subject).sym);
                consuming = it != vars_.end() && !it->second.borrowed;
                moveExpr(*m.subject, consuming);
            } else {
                moveExpr(*m.subject, false);
            }
            if (consuming) consumingMatches_.insert(&e);
            MoveState before = vars_;
            std::optional<MoveState> after;
            for (const auto& arm : m.arms) {
                vars_ = before;
                bindPattern(*arm.pattern, !consuming);
                bool reachable = true;
                if (arm.block) reachable = moveBlock(*arm.block);
                if (arm.expr) moveExpr(*arm.expr, consume);
                if (!reachable) continue;
                if (!after) after = vars_;
                else mergeInto(*after, vars_);
            }
            vars_ = after ? *after : before;
            return;
        }
        default: return; // literals
        }
    }

    bool isPlaceExpr(const Expr& e) {
        if (e.kind == ExprKind::Self) return true;
        if (e.kind == ExprKind::Ident) {
            const Symbol* s = static_cast<const IdentExpr&>(e).sym;
            return s && s->kind == Symbol::Kind::Local;
        }
        return e.kind == ExprKind::Unary && static_cast<const UnaryExpr&>(e).op == "*";
    }

    void noteOperatorCall(const Expr& e) {
        if (auto it = calls_.find(&e); it != calls_.end()) noteInstantiation(it->second, e.loc);
    }

    // Operator operands are borrowed unless the operator declares `own`.
    void moveOperands(std::vector<const Expr*> operands, const Expr& opExpr) {
        auto it = calls_.find(&opExpr);
        const FunctionDecl* fn = it != calls_.end() ? it->second.fn : nullptr;
        for (size_t i = 0; i < operands.size(); ++i)
            moveExpr(*operands[i], fn && i < fn->params.size() && fn->params[i].isOwn);
    }

    void moveCall(const CallExpr& c) {
        auto it = calls_.find(&c);
        CallInfo ci = it != calls_.end() ? it->second : CallInfo{};
        // Method receiver: borrowed.
        if (c.callee->kind == ExprKind::Member && !static_cast<const MemberExpr&>(*c.callee).sym)
            moveExpr(*static_cast<const MemberExpr&>(*c.callee).object, false);
        for (size_t i = 0; i < c.args.size(); ++i) {
            bool consume = ci.kind == CallInfo::ConsumeAll ||
                           (ci.kind == CallInfo::Fn && ci.fn && i < ci.fn->params.size() && ci.fn->params[i].isOwn);
            moveExpr(*c.args[i], consume);
        }
        noteInstantiation(ci, c.loc);
    }

    std::set<const Symbol*> matchBindings_;

    void bindPattern(const Pattern& p, bool borrowed) {
        if (p.kind == Pattern::Kind::Binding && p.sym && p.sym->kind == Symbol::Kind::Local) {
            vars_[p.sym] = VarInfo{VarState::Live, {}, borrowed, moveLoop_};
            matchBindings_.insert(p.sym);
        }
        for (const auto& f : p.fields) bindPattern(*f, borrowed);
    }

    std::set<const Expr*> consumingMatches_;

    // Not a place: a call result, an operator result, a literal, ...
    static bool isTemporary(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Ident:
        case ExprKind::Self:
        case ExprKind::Index: return false;
        case ExprKind::Member: return static_cast<const MemberExpr&>(e).sym != nullptr;
        case ExprKind::Unary: return static_cast<const UnaryExpr&>(e).op != "*";
        default: return true;
        }
    }

    static bool patternBinds(const Pattern& p) {
        if (p.kind == Pattern::Kind::Binding && p.sym && p.sym->kind == Symbol::Kind::Local) return true;
        for (const auto& f : p.fields)
            if (patternBinds(*f)) return true;
        return false;
    }

    static bool armsBind(const MatchExpr& m) {
        for (const auto& arm : m.arms)
            if (patternBinds(*arm.pattern)) return true;
        return false;
    }

    // Returns false if the end of the block is unreachable (return/break/continue).
    bool moveBlock(const Block& b) {
        for (const auto& s : b.stmts)
            if (!moveStmt(*s)) return false;
        return true;
    }

    bool moveStmt(const Stmt& s) {
        switch (s.kind) {
        case StmtKind::Block: return moveBlock(static_cast<const Block&>(s));
        case StmtKind::Let: {
            const auto& l = static_cast<const LetStmt&>(s);
            if (l.init && isNonEscaping(*l.init)) {
                // A local may hold it (it can't outlive this function); it stays non-escaping.
                if (l.sym) nonEscaping_.insert(l.sym);
                moveExpr(*l.init, false);
                if (l.sym) vars_[l.sym] = VarInfo{VarState::Live, {}, false, moveLoop_};
                return true;
            }
            if (l.init) moveExpr(*l.init, true);
            if (l.sym) vars_[l.sym] = VarInfo{VarState::Live, {}, false, moveLoop_};
            return true;
        }
        case StmtKind::Assign: {
            const auto& a = static_cast<const AssignStmt&>(s);
            moveExpr(*a.value, a.op == "=");
            if (a.target->kind == ExprKind::Ident && a.op == "=") {
                // Assigning a new value brings a moved variable back to life.
                const Symbol* t = static_cast<const IdentExpr&>(*a.target).sym;
                auto it = vars_.find(t);
                if (it != vars_.end() && !it->second.borrowed) it->second.state = VarState::Live;
                return true;
            }
            moveExpr(*a.target, false);
            return true;
        }
        case StmtKind::Expr: moveExpr(*static_cast<const ExprStmt&>(s).expr, false); return true;
        case StmtKind::If: {
            const auto& i = static_cast<const IfStmt&>(s);
            moveExpr(*i.cond, false);
            MoveState before = vars_;
            bool thenReach = moveBlock(*i.then);
            MoveState thenState = vars_;
            vars_ = before;
            bool elseReach = i.otherwise ? moveStmt(*i.otherwise) : true;
            if (!thenReach && !elseReach) return false;
            if (!thenReach) return true; // vars_ already holds the else state
            if (!elseReach) {
                vars_ = thenState;
                return true;
            }
            mergeInto(vars_, thenState);
            return true;
        }
        case StmtKind::While:
        case StmtKind::For: {
            const Block* body;
            if (s.kind == StmtKind::While) {
                const auto& w = static_cast<const WhileStmt&>(s);
                moveExpr(*w.cond, false);
                body = w.body.get();
            } else {
                const auto& f = static_cast<const ForStmt&>(s);
                moveExpr(*f.iterable, false);
                body = f.body.get();
                if (f.sym) vars_[f.sym] = VarInfo{VarState::Live, {}, /*borrowed=*/true, moveLoop_ + 1};
            }
            MoveState before = vars_;
            ++moveLoop_;
            breakStates_.emplace_back();
            continueStates_.emplace_back();
            bool reach = moveBlock(*body);
            // The back edge: end of the body, plus every `continue`.
            MoveState back = before;
            bool loopsBack = reach || !continueStates_.back().empty();
            if (reach) back = vars_;
            if (!continueStates_.back().empty()) {
                if (reach) mergeInto(back, continueStates_.back());
                else back = continueStates_.back();
            }
            if (loopsBack)
                for (const auto& [sym, info] : back) {
                    auto b = before.find(sym);
                    if (b == before.end() || b->second.state != VarState::Live || info.state == VarState::Live)
                        continue;
                    moveError(info.movedAt, q(sym->name) + " is moved inside the loop and not replaced before the "
                                            "next iteration." + copyHint(sym->name, movedTypes_[sym]));
                }
            MoveState exitState = before;
            if (loopsBack) mergeInto(exitState, back);
            if (!breakStates_.back().empty()) mergeInto(exitState, breakStates_.back());
            breakStates_.pop_back();
            continueStates_.pop_back();
            --moveLoop_;
            vars_ = exitState;
            return true;
        }
        case StmtKind::Return: {
            const auto& r = static_cast<const ReturnStmt&>(s);
            if (r.value && viewCoerced_.count(r.value.get())) {
                // Returning a str view is only safe if the string outlives this
                // function: a borrowed parameter, or something reached through one.
                const Expr* root = placeRoot(*r.value);
                bool borrowedRoot = false;
                if (root->kind == ExprKind::Ident) {
                    auto it = vars_.find(static_cast<const IdentExpr&>(*root).sym);
                    borrowedRoot = it != vars_.end() && it->second.borrowed;
                } else if (root->kind == ExprKind::Self) {
                    borrowedRoot = true;
                }
                if (!borrowedRoot) {
                    std::string what = r.value->kind == ExprKind::Ident
                                           ? q(static_cast<const IdentExpr&>(*r.value).name)
                                           : "a temporary string";
                    moveError(r.value->loc, "returns a str view of " + what + ", which is freed when the function "
                                            "ends. Return the string itself (-> string) instead");
                }
            }
            if (r.value) moveExpr(*r.value, true);
            return false;
        }
        case StmtKind::Break:
            if (!breakStates_.empty()) {
                if (breakStates_.back().empty()) breakStates_.back() = vars_;
                else mergeInto(breakStates_.back(), vars_);
            }
            return false;
        case StmtKind::Continue:
            if (!continueStates_.empty()) {
                if (continueStates_.back().empty()) continueStates_.back() = vars_;
                else mergeInto(continueStates_.back(), vars_);
            }
            return false;
        case StmtKind::Free: moveExpr(*static_cast<const FreeStmt&>(s).target, false); return true;
        case StmtKind::Asm: return true;
        }
        return true;
    }
};

} // namespace

void typecheckProgram(Program& program, TypeContext& types, Diagnostics& diags, std::vector<LocalTypeNote>* notes,
                      SemaInfo* info) {
    Checker checker(program, types, diags, notes);
    checker.run();
    checker.runMoveChecks();
    if (info) checker.exportInfo(*info);
}

} // namespace h2
