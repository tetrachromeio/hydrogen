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

    void error(SourceLoc loc, const std::string& msg) { diags_.error(file_->displayFile, loc, msg); }
    static std::string str(const Type* t) { return TypeContext::str(t); }

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
        lang_.stringT = get("string");
        lang_.intT = get("int");
        lang_.uintT = get("uint");
        lang_.floatT = get("float");
        if (atomic) {
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
        return true;
    }

    std::string conversionHint(const Type* from, const Type* to) {
        if (from->isError() || to->isError()) return "";
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
        } else if (owner->kind == K::Generic) {
            // Interface signatures use Self, which stands for the generic parameter.
            for (auto& [fn, s] : constraintOperators(owner))
                if (auto f = tryFn(fn, s, owner)) return f;
        }
        return std::nullopt;
    }

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
        case Symbol::Kind::Function:
            error(loc, "functions can't be used as values yet; call it: " + s->name + "(...)");
            return tc_.error();
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
        const Type* l = checkExpr(*b.lhs, arithmetic ? expected : nullptr);
        const Type* r = checkExpr(*b.rhs, arithmetic ? expected : nullptr);
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
        if (auto f = findBinary(op, l, r)) return f->result;
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
        error(ix.loc, str(obj) + " cannot be indexed" + operatorHint(obj, "[]"));
        return tc_.error();
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
            }
        }
        return ok;
    }

    // Structural interface check: does `t` provide every member of each constraint?
    void checkConstraints(const GenericParam& g, const Type* t, SourceLoc loc) {
        if (t->isError()) return;
        for (const auto& c : g.constraints) {
            if (!c->sym || c->sym->kind != Symbol::Kind::Interface) continue;
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
            if (base->kind == K::Named && base->sym->kind == Symbol::Kind::Class) {
                for (const auto& fn : declAs<ClassDecl>(base->sym)->methods) {
                    if (fn->isOperator || fn->name != m.member) continue;
                    checkMemberAccess(base->sym, fn->isPub, "method", fn->name, m.loc);
                    const Sig& sig = sigOf(*fn, selfTypeOf(base->sym));
                    Subst s = substFor(base);
                    std::set<const Symbol*> vars;
                    for (const auto& g : fn->generics) vars.insert(g.sym);
                    checkArgs(c, sig.params, s, vars, sig.ret, expected, m.member, &fn->generics);
                    m.type = tc_.voidT();
                    return subst(sig.ret, s);
                }
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
            std::vector<const Type*> args;
            for (const auto& g : *gs) args.push_back(sub[g.sym]);
            return tc_.named(enumSym, std::move(args));
        }
        case Symbol::Kind::Class:
        case Symbol::Kind::Type: return checkConstruct(c, s, explicitArgs, expected);
        case Symbol::Kind::Local: {
            auto it = localTypes_.find(s);
            if (it != localTypes_.end() && !it->second->isError())
                error(c.loc, q(s->name) + " is a variable of type " + str(it->second) + ", not a function");
            return argsUnchecked(c);
        }
        case Symbol::Kind::Generic:
            error(c.loc, "cannot construct generic type " + s->name);
            return argsUnchecked(c);
        default: return argsUnchecked(c);
        }
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
        if (s->kind == Symbol::Kind::Class) {
            for (const auto& fn : declAs<ClassDecl>(s)->methods)
                if (!fn->isOperator && fn->name == "init") {
                    params = sigOf(*fn, selfT).params;
                    usesInit = true;
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
            else if (!it->isError()) error(f.iterable->loc, "cannot loop over " + str(it) +
                                                             "; 'for' works with ranges (a..b), arrays and strings");
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
        if (!f.body) return;
        const Sig& sig = sigOf(f, ownerSelf);
        const Type* savedRet = retType_;
        const Type* savedSelf = selfType_;
        retType_ = sig.ret;
        selfType_ = ownerSelf;
        for (size_t i = 0; i < f.params.size(); ++i)
            if (f.params[i].sym) localTypes_[f.params[i].sym] = sig.params[i];
        checkBlock(*f.body);
        if (sig.ret != tc_.voidT() && !sig.ret->isError() && !alwaysReturns(*f.body))
            error(f.body->loc, "function " + q(f.name) + " must return a value of type " + str(sig.ret) +
                                   " on every path; add a return at the end");
        retType_ = savedRet;
        selfType_ = savedSelf;
    }

    void checkDecl(Decl& d) {
        switch (d.kind) {
        case DeclKind::Function: {
            auto& f = static_cast<FunctionDecl&>(d);
            if (!f.isOperator) checkFunction(f, nullptr);
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

    // Operators: 1 or 2 parameters, at least one of the owner's type.
    void checkOperatorShape(const FunctionDecl& op, const Type* owner) {
        size_t n = op.params.size();
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
};

} // namespace

void typecheckProgram(Program& program, TypeContext& types, Diagnostics& diags, std::vector<LocalTypeNote>* notes) {
    Checker(program, types, diags, notes).run();
}

} // namespace h2
