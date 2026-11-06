#pragma once
#include "hydrogen/AST.h"

#include <ostream>

namespace h2 {

// Dumps the AST as indented S-expressions (stable; used by golden tests).
void printAst(const Module& m, std::ostream& os);

} // namespace h2
