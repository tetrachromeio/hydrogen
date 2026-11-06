#include "hydrogen/Diagnostics.h"

namespace h2 {

void Diagnostics::error(SourceLoc loc, std::string message) {
    errors_.push_back({loc, std::move(message)});
}

void Diagnostics::print(std::ostream& os) const {
    for (const auto& d : errors_)
        os << file_ << ':' << d.loc.line << ':' << d.loc.col << ": error: " << d.message << '\n';
}

} // namespace h2
