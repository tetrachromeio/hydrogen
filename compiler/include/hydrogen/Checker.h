#pragma once
#include "hydrogen/Program.h"
#include "hydrogen/Types.h"

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

// Type-checks every function body in the program. Expr::type is filled in.
// If `notes` is given, records the type of every let/var in the main module.
void typecheckProgram(Program& program, TypeContext& types, Diagnostics& diags,
                      std::vector<LocalTypeNote>* notes = nullptr);

} // namespace h2
