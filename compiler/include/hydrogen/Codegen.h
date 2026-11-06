#pragma once
#include "hydrogen/Checker.h"
#include "hydrogen/Program.h"
#include "hydrogen/Types.h"

#include <ostream>

namespace h2 {

// Lowers a checked program to textual LLVM IR. The program's `main` becomes
// the C entry point. Returns false (after reporting) on unsupported features.
bool emitLLVM(Program& program, TypeContext& types, const SemaInfo& info, Diagnostics& diags, std::ostream& out);

} // namespace h2
