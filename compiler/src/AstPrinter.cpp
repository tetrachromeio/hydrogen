#include "hydrogen/AstPrinter.h"

#include <sstream>

namespace h2 {
namespace {

class Printer {
public:
    explicit Printer(std::ostream& os) : os_(os) {}

    void module(const Module& m) {
        os_ << "(module";
        for (const auto& d : m.decls) {
            nl(1);
            decl(*d, 1);
        }
        os_ << ")\n";
    }

private:
    std::ostream& os_;

    void nl(int depth) {
        os_ << '\n';
        for (int i = 0; i < depth; ++i) os_ << "  ";
    }

    static std::string quote(const std::string& s, char q) {
        std::string out(1, q);
        for (unsigned char c : s) {
            switch (c) {
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\0': out += "\\0"; break;
            case '\\': out += "\\\\"; break;
            default:
                if (c == static_cast<unsigned char>(q)) {
                    out += '\\';
                    out += static_cast<char>(c);
                } else if (c < 0x20 || c == 0x7f) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\x";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                } else {
                    out += static_cast<char>(c);
                }
            }
        }
        out += q;
        return out;
    }

    // ---------------------------------------------------------- types

    std::string typeStr(const TypeRef& t) {
        if (t.kind == TypeRef::Kind::Array) {
            std::string s = "[" + typeStr(*t.elem);
            if (t.size) s += "; " + exprStr(*t.size);
            return s + "]";
        }
        std::string s = t.name;
        if (!t.args.empty()) {
            s += '<';
            for (size_t i = 0; i < t.args.size(); ++i) {
                if (i) s += ", ";
                s += typeStr(*t.args[i]);
            }
            s += '>';
        }
        return s;
    }

    std::string genericsStr(const std::vector<GenericParam>& gs) {
        if (gs.empty()) return "";
        std::string s = " <";
        for (size_t i = 0; i < gs.size(); ++i) {
            if (i) s += ", ";
            s += gs[i].name;
            for (size_t j = 0; j < gs[i].constraints.size(); ++j) {
                s += j ? " + " : ": ";
                s += typeStr(*gs[i].constraints[j]);
            }
        }
        return s + ">";
    }

    // ---------------------------------------------------------- expressions

    std::string exprStr(const Expr& e, int depth = 0) {
        std::ostringstream ss;
        Printer sub(ss);
        sub.expr(e, depth);
        return ss.str();
    }

    void exprList(const std::vector<ExprPtr>& es, int depth) {
        for (const auto& a : es) {
            os_ << ' ';
            expr(*a, depth);
        }
    }

    void expr(const Expr& e, int depth) {
        switch (e.kind) {
        case ExprKind::IntLit:
        case ExprKind::FloatLit:
        case ExprKind::BoolLit: os_ << static_cast<const LiteralExpr&>(e).value; break;
        case ExprKind::StringLit: os_ << quote(static_cast<const LiteralExpr&>(e).value, '"'); break;
        case ExprKind::CharLit: os_ << quote(static_cast<const LiteralExpr&>(e).value, '\''); break;
        case ExprKind::Self: os_ << "self"; break;
        case ExprKind::Ident: {
            const auto& id = static_cast<const IdentExpr&>(e);
            os_ << id.name;
            if (!id.typeArgs.empty()) {
                os_ << '<';
                for (size_t i = 0; i < id.typeArgs.size(); ++i) {
                    if (i) os_ << ", ";
                    os_ << typeStr(*id.typeArgs[i]);
                }
                os_ << '>';
            }
            break;
        }
        case ExprKind::Unary: {
            const auto& u = static_cast<const UnaryExpr&>(e);
            os_ << '(' << u.op << ' ';
            expr(*u.operand, depth);
            os_ << ')';
            break;
        }
        case ExprKind::Binary: {
            const auto& b = static_cast<const BinaryExpr&>(e);
            os_ << '(' << b.op << ' ';
            expr(*b.lhs, depth);
            os_ << ' ';
            expr(*b.rhs, depth);
            os_ << ')';
            break;
        }
        case ExprKind::Call: {
            const auto& c = static_cast<const CallExpr&>(e);
            os_ << "(call ";
            expr(*c.callee, depth);
            exprList(c.args, depth);
            os_ << ')';
            break;
        }
        case ExprKind::Member: {
            const auto& m = static_cast<const MemberExpr&>(e);
            os_ << "(. ";
            expr(*m.object, depth);
            os_ << ' ' << m.member << ')';
            break;
        }
        case ExprKind::Index: {
            const auto& i = static_cast<const IndexExpr&>(e);
            os_ << "(index ";
            expr(*i.object, depth);
            os_ << ' ';
            expr(*i.index, depth);
            os_ << ')';
            break;
        }
        case ExprKind::Try:
            os_ << "(? ";
            expr(*static_cast<const TryExpr&>(e).operand, depth);
            os_ << ')';
            break;
        case ExprKind::Alloc: {
            const auto& a = static_cast<const AllocExpr&>(e);
            os_ << "(alloc ";
            if (a.arrayType)
                os_ << typeStr(*a.arrayType);
            else
                expr(*a.init, depth);
            os_ << ')';
            break;
        }
        case ExprKind::ArrayLit:
            os_ << "(array";
            exprList(static_cast<const ArrayLitExpr&>(e).elems, depth);
            os_ << ')';
            break;
        case ExprKind::Match: {
            const auto& m = static_cast<const MatchExpr&>(e);
            os_ << "(match ";
            expr(*m.subject, depth);
            for (const auto& arm : m.arms) {
                nl(depth + 1);
                os_ << '(' << patternStr(*arm.pattern) << " =>";
                if (arm.block) {
                    nl(depth + 2);
                    stmt(*arm.block, depth + 2);
                } else {
                    os_ << ' ';
                    expr(*arm.expr, depth + 1);
                }
                os_ << ')';
            }
            os_ << ')';
            break;
        }
        }
    }

    std::string patternStr(const Pattern& p) {
        switch (p.kind) {
        case Pattern::Kind::Wildcard: return "_";
        case Pattern::Kind::Binding: return p.name;
        case Pattern::Kind::Literal: return exprStr(*p.literal);
        case Pattern::Kind::Variant: {
            std::string s = "(variant " + p.name;
            for (const auto& f : p.fields) s += " " + patternStr(*f);
            return s + ")";
        }
        }
        return "?";
    }

    // ---------------------------------------------------------- statements

    void stmt(const Stmt& s, int depth) {
        switch (s.kind) {
        case StmtKind::Block: {
            const auto& b = static_cast<const Block&>(s);
            os_ << "(block";
            for (const auto& c : b.stmts) {
                nl(depth + 1);
                stmt(*c, depth + 1);
            }
            os_ << ')';
            break;
        }
        case StmtKind::Let: {
            const auto& l = static_cast<const LetStmt&>(s);
            os_ << '(' << (l.mutable_ ? "var " : "let ") << l.name;
            if (l.type) os_ << " : " << typeStr(*l.type);
            if (l.init) {
                os_ << " = ";
                expr(*l.init, depth);
            }
            os_ << ')';
            break;
        }
        case StmtKind::Assign: {
            const auto& a = static_cast<const AssignStmt&>(s);
            os_ << '(' << a.op << ' ';
            expr(*a.target, depth);
            os_ << ' ';
            expr(*a.value, depth);
            os_ << ')';
            break;
        }
        case StmtKind::Expr: expr(*static_cast<const ExprStmt&>(s).expr, depth); break;
        case StmtKind::If: {
            const auto& i = static_cast<const IfStmt&>(s);
            os_ << "(if ";
            expr(*i.cond, depth);
            nl(depth + 1);
            stmt(*i.then, depth + 1);
            if (i.otherwise) {
                nl(depth + 1);
                os_ << "(else ";
                stmt(*i.otherwise, depth + 1);
                os_ << ')';
            }
            os_ << ')';
            break;
        }
        case StmtKind::While: {
            const auto& w = static_cast<const WhileStmt&>(s);
            os_ << "(while ";
            expr(*w.cond, depth);
            nl(depth + 1);
            stmt(*w.body, depth + 1);
            os_ << ')';
            break;
        }
        case StmtKind::For: {
            const auto& f = static_cast<const ForStmt&>(s);
            os_ << "(for " << f.var << ' ';
            expr(*f.iterable, depth);
            nl(depth + 1);
            stmt(*f.body, depth + 1);
            os_ << ')';
            break;
        }
        case StmtKind::Return: {
            const auto& r = static_cast<const ReturnStmt&>(s);
            os_ << "(return";
            if (r.value) {
                os_ << ' ';
                expr(*r.value, depth);
            }
            os_ << ')';
            break;
        }
        case StmtKind::Break: os_ << "(break)"; break;
        case StmtKind::Continue: os_ << "(continue)"; break;
        case StmtKind::Free:
            os_ << "(free ";
            expr(*static_cast<const FreeStmt&>(s).target, depth);
            os_ << ')';
            break;
        case StmtKind::Asm:
            os_ << "(asm";
            for (const auto& l : static_cast<const AsmStmt&>(s).lines) os_ << ' ' << quote(l, '"');
            os_ << ')';
            break;
        }
    }

    // ---------------------------------------------------------- declarations

    void fields(const std::vector<Field>& fs, int depth) {
        for (const auto& f : fs) {
            nl(depth);
            os_ << (f.isPub ? "(pub field " : "(field ") << f.name << " : " << typeStr(*f.type) << ')';
        }
    }

    void function(const FunctionDecl& f, int depth) {
        if (f.isPub && !f.isOperator) os_ << "(pub ";
        else os_ << '(';
        os_ << (f.isOperator ? "operator " : "function ") << f.name << genericsStr(f.generics) << " (";
        for (size_t i = 0; i < f.params.size(); ++i) {
            if (i) os_ << ' ';
            os_ << '(' << f.params[i].name << " : " << typeStr(*f.params[i].type) << ')';
        }
        os_ << ')';
        if (f.returnType) os_ << " -> " << typeStr(*f.returnType);
        if (f.body) {
            nl(depth + 1);
            stmt(*f.body, depth + 1);
        }
        os_ << ')';
    }

    void decl(const Decl& d, int depth) {
        switch (d.kind) {
        case DeclKind::Use: {
            const auto& u = static_cast<const UseDecl&>(d);
            os_ << (u.isPub ? "(pub use " : "(use ") << u.path;
            if (!u.alias.empty()) os_ << " as " << u.alias;
            os_ << ')';
            break;
        }
        case DeclKind::Function: function(static_cast<const FunctionDecl&>(d), depth); break;
        case DeclKind::Type: {
            const auto& t = static_cast<const TypeDecl&>(d);
            os_ << (t.isPub ? "(pub type " : "(type ") << t.name << genericsStr(t.generics);
            switch (t.base) {
            case TypeDecl::Base::Layout: break;
            case TypeDecl::Base::Bits: os_ << " (bits " << t.bits << (t.isSigned ? " signed)" : ")"); break;
            case TypeDecl::Base::Float: os_ << " (float " << t.bits << ')'; break;
            case TypeDecl::Base::Alias: os_ << " (on " << typeStr(*t.aliasOf) << ')'; break;
            }
            fields(t.fields, depth + 1);
            for (const auto& op : t.operators) {
                nl(depth + 1);
                function(*op, depth + 1);
            }
            os_ << ')';
            break;
        }
        case DeclKind::Class: {
            const auto& c = static_cast<const ClassDecl&>(d);
            os_ << (c.isPub ? "(pub class " : "(class ") << c.name << genericsStr(c.generics);
            fields(c.fields, depth + 1);
            for (const auto& m : c.methods) {
                nl(depth + 1);
                function(*m, depth + 1);
            }
            os_ << ')';
            break;
        }
        case DeclKind::Enum: {
            const auto& e = static_cast<const EnumDecl&>(d);
            os_ << (e.isPub ? "(pub enum " : "(enum ") << e.name << genericsStr(e.generics);
            for (const auto& v : e.variants) {
                nl(depth + 1);
                os_ << '(' << v.name;
                for (const auto& p : v.payload) os_ << ' ' << typeStr(*p);
                os_ << ')';
            }
            os_ << ')';
            break;
        }
        case DeclKind::Interface: {
            const auto& i = static_cast<const InterfaceDecl&>(d);
            os_ << (i.isPub ? "(pub interface " : "(interface ") << i.name << genericsStr(i.generics);
            for (const auto& m : i.members) {
                nl(depth + 1);
                function(*m, depth + 1);
            }
            os_ << ')';
            break;
        }
        }
    }
};

} // namespace

void printAst(const Module& m, std::ostream& os) { Printer(os).module(m); }

} // namespace h2
