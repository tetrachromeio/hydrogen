#pragma once
#include "hydrogen/Program.h"
#include "hydrogen/Types.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace h2 {

// The inferred type of a local variable, for `h2c --types`.
struct LocalTypeNote {
    std::string file;
    SourceLoc loc;
    std::string name;
    std::string type;
};

// Facts the checker works out that code generation needs.
struct SemaInfo {
    struct Call {
        const FunctionDecl* fn = nullptr; // function, method or init (null for field-wise construction / variants)
        std::map<const Symbol*, const Type*> subst; // generic bindings at this call (may mention outer generics)
    };
    std::map<const Expr*, Call> calls;
    std::set<const Expr*> viewCoerced; // `string` expressions used where a `str` is expected
    std::map<const Expr*, const Type*> dynCoerced; // ptr<T> expressions used as `any I` -> T
    std::set<const Expr*> consumingMatches; // matches that take ownership of their subject
};

// Type-checks every function body in the program. Expr::type is filled in.
// If `notes` is given, records the type of every let/var in the main module.
// If `info` is given, it receives what code generation needs.
void typecheckProgram(Program& program, TypeContext& types, Diagnostics& diags,
                      std::vector<LocalTypeNote>* notes = nullptr, SemaInfo* info = nullptr);

} // namespace h2
