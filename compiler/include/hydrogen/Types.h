#pragma once
// Semantic types, produced by the type checker. Types are interned: two
// structurally equal types are the same pointer, so `a == b` compares them.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace h2 {

struct Symbol;

struct Type {
    enum class Kind {
        Error,    // poison: already reported; compatible with everything
        Void,
        IntLit,   // an integer literal whose type isn't decided yet
        FloatLit, // a float literal whose type isn't decided yet
        Named,    // new type / class / enum, with type arguments
        Ptr,      // ptr<T>
        Array,    // [T; N]
        Slice,    // [T]
        Generic,  // a generic parameter T
        Range,    // a..b
    };
    Kind kind;
    const Symbol* sym = nullptr;   // Named: the type/class/enum; Generic: the parameter
    std::vector<const Type*> args; // Named: type arguments; Ptr/Array/Slice/Range: {element}
    uint64_t size = 0;             // Array length

    bool is(Kind k) const { return kind == k; }
    bool isError() const { return kind == Kind::Error; }
    bool isLiteral() const { return kind == Kind::IntLit || kind == Kind::FloatLit; }
    const Type* elem() const { return args.empty() ? nullptr : args[0]; }
};

class TypeContext {
public:
    TypeContext();

    const Type* error() const { return error_; }
    const Type* voidT() const { return void_; }
    const Type* intLit() const { return intLit_; }
    const Type* floatLit() const { return floatLit_; }

    const Type* named(const Symbol* sym, std::vector<const Type*> args = {});
    const Type* ptr(const Type* elem);
    const Type* array(const Type* elem, uint64_t size);
    const Type* slice(const Type* elem);
    const Type* range(const Type* elem);
    const Type* generic(const Symbol* param);

    // Human-readable form for diagnostics: int, ptr<u8>, List<int>, [u8; 16]
    static std::string str(const Type* t);

private:
    using Key = std::tuple<int, const Symbol*, std::vector<const Type*>, uint64_t>;
    std::map<Key, std::unique_ptr<Type>> interned_;
    const Type* error_;
    const Type* void_;
    const Type* intLit_;
    const Type* floatLit_;

    const Type* get(Type::Kind kind, const Symbol* sym, std::vector<const Type*> args, uint64_t size);
};

} // namespace h2
