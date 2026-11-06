#include "hydrogen/Types.h"
#include "hydrogen/Program.h"

namespace h2 {

TypeContext::TypeContext() {
    error_ = get(Type::Kind::Error, nullptr, {}, 0);
    void_ = get(Type::Kind::Void, nullptr, {}, 0);
    intLit_ = get(Type::Kind::IntLit, nullptr, {}, 0);
    floatLit_ = get(Type::Kind::FloatLit, nullptr, {}, 0);
}

const Type* TypeContext::get(Type::Kind kind, const Symbol* sym, std::vector<const Type*> args, uint64_t size) {
    Key key{static_cast<int>(kind), sym, args, size};
    auto it = interned_.find(key);
    if (it != interned_.end()) return it->second.get();
    auto t = std::make_unique<Type>();
    t->kind = kind;
    t->sym = sym;
    t->args = std::move(args);
    t->size = size;
    const Type* raw = t.get();
    interned_.emplace(std::move(key), std::move(t));
    return raw;
}

const Type* TypeContext::named(const Symbol* sym, std::vector<const Type*> args) {
    for (const Type* a : args)
        if (a->isError()) return error_;
    return get(Type::Kind::Named, sym, std::move(args), 0);
}
const Type* TypeContext::ptr(const Type* e) { return e->isError() ? error_ : get(Type::Kind::Ptr, nullptr, {e}, 0); }
const Type* TypeContext::array(const Type* e, uint64_t n) {
    return e->isError() ? error_ : get(Type::Kind::Array, nullptr, {e}, n);
}
const Type* TypeContext::slice(const Type* e) { return e->isError() ? error_ : get(Type::Kind::Slice, nullptr, {e}, 0); }
const Type* TypeContext::range(const Type* e) { return e->isError() ? error_ : get(Type::Kind::Range, nullptr, {e}, 0); }
const Type* TypeContext::generic(const Symbol* p) { return get(Type::Kind::Generic, p, {}, 0); }
const Type* TypeContext::function(const Type* result, const std::vector<const Type*>& params) {
    std::vector<const Type*> args{result};
    for (const Type* p : params) {
        if (p->isError()) return error_;
        args.push_back(p);
    }
    return get(Type::Kind::Function, nullptr, std::move(args), 0);
}

std::string TypeContext::str(const Type* t) {
    switch (t->kind) {
    case Type::Kind::Error: return "<error>";
    case Type::Kind::Void: return "void";
    case Type::Kind::IntLit: return "integer literal";
    case Type::Kind::FloatLit: return "float literal";
    case Type::Kind::Generic: return t->sym->name;
    case Type::Kind::Ptr: return "ptr<" + str(t->elem()) + ">";
    case Type::Kind::Array: return "[" + str(t->elem()) + "; " + std::to_string(t->size) + "]";
    case Type::Kind::Slice: return "[" + str(t->elem()) + "]";
    case Type::Kind::Range: return "Range<" + str(t->elem()) + ">";
    case Type::Kind::Function: {
        std::string s = "function(";
        for (size_t i = 1; i < t->args.size(); ++i) s += (i > 1 ? ", " : "") + str(t->args[i]);
        s += ")";
        if (t->args[0]->kind != Type::Kind::Void) s += " -> " + str(t->args[0]);
        return s;
    }
    case Type::Kind::Named: {
        std::string s = t->sym->name;
        if (!t->args.empty()) {
            s += '<';
            for (size_t i = 0; i < t->args.size(); ++i) s += (i ? ", " : "") + str(t->args[i]);
            s += '>';
        }
        return s;
    }
    }
    return "?";
}

} // namespace h2
