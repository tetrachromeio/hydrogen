#include "hydrogen/Diagnostics.h"

#include <algorithm>
#include <map>

namespace h2 {

void Diagnostics::error(const std::string& file, SourceLoc loc, std::string message) {
    errors_.push_back({file, loc, std::move(message)});
}

void Diagnostics::print(std::ostream& os) const {
    // Files in the order they first reported an error; within a file, by position.
    std::map<std::string, size_t> fileOrder;
    for (const auto& d : errors_) fileOrder.emplace(d.file, fileOrder.size());
    std::vector<Diagnostic> sorted = errors_;
    std::stable_sort(sorted.begin(), sorted.end(), [&](const Diagnostic& a, const Diagnostic& b) {
        size_t fa = fileOrder[a.file], fb = fileOrder[b.file];
        if (fa != fb) return fa < fb;
        if (a.loc.line != b.loc.line) return a.loc.line < b.loc.line;
        return a.loc.col < b.loc.col;
    });
    for (const auto& d : sorted)
        os << d.file << ':' << d.loc.line << ':' << d.loc.col << ": error: " << d.message << '\n';
}

} // namespace h2
