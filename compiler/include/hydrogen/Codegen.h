#pragma once
#include "hydrogen/Checker.h"
#include "hydrogen/Program.h"
#include "hydrogen/Types.h"

#include <ostream>

namespace h2 {

// Lowers a checked program to textual LLVM IR. The program's `main` becomes
// the C entry point. Returns false (after reporting) on unsupported features.
// `checks`: stop the program on integer overflow and division by zero (debug builds).
bool emitLLVM(Program& program, TypeContext& types, const SemaInfo& info, Diagnostics& diags, std::ostream& out,
              bool checks = true);

} // namespace h2
