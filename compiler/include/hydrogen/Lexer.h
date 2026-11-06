#pragma once
#include "hydrogen/Diagnostics.h"
#include "hydrogen/Token.h"

#include <string_view>
#include <vector>

namespace h2 {

// Tokenizes a whole file. Always ends with an Eof token.
// Lexical errors are reported to `diags` and the offending text is skipped.
std::vector<Token> lex(std::string_view src, Diagnostics& diags);

} // namespace h2
