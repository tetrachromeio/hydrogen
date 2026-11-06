#pragma once
#include "hydrogen/Source.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace h2 {

struct Expr;
struct Stmt;
struct Decl;
struct TypeRef;
struct Pattern;
struct Symbol; // defined in Program.h; filled in by the resolver
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using DeclPtr = std::unique_ptr<Decl>;
using TypeRefPtr = std::unique_ptr<TypeRef>;
using PatternPtr = std::unique_ptr<Pattern>;

// ---------------------------------------------------------------- types

struct TypeRef {
    enum class Kind { Named, Array };
    Kind kind = Kind::Named;
    SourceLoc loc;
    // Named: dotted path (e.g. "Atomic.Memory.Block") plus optional type args.
    // `ptr<T>` is just Named{"ptr", [T]}.
    std::string name;
    std::vector<TypeRefPtr> args;
    // Array: [elem; size] or [elem] (slice) when size is null.
    TypeRefPtr elem;
    ExprPtr size;
    const Symbol* sym = nullptr; // resolved type (Named only)
};

struct GenericParam {
    SourceLoc loc;
    std::string name;
    std::vector<TypeRefPtr> constraints; // T: A + B
    const Symbol* sym = nullptr;         // set by the resolver
};

// ---------------------------------------------------------------- expressions

enum class ExprKind {
    IntLit, FloatLit, StringLit, CharLit, BoolLit, Ident, Self,
    Unary, Binary, Call, Member, Index, Try, Alloc, ArrayLit, Match
};

struct Type; // defined in Types.h; filled in by the type checker

struct Expr {
    ExprKind kind;
    SourceLoc loc;
    const Type* type = nullptr;
    explicit Expr(ExprKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Expr() = default;
};

struct LiteralExpr : Expr {
    std::string value; // raw digits / decoded string / "true"/"false"
    LiteralExpr(ExprKind k, SourceLoc l, std::string v) : Expr(k, l), value(std::move(v)) {}
};

struct IdentExpr : Expr {
    std::string name;
    std::vector<TypeRefPtr> typeArgs; // List<int>(...) / max<int>(...)
    const Symbol* sym = nullptr;
    IdentExpr(SourceLoc l, std::string n) : Expr(ExprKind::Ident, l), name(std::move(n)) {}
};

struct SelfExpr : Expr {
    explicit SelfExpr(SourceLoc l) : Expr(ExprKind::Self, l) {}
};

struct UnaryExpr : Expr {
    std::string op; // "-", "!", "~", "&", "*"
    ExprPtr operand;
    UnaryExpr(SourceLoc l, std::string o, ExprPtr e)
        : Expr(ExprKind::Unary, l), op(std::move(o)), operand(std::move(e)) {}
};

struct BinaryExpr : Expr {
    std::string op; // "+", "==", "..", ...
    ExprPtr lhs, rhs;
    BinaryExpr(SourceLoc l, std::string o, ExprPtr a, ExprPtr b)
        : Expr(ExprKind::Binary, l), op(std::move(o)), lhs(std::move(a)), rhs(std::move(b)) {}
};

struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    CallExpr(SourceLoc l, ExprPtr c) : Expr(ExprKind::Call, l), callee(std::move(c)) {}
};

struct MemberExpr : Expr {
    ExprPtr object;
    std::string member;
    // Set when this is a qualified name (Module.item, Enum.Variant);
    // null for runtime field/method access, which needs type checking.
    const Symbol* sym = nullptr;
    MemberExpr(SourceLoc l, ExprPtr o, std::string m)
        : Expr(ExprKind::Member, l), object(std::move(o)), member(std::move(m)) {}
};

struct IndexExpr : Expr {
    ExprPtr object, index;
    IndexExpr(SourceLoc l, ExprPtr o, ExprPtr i)
        : Expr(ExprKind::Index, l), object(std::move(o)), index(std::move(i)) {}
};

struct TryExpr : Expr { // expr?
    ExprPtr operand;
    TryExpr(SourceLoc l, ExprPtr e) : Expr(ExprKind::Try, l), operand(std::move(e)) {}
};

struct AllocExpr : Expr {
    // alloc Buffer(1024)  -> init is the constructor call
    // alloc [u8; 4096]    -> arrayType is set
    ExprPtr init;
    TypeRefPtr arrayType;
    explicit AllocExpr(SourceLoc l) : Expr(ExprKind::Alloc, l) {}
};

struct ArrayLitExpr : Expr {
    std::vector<ExprPtr> elems;
    explicit ArrayLitExpr(SourceLoc l) : Expr(ExprKind::ArrayLit, l) {}
};

struct Block;

struct Pattern {
    enum class Kind { Wildcard, Binding, Literal, Variant };
    Kind kind;
    SourceLoc loc;
    std::string name;               // Binding name, or Variant path ("Ok", "Color.Red")
    ExprPtr literal;                // Literal
    std::vector<PatternPtr> fields; // Variant payload
    bool hasParens = false;         // Variant written with (...)
    const Symbol* sym = nullptr;    // the variant, or the new local for a binding
};

struct MatchArm {
    PatternPtr pattern;
    ExprPtr expr;                 // either an expression...
    std::unique_ptr<Block> block; // ...or a block
};

struct MatchExpr : Expr {
    ExprPtr subject;
    std::vector<MatchArm> arms;
    explicit MatchExpr(SourceLoc l) : Expr(ExprKind::Match, l) {}
};

// ---------------------------------------------------------------- statements

enum class StmtKind { Let, Assign, Expr, If, While, For, Return, Break, Continue, Free, Asm, Block };

struct Stmt {
    StmtKind kind;
    SourceLoc loc;
    explicit Stmt(StmtKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Stmt() = default;
};

struct Block : Stmt {
    std::vector<StmtPtr> stmts;
    explicit Block(SourceLoc l) : Stmt(StmtKind::Block, l) {}
};

struct LetStmt : Stmt {
    bool mutable_ = false; // var vs let
    std::string name;
    TypeRefPtr type; // optional
    ExprPtr init;    // optional for var
    const Symbol* sym = nullptr;
    LetStmt(SourceLoc l) : Stmt(StmtKind::Let, l) {}
};

struct AssignStmt : Stmt {
    std::string op; // "=", "+=", ...
    ExprPtr target, value;
    AssignStmt(SourceLoc l) : Stmt(StmtKind::Assign, l) {}
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt(SourceLoc l, ExprPtr e) : Stmt(StmtKind::Expr, l), expr(std::move(e)) {}
};

struct IfStmt : Stmt {
    ExprPtr cond;
    std::unique_ptr<Block> then;
    StmtPtr otherwise; // Block or nested IfStmt, may be null
    IfStmt(SourceLoc l) : Stmt(StmtKind::If, l) {}
};

struct WhileStmt : Stmt {
    ExprPtr cond;
    std::unique_ptr<Block> body;
    WhileStmt(SourceLoc l) : Stmt(StmtKind::While, l) {}
};

struct ForStmt : Stmt {
    std::string var;
    const Symbol* sym = nullptr; // the loop variable
    ExprPtr iterable;
    std::unique_ptr<Block> body;
    ForStmt(SourceLoc l) : Stmt(StmtKind::For, l) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value; // may be null
    ReturnStmt(SourceLoc l) : Stmt(StmtKind::Return, l) {}
};

struct FreeStmt : Stmt {
    ExprPtr target;
    FreeStmt(SourceLoc l) : Stmt(StmtKind::Free, l) {}
};

struct AsmStmt : Stmt {
    std::vector<std::string> lines;
    AsmStmt(SourceLoc l) : Stmt(StmtKind::Asm, l) {}
};

// ---------------------------------------------------------------- declarations

enum class DeclKind { Use, Include, Function, Type, Class, Enum, Interface };

struct Decl {
    DeclKind kind;
    SourceLoc loc;
    bool isPub = false;
    explicit Decl(DeclKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Decl() = default;
};

struct UseDecl : Decl {
    std::string path;  // "Atomic.Memory"
    std::string alias; // empty if none
    UseDecl(SourceLoc l) : Decl(DeclKind::Use, l) {}
};

// `include shapes` in a module's root file adds shapes.h2 (same directory)
// to the module.
struct IncludeDecl : Decl {
    std::string name;
    IncludeDecl(SourceLoc l) : Decl(DeclKind::Include, l) {}
};

struct Param {
    SourceLoc loc;
    std::string name;
    bool isOwn = false; // `own T`: the function takes ownership (moves the argument in)
    TypeRefPtr type;
    const Symbol* sym = nullptr;
};

// `new function` and `new operator` share this node.
struct FunctionDecl : Decl {
    bool isOperator = false;
    std::string name; // function name, or operator symbol ("+", "==", "[]", "unary-")
    std::vector<GenericParam> generics;
    std::vector<Param> params;
    TypeRefPtr returnType;      // null means void
    std::unique_ptr<Block> body; // null for interface signatures
    FunctionDecl(SourceLoc l) : Decl(DeclKind::Function, l) {}
};

struct Field {
    SourceLoc loc;
    bool isPub = false;
    std::string name;
    TypeRefPtr type;
};

struct TypeDecl : Decl {
    // Layout:   new type string { data: ptr<u8>, len: uint }
    // Bits:     new type i32 : bits(32) signed
    // Float:    new type f64 : float(64)
    // Distinct: new type Meters : f64    (a new type with f64's representation)
    // Alias:    new type int = i64       (another name for the same type)
    enum class Base { Layout, Bits, Float, Distinct, Alias };
    std::string name;
    std::vector<GenericParam> generics;
    Base base = Base::Layout;
    uint32_t bits = 0;       // Bits / Float
    bool isSigned = false;   // Bits
    TypeRefPtr aliasOf;      // Distinct / Alias
    std::vector<Field> fields;
    std::vector<std::unique_ptr<FunctionDecl>> operators;
    TypeDecl(SourceLoc l) : Decl(DeclKind::Type, l) {}
};

struct ClassDecl : Decl {
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<Field> fields;
    std::vector<std::unique_ptr<FunctionDecl>> methods; // functions and operators
    ClassDecl(SourceLoc l) : Decl(DeclKind::Class, l) {}
};

struct Variant {
    SourceLoc loc;
    std::string name;
    std::vector<TypeRefPtr> payload;
};

struct EnumDecl : Decl {
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<Variant> variants;
    EnumDecl(SourceLoc l) : Decl(DeclKind::Enum, l) {}
};

struct InterfaceDecl : Decl {
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<std::unique_ptr<FunctionDecl>> members; // signatures (body may be null)
    InterfaceDecl(SourceLoc l) : Decl(DeclKind::Interface, l) {}
};

struct Module {
    std::string fileName;
    std::vector<DeclPtr> decls;
};

} // namespace h2
