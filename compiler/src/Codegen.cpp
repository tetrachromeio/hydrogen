// LLVM IR generation (textual).
//
// Conventions
//  - Every generic function/class is monomorphized: one IR function per
//    concrete instantiation, named like "max<Atomic.int>".
//  - Scalars (bits/float types, pointers) are passed by value. Aggregates
//    (layout types, classes, enums, arrays, slices, ranges) are passed by
//    pointer: a borrow points at the caller's storage; for an `own`
//    parameter the caller hands over a copy it will no longer drop.
//  - Methods take `ptr %self` first. `init` fills in a zeroed object.
//  - Values whose type needs cleanup ("resources") get a drop flag per
//    local. Moving clears the flag; scope exit drops values whose flag is set.
//    Temporaries that need cleanup are dropped at the end of their statement.

#include "hydrogen/Codegen.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <sstream>

namespace h2 {
namespace {

using Subst = std::map<const Symbol*, const Type*>;
using K = Type::Kind;

struct Val {
    std::string ty; // LLVM type
    std::string v;  // SSA value or constant
};

struct Local {
    std::string ptr;         // address of the value
    const Type* type = nullptr;
    std::string flag;        // drop flag (i1 alloca), empty if not a resource or borrowed
};

struct Temp {
    std::string ptr;
    const Type* type;
    std::string flag;
};

struct LoopCtx {
    std::string breakLabel, continueLabel;
    size_t scopeDepth;
};

struct FnCtx {
    std::ostringstream allocas;
    std::ostringstream body;
    int tmp = 0;
    int label = 0;
    bool terminated = false;
    Subst subst;
    const Type* owner = nullptr;   // owning type for methods/operators
    std::string selfPtr;
    const Type* retType = nullptr;
    const FunctionDecl* fn = nullptr;
    std::map<const Symbol*, Local> locals;
    std::vector<std::vector<const Symbol*>> scopes; // owned locals per scope
    std::vector<Temp> temps;                        // temporaries of the current statement
    std::vector<LoopCtx> loops;
};

std::string llvmQuote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\' || c < 0x20 || c >= 0x7f) {
            static const char* hex = "0123456789ABCDEF";
            out += '\\';
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

bool parseInt(const std::string& text, uint64_t& out) {
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
        out = out * base + d;
    }
    return true;
}

// LLVM wants float constants as exact hex doubles.
std::string floatConst(double v, bool isFloat32) {
    if (isFloat32) v = static_cast<double>(static_cast<float>(v));
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    char buf[32];
    snprintf(buf, sizeof buf, "0x%016llX", static_cast<unsigned long long>(bits));
    return buf;
}

// Decodes the UTF-8 bytes of a char literal into a code point.
uint32_t decodeChar(const std::string& s) {
    if (s.empty()) return 0;
    auto b = [&](size_t i) { return i < s.size() ? static_cast<unsigned char>(s[i]) : 0u; };
    unsigned c = b(0);
    if (c < 0x80) return c;
    if ((c >> 5) == 6) return ((c & 0x1F) << 6) | (b(1) & 0x3F);
    if ((c >> 4) == 14) return ((c & 0x0F) << 12) | ((b(1) & 0x3F) << 6) | (b(2) & 0x3F);
    return ((c & 0x07) << 18) | ((b(1) & 0x3F) << 12) | ((b(2) & 0x3F) << 6) | (b(3) & 0x3F);
}

class Codegen {
public:
    Codegen(Program& p, TypeContext& tc, const SemaInfo& info, Diagnostics& d, bool checks)
        : prog_(p), tc_(tc), info_(info), diags_(d), checks_(checks) {}

    bool run(std::ostream& out) {
        for (auto& m : prog_.modules) {
            for (auto& [name, s] : m->defs) declSym_[s->decl] = s;
            for (auto& f : m->files)
                for (auto& d : f->ast.decls) indexDecl(*d, m.get(), f.get());
        }
        findLang();

        ModuleInfo* main = prog_.main();
        auto it = main->defs.find("main");
        if (it == main->defs.end() || it->second->kind != Symbol::Kind::Function) {
            diags_.error(main->displayFile(), {1, 1}, "the program has no 'main' function to run");
            return false;
        }
        const auto* mainFn = static_cast<const FunctionDecl*>(it->second->decl);
        std::string userMain = requestFn(mainFn, {}, nullptr);
        drainWorklist();

        const Type* mainRet = mainFn->returnType ? typeOfRef(*mainFn->returnType, {}, nullptr) : tc_.voidT();
        std::ostringstream entry;
        entry << "define i32 @main() {\n";
        if (mainRet == tc_.voidT()) {
            entry << "  call void @" << llvmQuote(userMain) << "()\n  ret i32 0\n";
        } else {
            std::string rt = lty(mainRet);
            entry << "  %r = call " << rt << " @" << llvmQuote(userMain) << "()\n";
            if (rt == "i32") entry << "  ret i32 %r\n";
            else entry << "  %t = trunc " << rt << " %r to i32\n  ret i32 %t\n";
        }
        entry << "}\n";

        out << "; Generated by h2c from " << main->displayFile() << "\n\n";
        out << types_.str() << "\n" << globals_.str() << "\n" << funcs_.str() << entry.str() << "\n";
        out << declarations();
        return !failed_;
    }

private:
    Program& prog_;
    TypeContext& tc_;
    const SemaInfo& info_;
    Diagnostics& diags_;
    bool checks_;
    bool failed_ = false;
    std::set<std::string> rawDecls_; // extra declarations (overflow intrinsics, trap)

    std::ostringstream types_, globals_, funcs_;
    std::set<std::string> structsDone_;
    std::set<std::string> fnsRequested_;
    std::set<std::string> dropsDone_;
    std::set<std::string> usedDecls_;
    std::set<std::string> externDecls_;
    int strCounter_ = 0;

    struct Inst {
        const FunctionDecl* fn;
        Subst subst;
        const Type* owner;
        std::string name;
    };
    std::vector<Inst> worklist_;

    std::map<const Decl*, const Symbol*> declSym_;
    std::map<const FunctionDecl*, const Symbol*> fnOwner_;
    std::map<const FunctionDecl*, ModuleInfo*> fnModule_;
    std::map<const FunctionDecl*, SourceFile*> fnFile_;

    struct Lang {
        const Type *boolT = nullptr, *charT = nullptr, *strT = nullptr, *stringT = nullptr, *intT = nullptr,
                   *uintT = nullptr, *floatT = nullptr;
    } lang_;

    FnCtx* f_ = nullptr; // function being emitted
    std::map<const Type*, bool> resourceCache_;

    // ------------------------------------------------------------ setup

    void indexDecl(const Decl& d, ModuleInfo* m, SourceFile* file) {
        auto note = [&](const FunctionDecl& fn, const Symbol* owner) {
            fnModule_[&fn] = m;
            fnFile_[&fn] = file;
            if (owner) fnOwner_[&fn] = owner;
        };
        switch (d.kind) {
        case DeclKind::Function: note(static_cast<const FunctionDecl&>(d), nullptr); break;
        case DeclKind::Type:
            for (const auto& op : static_cast<const TypeDecl&>(d).operators) note(*op, declSym_[&d]);
            break;
        case DeclKind::Class:
            for (const auto& fn : static_cast<const ClassDecl&>(d).methods) note(*fn, declSym_[&d]);
            break;
        default: break;
        }
    }

    void findLang() {
        ModuleInfo* atomic = nullptr;
        for (auto& m : prog_.modules)
            if (m->path == "Atomic") atomic = m.get();
        if (!atomic) return;
        auto get = [&](const char* n) -> const Type* {
            auto it = atomic->defs.find(n);
            return it == atomic->defs.end() ? nullptr : typeOfSym(it->second, {});
        };
        lang_.boolT = get("bool");
        lang_.charT = get("char");
        lang_.strT = get("str");
        for (auto& m : prog_.modules)
            if (m->path == "Elemental")
                if (auto it = m->defs.find("string"); it != m->defs.end()) lang_.stringT = typeOfSym(it->second, {});
        lang_.intT = get("int");
        lang_.uintT = get("uint");
        lang_.floatT = get("float");
    }

    void unsupported(SourceLoc loc, const std::string& what) {
        std::string file = f_ && f_->fn && fnFile_.count(f_->fn) ? fnFile_[f_->fn]->displayFile : prog_.main()->displayFile();
        diags_.error(file, loc, what + " is not supported by the code generator yet");
        failed_ = true;
    }

    // ------------------------------------------------------------ types

    template <class D>
    static const D* declAs(const Symbol* s) { return static_cast<const D*>(s->decl); }

    const std::vector<GenericParam>* genericsOf(const Symbol* s) {
        if (!s || !s->decl) return nullptr;
        switch (s->decl->kind) {
        case DeclKind::Type: return &declAs<TypeDecl>(s)->generics;
        case DeclKind::Class: return &declAs<ClassDecl>(s)->generics;
        case DeclKind::Enum: return &declAs<EnumDecl>(s)->generics;
        default: return nullptr;
        }
    }

    Subst substFor(const Type* t) {
        Subst s;
        if (!t || t->kind != K::Named) return s;
        if (const auto* gs = genericsOf(t->sym))
            for (size_t i = 0; i < gs->size() && i < t->args.size(); ++i) s[(*gs)[i].sym] = t->args[i];
        return s;
    }

    const Type* typeOfSym(const Symbol* s, std::vector<const Type*> args) {
        if (s->kind == Symbol::Kind::Type) {
            const auto* td = declAs<TypeDecl>(s);
            if (td->base == TypeDecl::Base::Alias) return typeOfRef(*td->aliasOf, {}, nullptr);
        }
        return tc_.named(s, std::move(args));
    }

    const Type* typeOfRef(const TypeRef& r, const Subst& s, const Type* self) {
        if (r.kind == TypeRef::Kind::Array) {
            const Type* elem = typeOfRef(*r.elem, s, self);
            if (!r.size) return tc_.slice(elem);
            uint64_t n = 0;
            if (r.size->kind == ExprKind::IntLit) parseInt(static_cast<const LiteralExpr&>(*r.size).value, n);
            return tc_.array(elem, n);
        }
        const Symbol* sym = r.sym;
        if (!sym) return tc_.error();
        std::vector<const Type*> args;
        for (const auto& a : r.args) args.push_back(typeOfRef(*a, s, self));
        switch (sym->kind) {
        case Symbol::Kind::Builtin:
            if (sym->name == "ptr") return tc_.ptr(args.empty() ? tc_.error() : args[0]);
            if (sym->name == "Self") return self ? self : tc_.error();
            return tc_.error();
        case Symbol::Kind::Generic: {
            auto it = s.find(sym);
            return it != s.end() ? it->second : tc_.generic(sym);
        }
        case Symbol::Kind::Type:
        case Symbol::Kind::Class:
        case Symbol::Kind::Enum: return typeOfSym(sym, std::move(args));
        default: return tc_.error();
        }
    }

    const Type* subst(const Type* t, const Subst& s) {
        if (!t) return tc_.error();
        switch (t->kind) {
        case K::Generic: {
            auto it = s.find(t->sym);
            return it != s.end() ? it->second : t;
        }
        case K::Named: {
            if (t->args.empty()) return t;
            std::vector<const Type*> args;
            for (const Type* a : t->args) args.push_back(subst(a, s));
            return tc_.named(t->sym, std::move(args));
        }
        case K::Ptr: return tc_.ptr(subst(t->elem(), s));
        case K::Array: return tc_.array(subst(t->elem(), s), t->size);
        case K::Slice: return tc_.slice(subst(t->elem(), s));
        case K::Range: return tc_.range(subst(t->elem(), s));
        case K::IntLit: return lang_.intT ? lang_.intT : t;
        case K::FloatLit: return lang_.floatT ? lang_.floatT : t;
        default: return t;
        }
    }

    // The concrete type of an expression in the current instantiation.
    const Type* typeOf(const Expr& e) { return subst(e.type, f_->subst); }

    std::string typeName(const Type* t) {
        switch (t->kind) {
        case K::Named: {
            std::string n = t->sym->module ? t->sym->module->displayName() + "." + t->sym->name : t->sym->name;
            if (!t->args.empty()) {
                n += "<";
                for (size_t i = 0; i < t->args.size(); ++i) n += (i ? "," : "") + typeName(t->args[i]);
                n += ">";
            }
            return n;
        }
        case K::Ptr: return "ptr<" + typeName(t->elem()) + ">";
        case K::Array: return "[" + typeName(t->elem()) + ";" + std::to_string(t->size) + "]";
        case K::Slice: return "[" + typeName(t->elem()) + "]";
        case K::Range: return "Range<" + typeName(t->elem()) + ">";
        default: return TypeContext::str(t);
        }
    }

    struct ScalarInfo {
        enum Kind { None, Int, Float } kind = None;
        unsigned bits = 0;
        bool isSigned = false;
    };

    ScalarInfo scalarOf(const Type* t, int depth = 0) {
        if (t->kind != K::Named || t->sym->kind != Symbol::Kind::Type || depth > 32) return {};
        const auto* td = declAs<TypeDecl>(t->sym);
        switch (td->base) {
        case TypeDecl::Base::Bits: return {ScalarInfo::Int, td->bits, td->isSigned};
        case TypeDecl::Base::Float: return {ScalarInfo::Float, td->bits, true};
        case TypeDecl::Base::Distinct: return scalarOf(typeOfRef(*td->aliasOf, {}, nullptr), depth + 1);
        default: return {};
        }
    }

    bool isAggregate(const Type* t) {
        switch (t->kind) {
        case K::Array:
        case K::Slice:
        case K::Range: return true;
        case K::Named:
            if (t->sym->kind == Symbol::Kind::Class || t->sym->kind == Symbol::Kind::Enum) return true;
            return scalarOf(t).kind == ScalarInfo::None;
        default: return false;
        }
    }

    std::string lty(const Type* t) {
        switch (t->kind) {
        case K::Void:
        case K::Error: return "void";
        case K::IntLit: return "i64";
        case K::FloatLit: return "double";
        case K::Ptr: return "ptr";
        case K::Array: return "[" + std::to_string(t->size) + " x " + lty(t->elem()) + "]";
        case K::Slice: return "{ ptr, i64 }";
        case K::Range: return "{ " + lty(t->elem()) + ", " + lty(t->elem()) + " }";
        case K::Generic: return "i64";
        case K::Named: {
            ScalarInfo s = scalarOf(t);
            if (s.kind == ScalarInfo::Int) return "i" + std::to_string(s.bits);
            if (s.kind == ScalarInfo::Float) return s.bits == 32 ? "float" : "double";
            ensureStruct(t);
            return "%" + llvmQuote(typeName(t));
        }
        }
        return "void";
    }

    std::vector<const Type*> fieldTypes(const Type* t) {
        std::vector<const Type*> out;
        Subst s = substFor(t);
        const std::vector<Field>* fields = nullptr;
        if (t->sym->kind == Symbol::Kind::Class) fields = &declAs<ClassDecl>(t->sym)->fields;
        else if (t->sym->kind == Symbol::Kind::Type) fields = &declAs<TypeDecl>(t->sym)->fields;
        if (fields)
            for (const auto& f : *fields) out.push_back(typeOfRef(*f.type, s, t));
        return out;
    }

    int fieldIndex(const Type* t, const std::string& name) {
        const std::vector<Field>* fields = nullptr;
        if (t->sym->kind == Symbol::Kind::Class) fields = &declAs<ClassDecl>(t->sym)->fields;
        else if (t->sym->kind == Symbol::Kind::Type) fields = &declAs<TypeDecl>(t->sym)->fields;
        if (fields)
            for (size_t i = 0; i < fields->size(); ++i)
                if ((*fields)[i].name == name) return static_cast<int>(i);
        return -1;
    }

    std::vector<const Type*> payloadTypes(const Type* enumT, const Variant& v) {
        std::vector<const Type*> out;
        Subst s = substFor(enumT);
        for (const auto& p : v.payload) out.push_back(typeOfRef(*p, s, nullptr));
        return out;
    }

    std::string payloadStruct(const Type* enumT, const Variant& v) {
        std::string s = "{ ";
        auto ts = payloadTypes(enumT, v);
        for (size_t i = 0; i < ts.size(); ++i) s += (i ? ", " : "") + lty(ts[i]);
        return ts.empty() ? "{}" : s + " }";
    }

    void ensureStruct(const Type* t) {
        std::string name = typeName(t);
        if (structsDone_.count(name)) return;
        structsDone_.insert(name);
        std::string def;
        if (t->sym->kind == Symbol::Kind::Enum) {
            uint64_t maxSize = 0;
            for (const auto& v : declAs<EnumDecl>(t->sym)->variants) {
                uint64_t size = 0, align = 1;
                for (const Type* p : payloadTypes(t, v)) {
                    uint64_t a = alignOf(p);
                    size = (size + a - 1) / a * a + sizeOf(p);
                    align = std::max(align, a);
                }
                maxSize = std::max(maxSize, (size + align - 1) / align * align);
            }
            uint64_t words = std::max<uint64_t>(1, (maxSize + 7) / 8);
            def = "{ i32, [" + std::to_string(words) + " x i64] }";
        } else {
            def = "{ ";
            auto fs = fieldTypes(t);
            for (size_t i = 0; i < fs.size(); ++i) def += (i ? ", " : "") + lty(fs[i]);
            def = fs.empty() ? "{}" : def + " }";
        }
        types_ << "%" << llvmQuote(name) << " = type " << def << "\n";
    }

    uint64_t alignOf(const Type* t) {
        switch (t->kind) {
        case K::Ptr:
        case K::Slice: return 8;
        case K::Array: return alignOf(t->elem());
        case K::Range: return alignOf(t->elem());
        case K::Named: {
            ScalarInfo s = scalarOf(t);
            if (s.kind != ScalarInfo::None) return std::max<uint64_t>(1, std::min<uint64_t>(8, (s.bits + 7) / 8));
            if (t->sym->kind == Symbol::Kind::Enum) return 8;
            uint64_t a = 1;
            for (const Type* f : fieldTypes(t)) a = std::max(a, alignOf(f));
            return a;
        }
        default: return 8;
        }
    }

    uint64_t sizeOf(const Type* t) {
        switch (t->kind) {
        case K::Ptr: return 8;
        case K::Slice: return 16;
        case K::Array: return sizeOf(t->elem()) * t->size;
        case K::Range: return 2 * sizeOf(t->elem());
        case K::Named: {
            ScalarInfo s = scalarOf(t);
            if (s.kind != ScalarInfo::None) {
                uint64_t bytes = (s.bits + 7) / 8;
                uint64_t p = 1;
                while (p < bytes) p *= 2;
                return p;
            }
            if (t->sym->kind == Symbol::Kind::Enum) {
                ensureStruct(t);
                uint64_t maxSize = 0;
                for (const auto& v : declAs<EnumDecl>(t->sym)->variants) {
                    uint64_t size = 0;
                    for (const Type* p : payloadTypes(t, v)) size = (size + alignOf(p) - 1) / alignOf(p) * alignOf(p) + sizeOf(p);
                    maxSize = std::max(maxSize, size);
                }
                return 8 + std::max<uint64_t>(1, (maxSize + 7) / 8) * 8;
            }
            uint64_t size = 0, align = 1;
            for (const Type* f : fieldTypes(t)) {
                uint64_t a = alignOf(f);
                size = (size + a - 1) / a * a + sizeOf(f);
                align = std::max(align, a);
            }
            return (size + align - 1) / align * align;
        }
        default: return 8;
        }
    }

    bool isResource(const Type* t) {
        auto it = resourceCache_.find(t);
        if (it != resourceCache_.end()) return it->second;
        resourceCache_[t] = false;
        bool r = false;
        if (t->kind == K::Array) {
            r = isResource(t->elem());
        } else if (t->kind == K::Named) {
            if (t->sym->kind == Symbol::Kind::Class)
                for (const auto& m : declAs<ClassDecl>(t->sym)->methods)
                    if (!m->isOperator && m->name == "drop") r = true;
            if (t->sym->kind == Symbol::Kind::Enum) {
                for (const auto& v : declAs<EnumDecl>(t->sym)->variants)
                    for (const Type* p : payloadTypes(t, v)) r = r || isResource(p);
            } else if (scalarOf(t).kind == ScalarInfo::None) {
                for (const Type* ft : fieldTypes(t)) r = r || isResource(ft);
            }
        }
        return resourceCache_[t] = r;
    }

    int variantIndex(const Symbol* enumSym, const std::string& name) {
        const auto& vs = declAs<EnumDecl>(enumSym)->variants;
        for (size_t i = 0; i < vs.size(); ++i)
            if (vs[i].name == name) return static_cast<int>(i);
        return -1;
    }

    // ------------------------------------------------------------ function instances

    std::string fnName(const FunctionDecl* fn, const Subst& s, const Type* owner) {
        std::string name;
        if (owner) name = typeName(owner) + ".";
        else if (fnModule_.count(fn)) name = fnModule_[fn]->displayName() + ".";
        // Operators are named with their arity: unary and binary '-' are different functions.
        // Extension operators (no owner) also need their parameter types to be unique.
        name += fn->isOperator ? "op" + fn->name + "/" + std::to_string(fn->params.size()) : fn->name;
        if (fn->isOperator && !owner) {
            name += "(";
            for (size_t i = 0; i < fn->params.size(); ++i) name += (i ? "," : "") + typeName(paramType(fn, i, s, nullptr));
            name += ")";
        }
        if (!fn->generics.empty()) {
            name += "<";
            for (size_t i = 0; i < fn->generics.size(); ++i) {
                auto it = s.find(fn->generics[i].sym);
                name += (i ? "," : "") + (it != s.end() ? typeName(it->second) : fn->generics[i].name);
            }
            name += ">";
        }
        return name;
    }

    std::string requestFn(const FunctionDecl* fn, const Subst& s, const Type* owner) {
        if (fn->isExtern) {
            // A C function: declared once under its own name.
            std::string decl = "declare " + lty(retTypeOf(fn, s, nullptr)) + " @" + llvmQuote(fn->name) + "(";
            for (size_t i = 0; i < fn->params.size(); ++i)
                decl += (i ? ", " : "") + lty(paramType(fn, i, s, nullptr));
            externDecls_.insert(decl + ")");
            return fn->name;
        }
        std::string name = fnName(fn, s, owner);
        if (fnsRequested_.insert(name).second) worklist_.push_back({fn, s, owner, name});
        return name;
    }

    void drainWorklist() {
        while (!worklist_.empty()) {
            Inst inst = worklist_.back();
            worklist_.pop_back();
            emitFunction(inst);
        }
    }

    bool isMethod(const FunctionDecl* fn) {
        auto it = fnOwner_.find(fn);
        return it != fnOwner_.end() && it->second->kind == Symbol::Kind::Class && !fn->isOperator;
    }

    const Type* paramType(const FunctionDecl* fn, size_t i, const Subst& s, const Type* owner) {
        return typeOfRef(*fn->params[i].type, s, owner);
    }

    const Type* retTypeOf(const FunctionDecl* fn, const Subst& s, const Type* owner) {
        return fn->returnType ? typeOfRef(*fn->returnType, s, owner) : tc_.voidT();
    }

    // ------------------------------------------------------------ emission helpers

    std::string tmp() { return "%t" + std::to_string(f_->tmp++); }
    std::string newLabel(const std::string& hint) { return hint + "." + std::to_string(f_->label++); }

    void emit(const std::string& line) {
        if (f_->terminated) {
            // Code after a return/break: give it an (unreachable) block of its own.
            f_->body << "dead." << f_->label++ << ":\n";
            f_->terminated = false;
        }
        f_->body << "  " << line << "\n";
    }
    void terminate(const std::string& line) {
        emit(line);
        f_->terminated = true;
    }
    void startBlock(const std::string& label) {
        if (!f_->terminated) f_->body << "  br label %" << label << "\n";
        f_->body << label << ":\n";
        f_->terminated = false;
    }

    std::string alloca(const std::string& ty) {
        std::string name = "%a" + std::to_string(f_->tmp++);
        f_->allocas << "  " << name << " = alloca " << ty << "\n";
        return name;
    }

    std::string load(const std::string& ty, const std::string& ptr) {
        std::string t = tmp();
        emit(t + " = load " + ty + ", ptr " + ptr);
        return t;
    }

    void store(const Val& v, const std::string& ptr) { emit("store " + v.ty + " " + v.v + ", ptr " + ptr); }

    std::string gepField(const Type* structT, const std::string& ptr, int idx) {
        std::string t = tmp();
        emit(t + " = getelementptr inbounds " + lty(structT) + ", ptr " + ptr + ", i32 0, i32 " + std::to_string(idx));
        return t;
    }

    std::string declare(const std::string& decl) {
        usedDecls_.insert(decl);
        return decl;
    }

    std::string declarations() {
        static const std::map<std::string, std::string> all = {
            {"malloc", "declare ptr @malloc(i64)"},
            {"calloc", "declare ptr @calloc(i64, i64)"},
            {"realloc", "declare ptr @realloc(ptr, i64)"},
            {"free", "declare void @free(ptr)"},
            {"memcpy", "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)"},
            {"memset", "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)"},
            {"h2_memeq", "declare i1 @h2_memeq(ptr, i64, ptr, i64)"},
            {"h2_write", "declare i64 @h2_write(i64, ptr, i64)"},
            {"h2_exit", "declare void @h2_exit(i64)"},
            {"h2_signed_to_string", "declare void @h2_signed_to_string(i64, ptr)"},
            {"h2_unsigned_to_string", "declare void @h2_unsigned_to_string(i64, ptr)"},
            {"h2_float_to_string", "declare void @h2_float_to_string(double, ptr)"},
            {"h2_char_to_string", "declare void @h2_char_to_string(i32, ptr)"},
            {"sqrt", "declare double @llvm.sqrt.f64(double)"},
            {"floor", "declare double @llvm.floor.f64(double)"},
            {"ceil", "declare double @llvm.ceil.f64(double)"},
        };
        std::string out;
        std::set<std::string> externNames;
        for (const auto& d : rawDecls_) out += d + "\n";
        for (const auto& d : externDecls_) {
            out += d + "\n";
            size_t at = d.find('@'), paren = d.find('(', at);
            std::string name = d.substr(at + 1, paren - at - 1);
            if (name.size() > 1 && name.front() == '"') name = name.substr(1, name.size() - 2);
            externNames.insert(name);
        }
        // Built-in declarations, unless the program already declared the same C function.
        for (const auto& d : usedDecls_)
            if (auto it = all.find(d); it != all.end() && !externNames.count(d)) out += it->second + "\n";
        return out;
    }

    // Allocates an i1 drop flag initialized to `initial` at function entry.
    std::string newFlag(bool initial) {
        std::string flag = alloca("i1");
        f_->allocas << "  store i1 " << (initial ? "true" : "false") << ", ptr " << flag << "\n";
        return flag;
    }

    // Spills a value to a fresh stack slot. If it needs cleanup, it becomes a
    // temporary dropped at the end of the current statement.
    std::string spill(const Val& v, const Type* t, bool registerDrop) {
        std::string slot = alloca(v.ty);
        store(v, slot);
        if (registerDrop && isResource(t)) {
            std::string flag = newFlag(false);
            emit("store i1 true, ptr " + flag);
            f_->temps.push_back({slot, t, flag});
        }
        return slot;
    }

    void emitDropIfFlag(const std::string& ptr, const Type* t, const std::string& flag) {
        std::string fl = load("i1", flag);
        std::string yes = newLabel("drop"), done = newLabel("dropped");
        terminate("br i1 " + fl + ", label %" + yes + ", label %" + done);
        startBlock(yes);
        emit("call void @" + llvmQuote(dropFn(t)) + "(ptr " + ptr + ")");
        emit("store i1 false, ptr " + flag);
        startBlock(done);
    }

    void flushTemps() {
        auto temps = std::move(f_->temps);
        f_->temps.clear();
        for (auto it = temps.rbegin(); it != temps.rend(); ++it) emitDropIfFlag(it->ptr, it->type, it->flag);
    }

    void dropScope(const std::vector<const Symbol*>& owned) {
        for (auto it = owned.rbegin(); it != owned.rend(); ++it) {
            const Local& l = f_->locals[*it];
            if (!l.flag.empty()) emitDropIfFlag(l.ptr, l.type, l.flag);
        }
    }

    // Drops for an early exit (return, break, continue, ?): the statement's
    // temporaries and the scopes being left. The temporaries stay registered,
    // because the normal path continues past this point.
    void dropScopesFrom(size_t depth) {
        for (auto it = f_->temps.rbegin(); it != f_->temps.rend(); ++it) emitDropIfFlag(it->ptr, it->type, it->flag);
        for (size_t i = f_->scopes.size(); i > depth; --i) dropScope(f_->scopes[i - 1]);
    }

    // ------------------------------------------------------------ drop functions

    std::string dropFn(const Type* t) {
        std::string name = "drop:" + typeName(t);
        if (dropsDone_.count(name)) return name;
        dropsDone_.insert(name);

        FnCtx ctx;
        FnCtx* saved = f_;
        f_ = &ctx;
        std::string st = lty(t);
        if (t->kind == K::Array) {
            for (uint64_t i = t->size; i > 0; --i) {
                std::string p = tmp();
                emit(p + " = getelementptr inbounds " + st + ", ptr %p, i64 0, i64 " + std::to_string(i - 1));
                emit("call void @" + llvmQuote(dropFn(t->elem())) + "(ptr " + p + ")");
            }
        } else if (t->kind == K::Named && t->sym->kind == Symbol::Kind::Enum) {
            const auto& vs = declAs<EnumDecl>(t->sym)->variants;
            std::string tag = load("i32", "%p");
            std::string done = newLabel("done");
            std::string sw = "switch i32 " + tag + ", label %" + done + " [";
            std::vector<std::pair<int, std::string>> cases;
            for (size_t i = 0; i < vs.size(); ++i) {
                auto ts = payloadTypes(t, vs[i]);
                if (std::none_of(ts.begin(), ts.end(), [&](const Type* p) { return isResource(p); })) continue;
                std::string l = newLabel("v");
                cases.push_back({static_cast<int>(i), l});
                sw += " i32 " + std::to_string(i) + ", label %" + l;
            }
            terminate(sw + " ]");
            for (auto& [idx, label] : cases) {
                startBlock(label);
                const Variant& v = vs[idx];
                auto ts = payloadTypes(t, v);
                std::string payload = gepField(t, "%p", 1);
                for (size_t j = ts.size(); j > 0; --j) {
                    if (!isResource(ts[j - 1])) continue;
                    std::string fp = tmp();
                    emit(fp + " = getelementptr inbounds " + payloadStruct(t, v) + ", ptr " + payload + ", i32 0, i32 " +
                         std::to_string(j - 1));
                    emit("call void @" + llvmQuote(dropFn(ts[j - 1])) + "(ptr " + fp + ")");
                }
                terminate("br label %" + done);
            }
            startBlock(done);
        } else if (t->kind == K::Named) {
            if (t->sym->kind == Symbol::Kind::Class)
                for (const auto& m : declAs<ClassDecl>(t->sym)->methods)
                    if (!m->isOperator && m->name == "drop") {
                        Subst s = substFor(t);
                        emit("call void @" + llvmQuote(requestFn(m.get(), s, t)) + "(ptr %p)");
                    }
            auto fs = fieldTypes(t);
            for (size_t i = fs.size(); i > 0; --i) {
                if (!isResource(fs[i - 1])) continue;
                std::string fp = gepField(t, "%p", static_cast<int>(i - 1));
                emit("call void @" + llvmQuote(dropFn(fs[i - 1])) + "(ptr " + fp + ")");
            }
        }
        emit("ret void");
        funcs_ << "define internal void @" << llvmQuote(name) << "(ptr %p) {\nentry:\n" << ctx.allocas.str()
               << ctx.body.str() << "}\n\n";
        f_ = saved;
        return name;
    }

    // ------------------------------------------------------------ functions

    void emitFunction(const Inst& inst) {
        const FunctionDecl* fn = inst.fn;
        FnCtx ctx;
        ctx.subst = inst.subst;
        ctx.owner = inst.owner;
        ctx.fn = fn;
        ctx.retType = retTypeOf(fn, inst.subst, inst.owner);
        FnCtx* saved = f_;
        f_ = &ctx;

        std::string sig;
        bool method = isMethod(fn);
        if (method) {
            sig += "ptr %self";
            ctx.selfPtr = "%self";
        }
        ctx.scopes.emplace_back();
        for (size_t i = 0; i < fn->params.size(); ++i) {
            const Param& p = fn->params[i];
            const Type* pt = paramType(fn, i, inst.subst, inst.owner);
            std::string pname = "%p" + std::to_string(i);
            if (!sig.empty()) sig += ", ";
            Local local;
            local.type = pt;
            if (isAggregate(pt)) {
                sig += "ptr " + pname;
                local.ptr = pname;
                if (p.isOwn && isResource(pt)) {
                    local.flag = newFlag(true);
                    ctx.scopes.back().push_back(p.sym);
                }
            } else {
                sig += lty(pt) + " " + pname;
                local.ptr = alloca(lty(pt));
                ctx.allocas << "  store " << lty(pt) << " " << pname << ", ptr " << local.ptr << "\n";
            }
            if (p.sym) ctx.locals[p.sym] = local;
        }

        emitBlockStmts(*fn->body);
        if (!ctx.terminated) {
            dropScopesFrom(0);
            if (ctx.retType == tc_.voidT()) emit("ret void");
            else emit("unreachable");
        }
        std::string attrs = (fn->isOperator && fn->body && fn->body->stmts.size() == 1) ? " alwaysinline" : "";
        funcs_ << "define internal " << lty(ctx.retType) << " @" << llvmQuote(inst.name) << "(" << sig << ")"
               << attrs << " {\nentry:\n" << ctx.allocas.str() << ctx.body.str() << "}\n\n";
        f_ = saved;
    }

    void emitBlockStmts(const Block& b) {
        for (const auto& s : b.stmts) emitStmt(*s);
    }

    void emitScopedBlock(const Block& b) {
        f_->scopes.emplace_back();
        emitBlockStmts(b);
        if (!f_->terminated) dropScope(f_->scopes.back());
        f_->scopes.pop_back();
    }

    // ------------------------------------------------------------ statements

    void emitStmt(const Stmt& s) {
        switch (s.kind) {
        case StmtKind::Block: emitScopedBlock(static_cast<const Block&>(s)); return;
        case StmtKind::Let: {
            const auto& l = static_cast<const LetStmt&>(s);
            const Type* t = l.type ? typeOfRef(*l.type, f_->subst, f_->owner) : (l.init ? typeOf(*l.init) : tc_.error());
            Local local;
            local.type = t;
            local.ptr = alloca(lty(t));
            if (l.init) {
                Val v = consume(*l.init, t);
                store(v, local.ptr);
            } else {
                emit("store " + lty(t) + " zeroinitializer, ptr " + local.ptr);
            }
            if (isResource(t)) {
                local.flag = newFlag(false);
                if (l.init) emit("store i1 true, ptr " + local.flag);
                f_->scopes.back().push_back(l.sym);
            }
            if (l.sym) f_->locals[l.sym] = local;
            flushTemps();
            return;
        }
        case StmtKind::Assign: emitAssign(static_cast<const AssignStmt&>(s)); flushTemps(); return;
        case StmtKind::Expr: {
            const Expr& e = *static_cast<const ExprStmt&>(s).expr;
            if (e.kind == ExprKind::Match) {
                emitMatch(static_cast<const MatchExpr&>(e), nullptr);
            } else {
                const Type* t = typeOf(e);
                Val v = rvalue(e);
                if (t != tc_.voidT() && !t->isError() && isResource(t) && !isPlace(e)) spill(v, t, true);
            }
            flushTemps();
            return;
        }
        case StmtKind::If: {
            const auto& i = static_cast<const IfStmt&>(s);
            std::string c = rvalue(*i.cond).v;
            flushTemps();
            std::string thenL = newLabel("then"), elseL = newLabel("else"), endL = newLabel("endif");
            terminate("br i1 " + c + ", label %" + thenL + ", label %" + (i.otherwise ? elseL : endL));
            startBlock(thenL);
            emitScopedBlock(*i.then);
            if (!f_->terminated) terminate("br label %" + endL);
            if (i.otherwise) {
                startBlock(elseL);
                f_->scopes.emplace_back();
                emitStmt(*i.otherwise);
                if (!f_->terminated) dropScope(f_->scopes.back());
                f_->scopes.pop_back();
                if (!f_->terminated) terminate("br label %" + endL);
            }
            startBlock(endL);
            return;
        }
        case StmtKind::While: {
            const auto& w = static_cast<const WhileStmt&>(s);
            std::string condL = newLabel("while"), bodyL = newLabel("body"), endL = newLabel("endwhile");
            startBlock(condL);
            std::string c = rvalue(*w.cond).v;
            flushTemps();
            terminate("br i1 " + c + ", label %" + bodyL + ", label %" + endL);
            startBlock(bodyL);
            f_->loops.push_back({endL, condL, f_->scopes.size()});
            emitScopedBlock(*w.body);
            f_->loops.pop_back();
            if (!f_->terminated) terminate("br label %" + condL);
            startBlock(endL);
            return;
        }
        case StmtKind::For: emitFor(static_cast<const ForStmt&>(s)); return;
        case StmtKind::Return: {
            const auto& r = static_cast<const ReturnStmt&>(s);
            if (r.value && f_->retType != tc_.voidT()) {
                Val v = consume(*r.value, f_->retType);
                dropScopesFrom(0);
                terminate("ret " + v.ty + " " + v.v);
            } else {
                if (r.value) rvalue(*r.value);
                dropScopesFrom(0);
                terminate("ret void");
            }
            return;
        }
        case StmtKind::Break:
        case StmtKind::Continue: {
            if (f_->loops.empty()) return;
            const LoopCtx& lc = f_->loops.back();
            dropScopesFrom(lc.scopeDepth);
            terminate("br label %" + (s.kind == StmtKind::Break ? lc.breakLabel : lc.continueLabel));
            return;
        }
        case StmtKind::Free: {
            const Expr& target = *static_cast<const FreeStmt&>(s).target;
            const Type* pt = typeOf(target);
            std::string p = rvalue(target).v;
            if (pt->kind == K::Ptr && isResource(pt->elem()))
                emit("call void @" + llvmQuote(dropFn(pt->elem())) + "(ptr " + p + ")");
            declare("free");
            emit("call void @free(ptr " + p + ")");
            flushTemps();
            return;
        }
        case StmtKind::Asm: unsupported(s.loc, "inline asm"); return;
        }
    }

    void emitFor(const ForStmt& fs) {
        const Type* it = typeOf(*fs.iterable);
        std::string condL = newLabel("for"), bodyL = newLabel("forbody"), stepL = newLabel("forstep"),
                    endL = newLabel("endfor");
        std::string idx = alloca("i64");
        std::string limit;
        std::function<void()> bindElem;
        if (it->kind == K::Range) {
            const Type* et = it->elem();
            std::string ety = lty(et);
            Val r = rvalue(*fs.iterable);
            std::string lo = tmp(), hi = tmp();
            emit(lo + " = extractvalue " + r.ty + " " + r.v + ", 0");
            emit(hi + " = extractvalue " + r.ty + " " + r.v + ", 1");
            flushTemps();
            std::string var = alloca(ety);
            store({ety, lo}, var);
            f_->locals[fs.sym] = {var, et, ""};
            startBlock(condL);
            std::string cur = load(ety, var);
            std::string c = tmp();
            emit(c + " = icmp " + (scalarOf(et).isSigned ? "slt " : "ult ") + ety + " " + cur + ", " + hi);
            terminate("br i1 " + c + ", label %" + bodyL + ", label %" + endL);
            startBlock(bodyL);
            f_->loops.push_back({endL, stepL, f_->scopes.size()});
            emitScopedBlock(*fs.body);
            f_->loops.pop_back();
            startBlock(stepL);
            std::string c2 = load(ety, var), n = tmp();
            emit(n + " = add " + ety + " " + c2 + ", 1");
            store({ety, n}, var);
            terminate("br label %" + condL);
            startBlock(endL);
            return;
        }
        // Arrays, slices and text: an index loop.
        std::string base, len;
        const Type* elemT = nullptr;
        bool textBytes = false;
        if (it->kind == K::Array) {
            base = addressOf(*fs.iterable);
            len = std::to_string(it->size);
            elemT = it->elem();
        } else if (it->kind == K::Slice) {
            Val sl = rvalue(*fs.iterable);
            base = tmp();
            emit(base + " = extractvalue " + sl.ty + " " + sl.v + ", 0");
            len = tmp();
            emit(len + " = extractvalue " + sl.ty + " " + sl.v + ", 1");
            elemT = it->elem();
        } else if (it == lang_.strT || it == lang_.stringT) {
            std::string p = addressOf(*fs.iterable);
            std::string bp = gepField(it, p, 0), lp = gepField(it, p, 1);
            base = load("ptr", bp);
            len = load("i64", lp);
            textBytes = true;
        } else {
            unsupported(fs.loc, "this kind of for loop");
            return;
        }
        emit("store i64 0, ptr " + idx);
        startBlock(condL);
        std::string i = load("i64", idx), c = tmp();
        emit(c + " = icmp ult i64 " + i + ", " + len);
        terminate("br i1 " + c + ", label %" + bodyL + ", label %" + endL);
        startBlock(bodyL);
        std::string i2 = load("i64", idx);
        if (textBytes) {
            std::string bp = tmp();
            emit(bp + " = getelementptr inbounds i8, ptr " + base + ", i64 " + i2);
            std::string byte = load("i8", bp), ch = tmp();
            emit(ch + " = zext i8 " + byte + " to i32");
            std::string var = alloca("i32");
            store({"i32", ch}, var);
            f_->locals[fs.sym] = {var, lang_.charT, ""};
        } else {
            std::string ep = tmp();
            if (it->kind == K::Array)
                emit(ep + " = getelementptr inbounds " + lty(it) + ", ptr " + base + ", i64 0, i64 " + i2);
            else
                emit(ep + " = getelementptr inbounds " + lty(elemT) + ", ptr " + base + ", i64 " + i2);
            f_->locals[fs.sym] = {ep, elemT, ""}; // borrowed element
        }
        f_->loops.push_back({endL, stepL, f_->scopes.size()});
        emitScopedBlock(*fs.body);
        f_->loops.pop_back();
        startBlock(stepL);
        std::string i3 = load("i64", idx), n = tmp();
        emit(n + " = add i64 " + i3 + ", 1");
        emit("store i64 " + n + ", ptr " + idx);
        terminate("br label %" + condL);
        startBlock(endL);
    }

    bool throughRawPointer(const Expr& target) {
        if (target.kind == ExprKind::Unary) return true;
        if (target.kind == ExprKind::Index) {
            const Type* ot = typeOf(*static_cast<const IndexExpr&>(target).object);
            return ot->kind == K::Ptr || ot->kind == K::Slice;
        }
        return false;
    }

    void emitAssign(const AssignStmt& a) {
        const Type* t = typeOf(*a.target);
        if (a.op == "=") {
            Val v = consume(*a.value, t);
            if (a.target->kind == ExprKind::Ident) {
                const Symbol* s = static_cast<const IdentExpr&>(*a.target).sym;
                Local& l = f_->locals[s];
                if (!l.flag.empty()) emitDropIfFlag(l.ptr, l.type, l.flag);
                store(v, l.ptr);
                if (!l.flag.empty()) emit("store i1 true, ptr " + l.flag);
                return;
            }
            std::string p = addressOf(*a.target);
            // Memory reached through a raw pointer may be uninitialized: no drop.
            if (isResource(t) && !throughRawPointer(*a.target))
                emit("call void @" + llvmQuote(dropFn(t)) + "(ptr " + p + ")");
            store(v, p);
            return;
        }
        std::string op = a.op.substr(0, a.op.size() - 1);
        std::string p = addressOf(*a.target);
        Val cur{lty(t), load(lty(t), p)};
        Val rhs = convertTo(*a.value, t);
        Val r = applyBinary(op, t, cur, t, rhs, a.loc);
        store(r, p);
    }

    // ------------------------------------------------------------ places

    bool isPlace(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Ident: {
            const Symbol* s = static_cast<const IdentExpr&>(e).sym;
            return s && s->kind == Symbol::Kind::Local;
        }
        case ExprKind::Self: return true;
        case ExprKind::Member: {
            const auto& m = static_cast<const MemberExpr&>(e);
            if (m.sym) return false;
            const Type* ot = typeOf(*m.object);
            return ot->kind == K::Ptr || isPlace(*m.object);
        }
        case ExprKind::Index: {
            const auto& ix = static_cast<const IndexExpr&>(e);
            const Type* ot = typeOf(*ix.object);
            if (ot->kind == K::Ptr || ot->kind == K::Slice) return true;
            return ot->kind == K::Array && isPlace(*ix.object);
        }
        case ExprKind::Unary: return static_cast<const UnaryExpr&>(e).op == "*";
        default: return false;
        }
    }

    // Address of an expression's value. Non-places are spilled to a temporary.
    std::string addressOf(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Ident: {
            const Symbol* s = static_cast<const IdentExpr&>(e).sym;
            if (s && s->kind == Symbol::Kind::Local) return f_->locals[s].ptr;
            break;
        }
        case ExprKind::Self: return f_->selfPtr;
        case ExprKind::Member: {
            const auto& m = static_cast<const MemberExpr&>(e);
            if (m.sym) break;
            const Type* ot = typeOf(*m.object);
            std::string base;
            if (ot->kind == K::Ptr) {
                base = rvalue(*m.object).v;
                ot = ot->elem();
            } else {
                base = addressOf(*m.object);
            }
            if (ot->kind == K::Named) {
                int idx = fieldIndex(ot, m.member);
                if (idx >= 0) return gepField(ot, base, idx);
            }
            break;
        }
        case ExprKind::Index: {
            const auto& ix = static_cast<const IndexExpr&>(e);
            const Type* ot = typeOf(*ix.object);
            if (ot->kind == K::Ptr || ot->kind == K::Array || ot->kind == K::Slice) {
                Val i = convertTo(*ix.index, lang_.uintT);
                std::string p = tmp();
                if (ot->kind == K::Ptr) {
                    std::string base = rvalue(*ix.object).v;
                    emit(p + " = getelementptr inbounds " + lty(ot->elem()) + ", ptr " + base + ", i64 " + i.v);
                } else if (ot->kind == K::Array) {
                    std::string base = addressOf(*ix.object);
                    emit(p + " = getelementptr inbounds " + lty(ot) + ", ptr " + base + ", i64 0, i64 " + i.v);
                } else {
                    Val sl = rvalue(*ix.object);
                    std::string base = tmp();
                    emit(base + " = extractvalue " + sl.ty + " " + sl.v + ", 0");
                    emit(p + " = getelementptr inbounds " + lty(ot->elem()) + ", ptr " + base + ", i64 " + i.v);
                }
                return p;
            }
            break;
        }
        case ExprKind::Unary:
            if (static_cast<const UnaryExpr&>(e).op == "*") return rvalue(*static_cast<const UnaryExpr&>(e).operand).v;
            break;
        default: break;
        }
        const Type* t = typeOf(e);
        return spill(rvalue(e), t, true);
    }

    // ------------------------------------------------------------ value conversions

    // A value for a position that takes ownership (let, return, own param, field init).
    Val consume(const Expr& e, const Type* target) {
        Val v = convertTo(e, target);
        if (e.kind == ExprKind::Ident && !info_.viewCoerced.count(&e)) {
            const Symbol* s = static_cast<const IdentExpr&>(e).sym;
            auto it = f_->locals.find(s);
            if (it != f_->locals.end() && !it->second.flag.empty()) emit("store i1 false, ptr " + it->second.flag);
        }
        return v;
    }

    // The value of `e`, converted to `target` (literal typing, string -> str, array -> slice).
    Val convertTo(const Expr& e, const Type* target) {
        target = subst(target, f_->subst);
        if (info_.viewCoerced.count(&e) && lang_.strT && target == lang_.strT) {
            std::string p = addressOf(e);
            const Type* st = typeOf(e);
            std::string buf = load("ptr", gepField(st, p, 0));
            std::string len = load("i64", gepField(st, p, 1));
            return makeStruct(lty(target), {{"ptr", buf}, {"i64", len}});
        }
        if (e.kind == ExprKind::Call) {
            // Intrinsics have no checked type; the context says what they produce.
            std::string iname;
            if (isIntrinsic(*static_cast<const CallExpr&>(e).callee, iname))
                return emitIntrinsic(iname, static_cast<const CallExpr&>(e), target);
        }
        const Type* t = typeOf(e);
        if (t->kind == K::Array && target->kind == K::Slice) {
            std::string p = addressOf(e);
            return makeStruct("{ ptr, i64 }", {{"ptr", p}, {"i64", std::to_string(t->size)}});
        }
        if (e.type && e.type->isLiteral() && target->kind == K::Named) return literalAs(e, target);
        return rvalue(e);
    }

    Val makeStruct(const std::string& ty, const std::vector<Val>& fields) {
        std::string cur = "undef";
        for (size_t i = 0; i < fields.size(); ++i) {
            std::string t = tmp();
            emit(t + " = insertvalue " + ty + " " + cur + ", " + fields[i].ty + " " + fields[i].v + ", " +
                 std::to_string(i));
            cur = t;
        }
        return {ty, cur};
    }

    // ------------------------------------------------------------ expressions

    Val literalAs(const Expr& e, const Type* t) {
        std::string ty = lty(t);
        ScalarInfo s = scalarOf(t);
        if (e.kind == ExprKind::IntLit) {
            uint64_t v;
            parseInt(static_cast<const LiteralExpr&>(e).value, v);
            if (s.kind == ScalarInfo::Float) return {ty, floatConst(static_cast<double>(v), s.bits == 32)};
            return {ty, std::to_string(v)};
        }
        if (e.kind == ExprKind::FloatLit) {
            double v = std::strtod(static_cast<const LiteralExpr&>(e).value.c_str(), nullptr);
            return {ty, floatConst(v, s.bits == 32)};
        }
        if (e.kind == ExprKind::Unary && static_cast<const UnaryExpr&>(e).op == "-") {
            Val inner = literalAs(*static_cast<const UnaryExpr&>(e).operand, t);
            if (s.kind == ScalarInfo::Float) {
                double d;
                uint64_t bits = std::stoull(inner.v.substr(2), nullptr, 16);
                std::memcpy(&d, &bits, sizeof d);
                return {ty, floatConst(-d, s.bits == 32)};
            }
            return {ty, "-" + inner.v};
        }
        // Arithmetic on literals: evaluate at run time with the target type.
        return rvalue(e);
    }

    Val rvalue(const Expr& e) {
        switch (e.kind) {
        case ExprKind::IntLit:
        case ExprKind::FloatLit: return literalAs(e, typeOf(e));
        case ExprKind::BoolLit: return {"i1", static_cast<const LiteralExpr&>(e).value};
        case ExprKind::NullLit: return {"ptr", "null"};
        case ExprKind::CharLit:
            return {"i32", std::to_string(decodeChar(static_cast<const LiteralExpr&>(e).value))};
        case ExprKind::StringLit: return strLiteral(static_cast<const LiteralExpr&>(e).value);
        case ExprKind::Self: {
            const Type* t = typeOf(e);
            return {lty(t), load(lty(t), f_->selfPtr)};
        }
        case ExprKind::Ident: {
            const auto& id = static_cast<const IdentExpr&>(e);
            if (id.sym && id.sym->kind == Symbol::Kind::Local) {
                const Local& l = f_->locals[id.sym];
                std::string ty = lty(l.type);
                return {ty, load(ty, l.ptr)};
            }
            if (id.sym && id.sym->kind == Symbol::Kind::Variant) return makeVariant(typeOf(e), id.sym, {});
            unsupported(e.loc, "this kind of name");
            return {"i64", "0"};
        }
        case ExprKind::Member: {
            const auto& m = static_cast<const MemberExpr&>(e);
            if (m.sym && m.sym->kind == Symbol::Kind::Variant) return makeVariant(typeOf(e), m.sym, {});
            const Type* ot = typeOf(*m.object);
            if ((ot->kind == K::Array) && m.member == "len") return {"i64", std::to_string(ot->size)};
            if (ot->kind == K::Slice && m.member == "len") {
                Val sl = rvalue(*m.object);
                std::string t = tmp();
                emit(t + " = extractvalue " + sl.ty + " " + sl.v + ", 1");
                return {"i64", t};
            }
            const Type* t = typeOf(e);
            return {lty(t), load(lty(t), addressOf(e))};
        }
        case ExprKind::Index: {
            const auto& ix = static_cast<const IndexExpr&>(e);
            const Type* ot = typeOf(*ix.object);
            if (ot->kind == K::Ptr || ot->kind == K::Array || ot->kind == K::Slice) {
                const Type* t = typeOf(e);
                return {lty(t), load(lty(t), addressOf(e))};
            }
            const Type* it = typeOf(*ix.index);
            return callOperator("[]", {ix.object.get(), ix.index.get()}, {ot, it}, e.loc);
        }
        case ExprKind::Unary: return emitUnary(static_cast<const UnaryExpr&>(e));
        case ExprKind::Binary: return emitBinary(static_cast<const BinaryExpr&>(e));
        case ExprKind::Call: return emitCall(static_cast<const CallExpr&>(e));
        case ExprKind::Try: return emitTry(static_cast<const TryExpr&>(e));
        case ExprKind::Alloc: {
            const auto& a = static_cast<const AllocExpr&>(e);
            if (a.arrayType) {
                const Type* at = typeOfRef(*a.arrayType, f_->subst, f_->owner);
                declare("calloc");
                std::string p = tmp();
                emit(p + " = call ptr @calloc(i64 " + std::to_string(at->size) + ", i64 " +
                     std::to_string(sizeOf(at->elem())) + ")");
                return {"ptr", p};
            }
            const Type* t = typeOf(*a.init);
            Val v = consume(*a.init, t);
            declare("malloc");
            std::string p = tmp();
            emit(p + " = call ptr @malloc(i64 " + std::to_string(std::max<uint64_t>(1, sizeOf(t))) + ")");
            store(v, p);
            return {"ptr", p};
        }
        case ExprKind::ArrayLit: {
            const auto& a = static_cast<const ArrayLitExpr&>(e);
            const Type* t = typeOf(e);
            std::string slot = alloca(lty(t));
            emit("store " + lty(t) + " zeroinitializer, ptr " + slot);
            for (size_t i = 0; i < a.elems.size(); ++i) {
                Val v = consume(*a.elems[i], t->elem());
                std::string p = tmp();
                emit(p + " = getelementptr inbounds " + lty(t) + ", ptr " + slot + ", i64 0, i64 " + std::to_string(i));
                store(v, p);
            }
            return {lty(t), load(lty(t), slot)};
        }
        case ExprKind::Match: return emitMatch(static_cast<const MatchExpr&>(e), typeOf(e));
        }
        return {"i64", "0"};
    }

    Val strLiteral(const std::string& text) {
        std::string name = "@.str." + std::to_string(strCounter_++);
        std::string bytes;
        for (unsigned char c : text) {
            if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
                bytes += static_cast<char>(c);
            } else {
                static const char* hex = "0123456789ABCDEF";
                bytes += '\\';
                bytes += hex[c >> 4];
                bytes += hex[c & 15];
            }
        }
        globals_ << name << " = private unnamed_addr constant [" << text.size() << " x i8] c\"" << bytes << "\"\n";
        return makeStruct(lty(lang_.strT), {{"ptr", name}, {"i64", std::to_string(text.size())}});
    }

    Val makeVariant(const Type* enumT, const Symbol* variant, const std::vector<const Expr*>& args) {
        std::string ty = lty(enumT);
        std::string slot = alloca(ty);
        int idx = variantIndex(enumT->sym, variant->name);
        emit("store i32 " + std::to_string(idx) + ", ptr " + slot);
        if (!args.empty()) {
            auto ts = payloadTypes(enumT, *variant->variant);
            std::string ps = payloadStruct(enumT, *variant->variant);
            std::string payload = gepField(enumT, slot, 1);
            for (size_t i = 0; i < args.size(); ++i) {
                Val v = consume(*args[i], ts[i]);
                std::string fp = tmp();
                emit(fp + " = getelementptr inbounds " + ps + ", ptr " + payload + ", i32 0, i32 " + std::to_string(i));
                store(v, fp);
            }
        }
        return {ty, load(ty, slot)};
    }

    Val emitUnary(const UnaryExpr& u) {
        const Type* t = typeOf(u);
        if (u.op == "&") return {"ptr", addressOf(*u.operand)};
        if (u.op == "*") {
            std::string p = rvalue(*u.operand).v;
            return {lty(t), load(lty(t), p)};
        }
        if (u.op == "-" && u.operand->type && u.operand->type->isLiteral()) return literalAs(u, t);
        const Type* ot = typeOf(*u.operand);
        return callOperator(u.op, {u.operand.get()}, {ot}, u.loc);
    }

    Val emitBinary(const BinaryExpr& b) {
        const std::string& op = b.op;
        if (op == "&&" || op == "||") {
            std::string slot = alloca("i1");
            std::string l = rvalue(*b.lhs).v;
            store({"i1", l}, slot);
            std::string rhsL = newLabel("rhs"), endL = newLabel("endlogic");
            if (op == "&&") terminate("br i1 " + l + ", label %" + rhsL + ", label %" + endL);
            else terminate("br i1 " + l + ", label %" + endL + ", label %" + rhsL);
            startBlock(rhsL);
            std::string r = rvalue(*b.rhs).v;
            store({"i1", r}, slot);
            startBlock(endL);
            return {"i1", load("i1", slot)};
        }
        const Type* lt = typeOf(*b.lhs);
        const Type* rt = typeOf(*b.rhs);
        if (b.lhs->type && b.lhs->type->isLiteral() && !(b.rhs->type && b.rhs->type->isLiteral())) lt = rt;
        if (b.rhs->type && b.rhs->type->isLiteral() && !(b.lhs->type && b.lhs->type->isLiteral())) rt = lt;
        if (b.lhs->type && b.lhs->type->isLiteral() && b.rhs->type && b.rhs->type->isLiteral()) {
            const Type* rtype = typeOf(b);
            if (rtype == lang_.boolT || op == "..") {
                bool isFloat = b.lhs->type->kind == K::FloatLit || b.rhs->type->kind == K::FloatLit;
                lt = rt = isFloat ? lang_.floatT : lang_.intT;
            } else {
                lt = rt = rtype;
            }
        }
        if (op == "..") {
            Val l = convertTo(*b.lhs, lt), r = convertTo(*b.rhs, rt);
            return makeStruct(lty(tc_.range(lt)), {l, r});
        }
        if (lt->kind == K::Ptr && (op == "==" || op == "!=")) {
            Val l = rvalue(*b.lhs), r = rvalue(*b.rhs);
            std::string t = tmp();
            emit(t + " = icmp " + (op == "==" ? "eq" : "ne") + " ptr " + l.v + ", " + r.v);
            return {"i1", t};
        }
        // Operands are borrowed: aggregates by address, scalars by value.
        Val l = operand(*b.lhs, lt), r = operand(*b.rhs, rt);
        // Mixed string/str with no exact operator: compare/combine as str views.
        if (lang_.strT && lang_.stringT && !findOperator(lt, op, {lt, rt}) && !findOperator(rt, op, {lt, rt})) {
            if (lt == lang_.stringT && rt == lang_.strT) { l = viewOf(l.v); lt = lang_.strT; }
            else if (rt == lang_.stringT && lt == lang_.strT) { r = viewOf(r.v); rt = lang_.strT; }
        }
        return applyBinary(op, lt, l, rt, r, b.loc);
    }

    // A pointer to a str view of the string at `stringPtr`.
    Val viewOf(const std::string& stringPtr) {
        std::string buf = load("ptr", gepField(lang_.stringT, stringPtr, 0));
        std::string len = load("i64", gepField(lang_.stringT, stringPtr, 1));
        Val v = makeStruct(lty(lang_.strT), {{"ptr", buf}, {"i64", len}});
        return {"ptr", spill(v, lang_.strT, false)};
    }

    Val operand(const Expr& e, const Type* t) {
        if (isAggregate(t)) return {"ptr", addressOf(e)};
        return convertTo(e, t);
    }

    // `op` with derived forms: != from ==, > <= >= from <.
    Val applyBinary(const std::string& op, const Type* lt, const Val& l, const Type* rt, const Val& r, SourceLoc loc) {
        if (OpRef f = findOperator(lt, op, {lt, rt})) return callOpVals(f, {l, r});
        if (rt != lt)
            if (OpRef f = findOperator(rt, op, {lt, rt})) return callOpVals(f, {l, r});
        auto negate = [&](Val v) {
            std::string t = tmp();
            emit(t + " = xor i1 " + v.v + ", true");
            return Val{"i1", t};
        };
        if (op == "!=") {
            if (OpRef f = findOperator(lt, "==", {lt, rt})) return negate(callOpVals(f, {l, r}));
        }
        if (op == ">" || op == "<=") {
            if (OpRef f = findOperator(rt, "<", {rt, lt})) {
                Val v = callOpVals(f, {r, l});
                return op == ">" ? v : negate(v);
            }
        }
        if (op == ">=") {
            if (OpRef f = findOperator(lt, "<", {lt, rt})) return negate(callOpVals(f, {l, r}));
        }
        unsupported(loc, "operator '" + op + "' on " + typeName(lt));
        return {"i1", "false"};
    }

    struct OpRef {
        const FunctionDecl* fn = nullptr;
        const Type* owner = nullptr; // null for an extension operator
        explicit operator bool() const { return fn != nullptr; }
    };

    OpRef findOperator(const Type* owner, const std::string& name, const std::vector<const Type*>& args) {
        if (owner->kind != K::Named) return {};
        std::vector<const FunctionDecl*> ops;
        if (owner->sym->kind == Symbol::Kind::Type)
            for (const auto& op : declAs<TypeDecl>(owner->sym)->operators) ops.push_back(op.get());
        else if (owner->sym->kind == Symbol::Kind::Class)
            for (const auto& m : declAs<ClassDecl>(owner->sym)->methods)
                if (m->isOperator) ops.push_back(m.get());
        Subst s = substFor(owner);
        for (const FunctionDecl* fn : ops) {
            if (fn->name != name || fn->params.size() != args.size()) continue;
            bool ok = true;
            for (size_t i = 0; i < args.size(); ++i)
                if (typeOfRef(*fn->params[i].type, s, owner) != args[i]) ok = false;
            if (ok) return {fn, owner};
        }
        // Extension operators (the checker has already enforced visibility).
        for (auto& m : prog_.modules)
            for (const FunctionDecl* fn : m->extOps) {
                if (fn->name != name || fn->params.size() != args.size()) continue;
                bool ok = true;
                for (size_t i = 0; i < args.size(); ++i)
                    if (typeOfRef(*fn->params[i].type, {}, nullptr) != args[i]) ok = false;
                if (ok) return {fn, nullptr};
            }
        return {};
    }

    // Calls an operator whose operands were already evaluated (aggregates as pointers).
    Val callOpVals(OpRef op, std::vector<Val> args) {
        const FunctionDecl* fn = op.fn;
        const Type* owner = op.owner;
        Subst s = owner ? substFor(owner) : Subst{};
        std::string name = requestFn(fn, s, owner);
        const Type* ret = retTypeOf(fn, s, owner);
        std::string argList;
        for (size_t i = 0; i < args.size(); ++i) argList += (i ? ", " : "") + args[i].ty + " " + args[i].v;
        return callRet(ret, "@" + llvmQuote(name), argList);
    }

    Val callRet(const Type* ret, const std::string& callee, const std::string& args) {
        if (ret == tc_.voidT() || ret->isError()) {
            emit("call void " + callee + "(" + args + ")");
            return {"void", ""};
        }
        std::string t = tmp();
        emit(t + " = call " + lty(ret) + " " + callee + "(" + args + ")");
        return {lty(ret), t};
    }

    Val callOperator(const std::string& op, std::vector<const Expr*> operands, std::vector<const Type*> types,
                     SourceLoc loc) {
        OpRef f = findOperator(types[0], op, types);
        if (!f) {
            unsupported(loc, "operator '" + op + "' on " + typeName(types[0]));
            return {"i64", "0"};
        }
        Subst s = f.owner ? substFor(f.owner) : Subst{};
        std::vector<Val> vals;
        for (size_t i = 0; i < operands.size(); ++i) {
            const Type* p = paramType(f.fn, i, s, f.owner);
            vals.push_back(passArg(*operands[i], p, f.fn->params[i].isOwn));
        }
        return callOpVals(f, vals);
    }

    // Evaluates an argument for a parameter of type `p`.
    Val passArg(const Expr& e, const Type* p, bool own) {
        if (isAggregate(p)) {
            if (own) {
                Val v = consume(e, p);
                std::string slot = alloca(v.ty);
                store(v, slot);
                return {"ptr", slot};
            }
            const Type* t = typeOf(e);
            if (info_.viewCoerced.count(&e) && p == lang_.strT && t != lang_.strT) {
                Val view = convertTo(e, p);
                return {"ptr", spill(view, p, false)};
            }
            if (t->kind == K::Array && p->kind == K::Slice) return {"ptr", spill(convertTo(e, p), p, false)};
            return {"ptr", addressOf(e)};
        }
        return own ? consume(e, p) : convertTo(e, p);
    }

    // ------------------------------------------------------------ calls

    Subst compose(const Subst& callSubst) {
        Subst out;
        for (const auto& [k, v] : callSubst) out[k] = subst(v, f_->subst);
        return out;
    }

    bool isIntrinsic(const Expr& callee, std::string& name) {
        if (callee.kind != ExprKind::Member) return false;
        const auto& m = static_cast<const MemberExpr&>(callee);
        if (m.sym && m.sym->kind == Symbol::Kind::Builtin && m.sym->name == "intrinsic") {
            name = m.member;
            return true;
        }
        if (m.object->kind != ExprKind::Ident) return false;
        const Symbol* s = static_cast<const IdentExpr&>(*m.object).sym;
        if (!s || s->kind != Symbol::Kind::Builtin || s->name != "intrinsic") return false;
        name = m.member;
        return true;
    }

    Val emitCall(const CallExpr& c) {
        std::string intrinsicName;
        if (isIntrinsic(*c.callee, intrinsicName)) return emitIntrinsic(intrinsicName, c, nullptr);

        auto callInfo = info_.calls.find(&c);
        // Method call.
        if (c.callee->kind == ExprKind::Member && !static_cast<const MemberExpr&>(*c.callee).sym) {
            const auto& m = static_cast<const MemberExpr&>(*c.callee);
            const Type* ot = typeOf(*m.object);
            std::string self;
            if (ot->kind == K::Ptr) {
                self = rvalue(*m.object).v;
                ot = ot->elem();
            } else {
                self = addressOf(*m.object);
            }
            const FunctionDecl* fn = callInfo != info_.calls.end() ? callInfo->second.fn : nullptr;
            if (!fn && ot->kind == K::Named && ot->sym->kind == Symbol::Kind::Class)
                for (const auto& mm : declAs<ClassDecl>(ot->sym)->methods)
                    if (!mm->isOperator && mm->name == m.member) fn = mm.get();
            if (!fn) {
                unsupported(c.loc, "this method call");
                return {"i64", "0"};
            }
            Subst s = substFor(ot);
            if (callInfo != info_.calls.end())
                for (const auto& [k, v] : compose(callInfo->second.subst)) s[k] = v;
            return callFunction(fn, s, ot, self, c);
        }

        const Symbol* sym = nullptr;
        if (c.callee->kind == ExprKind::Ident) sym = static_cast<const IdentExpr&>(*c.callee).sym;
        else if (c.callee->kind == ExprKind::Member) sym = static_cast<const MemberExpr&>(*c.callee).sym;
        if (!sym) {
            unsupported(c.loc, "this call");
            return {"i64", "0"};
        }
        switch (sym->kind) {
        case Symbol::Kind::Function: {
            const auto* fn = static_cast<const FunctionDecl*>(sym->decl);
            Subst s = callInfo != info_.calls.end() ? compose(callInfo->second.subst) : Subst{};
            return callFunction(fn, s, nullptr, "", c);
        }
        case Symbol::Kind::Variant: {
            std::vector<const Expr*> args;
            for (const auto& a : c.args) args.push_back(a.get());
            return makeVariant(typeOf(c), sym, args);
        }
        case Symbol::Kind::Class:
        case Symbol::Kind::Type: return emitConstruct(c, sym, callInfo != info_.calls.end() ? &callInfo->second : nullptr);
        case Symbol::Kind::Builtin:
            if (sym->name == "ptr" && c.args.size() == 1) {
                // ptr<T>(x): pointers are untyped in LLVM; integers become addresses.
                Val v = rvalue(*c.args[0]);
                if (v.ty == "ptr") return v;
                std::string t = tmp();
                emit(t + " = inttoptr " + v.ty + " " + v.v + " to ptr");
                return {"ptr", t};
            }
            unsupported(c.loc, "this call");
            return {"i64", "0"};
        default: unsupported(c.loc, "this call"); return {"i64", "0"};
        }
    }

    Val callFunction(const FunctionDecl* fn, const Subst& s, const Type* owner, const std::string& self,
                     const CallExpr& c) {
        std::string name = requestFn(fn, s, owner);
        const Type* ret = retTypeOf(fn, s, owner);
        std::string args;
        if (!self.empty()) args = "ptr " + self;
        for (size_t i = 0; i < c.args.size() && i < fn->params.size(); ++i) {
            const Type* p = paramType(fn, i, s, owner);
            Val v = passArg(*c.args[i], p, fn->params[i].isOwn);
            args += (args.empty() ? "" : ", ") + v.ty + " " + v.v;
        }
        return callRet(ret, "@" + llvmQuote(name), args);
    }

    Val emitConstruct(const CallExpr& c, const Symbol*, const SemaInfo::Call* ci) {
        const Type* t = typeOf(c);
        // string(x): copy a str, or convert via `operator string`.
        if (lang_.stringT && t == lang_.stringT && c.args.size() == 1) {
            const Type* at = typeOf(*c.args[0]);
            if (at != lang_.strT) {
                OpRef conv = findOperator(at, "string", {at});
                if (!conv) {
                    unsupported(c.loc, "string() of " + typeName(at));
                    return {"i64", "0"};
                }
                Val a = isAggregate(at) ? Val{"ptr", addressOf(*c.args[0])} : convertTo(*c.args[0], at);
                return callOpVals(conv, {a});
            }
        }
        if (t->kind == K::Named && t->sym->kind == Symbol::Kind::Type && scalarOf(t).kind != ScalarInfo::None)
            return emitConversion(c, t);
        // init(...)
        const FunctionDecl* initFn = ci ? ci->fn : nullptr;
        if (!initFn && t->kind == K::Named && t->sym->kind == Symbol::Kind::Class)
            for (const auto& m : declAs<ClassDecl>(t->sym)->methods)
                if (!m->isOperator && m->name == "init") initFn = m.get();
        if (initFn) {
            std::string slot = alloca(lty(t));
            emit("store " + lty(t) + " zeroinitializer, ptr " + slot);
            Subst s = substFor(t);
            callFunction(initFn, s, t, slot, c);
            return {lty(t), load(lty(t), slot)};
        }
        // Fields in order.
        auto fts = fieldTypes(t);
        std::vector<Val> vals;
        for (size_t i = 0; i < c.args.size() && i < fts.size(); ++i) vals.push_back(consume(*c.args[i], fts[i]));
        if (vals.empty()) return {lty(t), "zeroinitializer"};
        return makeStruct(lty(t), vals);
    }

    Val emitConversion(const CallExpr& c, const Type* target) {
        const Expr& arg = *c.args[0];
        if (arg.type && arg.type->isLiteral()) return literalAs(arg, target);
        const Type* from = typeOf(arg);
        Val v = rvalue(arg);
        ScalarInfo fs = scalarOf(from), ts = scalarOf(target);
        std::string fty = lty(from), tty = lty(target);
        if (fty == tty) return {tty, v.v};
        std::string t = tmp();
        if (fs.kind == ScalarInfo::Int && ts.kind == ScalarInfo::Int) {
            const char* inst = ts.bits < fs.bits ? "trunc" : (fs.isSigned && fs.bits > 1 ? "sext" : "zext");
            emit(t + " = " + inst + " " + fty + " " + v.v + " to " + tty);
        } else if (fs.kind == ScalarInfo::Int && ts.kind == ScalarInfo::Float) {
            emit(t + " = " + (fs.isSigned ? "sitofp " : "uitofp ") + fty + " " + v.v + " to " + tty);
        } else if (fs.kind == ScalarInfo::Float && ts.kind == ScalarInfo::Int) {
            emit(t + " = " + (ts.isSigned ? "fptosi " : "fptoui ") + fty + " " + v.v + " to " + tty);
        } else {
            emit(t + " = " + (ts.bits < fs.bits ? "fptrunc " : "fpext ") + fty + " " + v.v + " to " + tty);
        }
        return {tty, t};
    }

    Val emitIntrinsic(const std::string& name, const CallExpr& c, const Type* expected) {
        std::vector<Val> a;
        std::vector<const Type*> ts;
        for (const auto& arg : c.args) {
            const Type* t = typeOf(*arg);
            ts.push_back(t);
            a.push_back(convertTo(*arg, t));
        }
        auto bin = [&](const std::string& inst) {
            std::string t = tmp();
            emit(t + " = " + inst + " " + a[0].ty + " " + a[0].v + ", " + a[1].v);
            return Val{a[0].ty, t};
        };
        auto cmp = [&](const std::string& inst) {
            std::string t = tmp();
            emit(t + " = " + inst + " " + a[0].ty + " " + a[0].v + ", " + a[1].v);
            return Val{"i1", t};
        };
        static const std::map<std::string, std::string> arith = {
            {"add", "add"}, {"sub", "sub"}, {"mul", "mul"}, {"sdiv", "sdiv"}, {"udiv", "udiv"},
            {"srem", "srem"}, {"urem", "urem"}, {"and", "and"}, {"or", "or"}, {"xor", "xor"},
            {"shl", "shl"}, {"ashr", "ashr"}, {"lshr", "lshr"}, {"fadd", "fadd"}, {"fsub", "fsub"},
            {"fmul", "fmul"}, {"fdiv", "fdiv"}};
        static const std::map<std::string, std::string> cmps = {
            {"eq", "icmp eq"}, {"slt", "icmp slt"}, {"ult", "icmp ult"}, {"sle", "icmp sle"}, {"sgt", "icmp sgt"},
            {"ugt", "icmp ugt"}, {"sge", "icmp sge"}, {"feq", "fcmp oeq"}, {"flt", "fcmp olt"}};
        if (checks_ && (name == "add" || name == "sub" || name == "mul") && a[0].ty != "i1") {
            // Debug builds stop the program on overflow instead of wrapping.
            bool isSigned = scalarOf(ts[0]).isSigned;
            std::string fn = std::string("llvm.") + (isSigned ? "s" : "u") + name + ".with.overflow." + a[0].ty;
            std::string pair = "{ " + a[0].ty + ", i1 }";
            rawDecls_.insert("declare " + pair + " @" + fn + "(" + a[0].ty + ", " + a[0].ty + ")");
            std::string r = tmp(), v = tmp(), o = tmp();
            emit(r + " = call " + pair + " @" + fn + "(" + a[0].ty + " " + a[0].v + ", " + a[0].ty + " " + a[1].v + ")");
            emit(v + " = extractvalue " + pair + " " + r + ", 0");
            emit(o + " = extractvalue " + pair + " " + r + ", 1");
            trapIf(o);
            return {a[0].ty, v};
        }
        if (checks_ && (name == "sdiv" || name == "udiv" || name == "srem" || name == "urem")) {
            std::string z = tmp();
            emit(z + " = icmp eq " + a[1].ty + " " + a[1].v + ", 0");
            trapIf(z);
        }
        if (auto it = arith.find(name); it != arith.end()) return bin(it->second);
        if (auto it = cmps.find(name); it != cmps.end()) return cmp(it->second);
        if (name == "load") { // take(p): read the value at p (the target type comes from context)
            if (!expected || expected->isError() || ts[0]->kind != K::Ptr) {
                unsupported(c.loc, "intrinsic.load without a known result type");
                return {"i64", "0"};
            }
            return {lty(expected), load(lty(expected), a[0].v)};
        }
        if (name == "size_of") {
            if (ts[0]->kind != K::Ptr) {
                unsupported(c.loc, "intrinsic.size_of of a non-pointer");
                return {"i64", "0"};
            }
            return {"i64", std::to_string(sizeOf(ts[0]->elem()))};
        }
        if (name == "neg") {
            std::string t = tmp();
            emit(t + " = sub " + a[0].ty + " 0, " + a[0].v);
            return {a[0].ty, t};
        }
        if (name == "fneg") {
            std::string t = tmp();
            emit(t + " = fneg " + a[0].ty + " " + a[0].v);
            return {a[0].ty, t};
        }
        if (name == "not") {
            std::string t = tmp();
            emit(t + " = xor " + a[0].ty + " " + a[0].v + ", " + (a[0].ty == "i1" ? "true" : "-1"));
            return {a[0].ty, t};
        }
        if (name == "malloc" || name == "realloc") {
            declare(name);
            std::string t = tmp();
            if (name == "malloc") emit(t + " = call ptr @malloc(i64 " + a[0].v + ")");
            else emit(t + " = call ptr @realloc(ptr " + a[0].v + ", i64 " + a[1].v + ")");
            return {"ptr", t};
        }
        if (name == "free") {
            declare("free");
            emit("call void @free(ptr " + a[0].v + ")");
            return {"void", ""};
        }
        if (name == "memcpy") {
            declare("memcpy");
            emit("call void @llvm.memcpy.p0.p0.i64(ptr " + a[0].v + ", ptr " + a[1].v + ", i64 " + a[2].v + ", i1 false)");
            return {"void", ""};
        }
        if (name == "memset") {
            declare("memset");
            emit("call void @llvm.memset.p0.i64(ptr " + a[0].v + ", i8 " + a[1].v + ", i64 " + a[2].v + ", i1 false)");
            return {"void", ""};
        }
        if (name == "memeq") {
            declare("h2_memeq");
            std::string t = tmp();
            emit(t + " = call i1 @h2_memeq(ptr " + a[0].v + ", i64 " + a[1].v + ", ptr " + a[2].v + ", i64 " + a[3].v + ")");
            return {"i1", t};
        }
        if (name == "sys_write") {
            declare("h2_write");
            std::string t = tmp();
            emit(t + " = call i64 @h2_write(i64 " + a[0].v + ", ptr " + a[1].v + ", i64 " + a[2].v + ")");
            return {"i64", t};
        }
        if (name == "sys_exit") {
            declare("h2_exit");
            emit("call void @h2_exit(i64 " + a[0].v + ")");
            return {"void", ""};
        }
        if (name == "sqrt" || name == "floor" || name == "ceil") {
            declare(name);
            std::string t = tmp();
            emit(t + " = call double @llvm." + name + ".f64(double " + a[0].v + ")");
            return {"double", t};
        }
        if (name == "signed_to_string" || name == "unsigned_to_string" || name == "float_to_string" ||
            name == "char_to_string") {
            std::string fn = "h2_" + name;
            declare(fn);
            Val arg = a[0];
            ScalarInfo s = scalarOf(ts[0]);
            if (name == "float_to_string" && arg.ty == "float") {
                std::string t = tmp();
                emit(t + " = fpext float " + arg.v + " to double");
                arg = {"double", t};
            } else if ((name == "signed_to_string" || name == "unsigned_to_string") && arg.ty != "i64") {
                std::string t = tmp();
                emit(t + " = " + (s.isSigned ? "sext " : "zext ") + arg.ty + " " + arg.v + " to i64");
                arg = {"i64", t};
            }
            std::string st = lty(lang_.stringT);
            std::string slot = alloca(st);
            emit("call void @" + fn + "(" + arg.ty + " " + arg.v + ", ptr " + slot + ")");
            return {st, load(st, slot)};
        }
        unsupported(c.loc, "intrinsic." + name);
        return {"i64", "0"};
    }

    // Stops the program (llvm.trap) when `cond` is true. Works without any OS.
    void trapIf(const std::string& cond) {
        rawDecls_.insert("declare void @llvm.trap() cold noreturn nounwind");
        std::string bad = newLabel("trap"), ok = newLabel("checked");
        terminate("br i1 " + cond + ", label %" + bad + ", label %" + ok);
        startBlock(bad);
        emit("call void @llvm.trap()");
        terminate("unreachable");
        startBlock(ok);
    }

    // ------------------------------------------------------------ match and ?

    // Emits an i1 that is true if the value at `p` (of type `t`) matches.
    std::string patternTest(const Pattern& pat, const std::string& p, const Type* t) {
        switch (pat.kind) {
        case Pattern::Kind::Wildcard: return "true";
        case Pattern::Kind::Binding:
            if (!pat.sym || pat.sym->kind != Symbol::Kind::Variant) return "true";
            [[fallthrough]];
        case Pattern::Kind::Variant: {
            const Symbol* v = pat.sym;
            std::string tag = load("i32", gepField(t, p, 0));
            std::string ok = tmp();
            emit(ok + " = icmp eq i32 " + tag + ", " + std::to_string(variantIndex(t->sym, v->name)));
            if (pat.fields.empty()) return ok;
            auto ts = payloadTypes(t, *v->variant);
            std::string ps = payloadStruct(t, *v->variant);
            std::string payload = gepField(t, p, 1);
            std::string acc = ok;
            for (size_t i = 0; i < pat.fields.size() && i < ts.size(); ++i) {
                std::string fp = tmp();
                emit(fp + " = getelementptr inbounds " + ps + ", ptr " + payload + ", i32 0, i32 " + std::to_string(i));
                std::string sub = patternTest(*pat.fields[i], fp, ts[i]);
                if (sub == "true") continue;
                std::string sel = tmp();
                emit(sel + " = select i1 " + acc + ", i1 " + sub + ", i1 false");
                acc = sel;
            }
            return acc;
        }
        case Pattern::Kind::Literal: {
            const Expr& lit = *pat.literal;
            if (t == lang_.strT) {
                Val s = rvalue(lit);
                std::string a = load("ptr", gepField(t, p, 0)), al = load("i64", gepField(t, p, 1));
                std::string b = tmp(), bl = tmp();
                emit(b + " = extractvalue " + s.ty + " " + s.v + ", 0");
                emit(bl + " = extractvalue " + s.ty + " " + s.v + ", 1");
                declare("h2_memeq");
                std::string r = tmp();
                emit(r + " = call i1 @h2_memeq(ptr " + a + ", i64 " + al + ", ptr " + b + ", i64 " + bl + ")");
                return r;
            }
            Val v = convertTo(lit, t);
            std::string cur = load(lty(t), p);
            std::string r = tmp();
            bool isFloat = scalarOf(t).kind == ScalarInfo::Float;
            emit(r + " = " + (isFloat ? "fcmp oeq " : "icmp eq ") + lty(t) + " " + cur + ", " + v.v);
            return r;
        }
        }
        return "true";
    }

    void bindPattern(const Pattern& pat, const std::string& p, const Type* t) {
        if (pat.kind == Pattern::Kind::Binding && pat.sym && pat.sym->kind == Symbol::Kind::Local) {
            f_->locals[pat.sym] = {p, t, ""}; // borrowed view of the subject
            return;
        }
        if ((pat.kind == Pattern::Kind::Variant || pat.kind == Pattern::Kind::Binding) && pat.sym &&
            pat.sym->kind == Symbol::Kind::Variant && !pat.fields.empty()) {
            auto ts = payloadTypes(t, *pat.sym->variant);
            std::string ps = payloadStruct(t, *pat.sym->variant);
            std::string payload = gepField(t, p, 1);
            for (size_t i = 0; i < pat.fields.size() && i < ts.size(); ++i) {
                std::string fp = tmp();
                emit(fp + " = getelementptr inbounds " + ps + ", ptr " + payload + ", i32 0, i32 " + std::to_string(i));
                bindPattern(*pat.fields[i], fp, ts[i]);
            }
        }
    }

    Val emitMatch(const MatchExpr& m, const Type* resultT) {
        const Type* st = typeOf(*m.subject);
        std::string subj = addressOf(*m.subject);
        bool hasValue = resultT && resultT != tc_.voidT() && !resultT->isError();
        std::string slot = hasValue ? alloca(lty(resultT)) : "";
        std::string endL = newLabel("endmatch");
        for (size_t i = 0; i < m.arms.size(); ++i) {
            const MatchArm& arm = m.arms[i];
            std::string armL = newLabel("arm"), nextL = newLabel("next");
            std::string c = patternTest(*arm.pattern, subj, st);
            terminate("br i1 " + c + ", label %" + armL + ", label %" + nextL);
            startBlock(armL);
            f_->scopes.emplace_back();
            bindPattern(*arm.pattern, subj, st);
            if (arm.block) {
                emitBlockStmts(*arm.block);
            } else if (hasValue) {
                store(consume(*arm.expr, resultT), slot);
            } else {
                const Type* t = typeOf(*arm.expr);
                Val v = rvalue(*arm.expr);
                if (t != tc_.voidT() && !t->isError() && isResource(t) && !isPlace(*arm.expr)) spill(v, t, true);
            }
            if (!f_->terminated) {
                flushTemps();
                dropScope(f_->scopes.back());
            }
            f_->scopes.pop_back();
            if (!f_->terminated) terminate("br label %" + endL);
            startBlock(nextL);
        }
        terminate("unreachable");
        startBlock(endL);
        if (!hasValue) return {"void", ""};
        return {lty(resultT), load(lty(resultT), slot)};
    }

    Val emitTry(const TryExpr& t) {
        const Type* vt = typeOf(*t.operand);
        Val v = consume(*t.operand, vt);
        std::string slot = alloca(v.ty);
        store(v, slot);
        bool isResult = declAs<EnumDecl>(vt->sym)->variants.size() == 2 &&
                        declAs<EnumDecl>(vt->sym)->variants[0].name == "Ok";
        int okIdx = 0;
        std::string tag = load("i32", gepField(vt, slot, 0));
        std::string ok = tmp();
        emit(ok + " = icmp eq i32 " + tag + ", " + std::to_string(okIdx));
        std::string contL = newLabel("tryok"), failL = newLabel("tryfail");
        terminate("br i1 " + ok + ", label %" + contL + ", label %" + failL);

        // Failure: return Err(e) / None from this function.
        startBlock(failL);
        const Type* rt = f_->retType;
        std::string rslot = alloca(lty(rt));
        if (isResult) {
            const Variant& errV = declAs<EnumDecl>(vt->sym)->variants[1];
            std::string ps = payloadStruct(vt, errV);
            std::string from = gepField(vt, slot, 1);
            std::string e = load(lty(payloadTypes(vt, errV)[0]), from);
            emit("store i32 1, ptr " + rslot);
            std::string to = gepField(rt, rslot, 1);
            store({lty(payloadTypes(vt, errV)[0]), e}, to);
        } else {
            emit("store i32 1, ptr " + rslot);
        }
        std::string rv = load(lty(rt), rslot);
        dropScopesFrom(0);
        terminate("ret " + lty(rt) + " " + rv);

        startBlock(contL);
        const Variant& okV = declAs<EnumDecl>(vt->sym)->variants[0];
        const Type* okT = payloadTypes(vt, okV)[0];
        std::string payload = gepField(vt, slot, 1);
        return {lty(okT), load(lty(okT), payload)};
    }
};

} // namespace

bool emitLLVM(Program& program, TypeContext& types, const SemaInfo& info, Diagnostics& diags, std::ostream& out,
              bool checks) {
    return Codegen(program, types, info, diags, checks).run(out);
}

} // namespace h2
