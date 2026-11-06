#pragma once
#include "hydrogen/AST.h"
#include "hydrogen/Diagnostics.h"
#include "hydrogen/Token.h"

#include <vector>

namespace h2 {

// Parses a token stream into a Module. On syntax errors, reports to `diags`,
// recovers at the next declaration or statement, and keeps going.
Module parse(std::vector<Token> tokens, Diagnostics& diags);

} // namespace h2
