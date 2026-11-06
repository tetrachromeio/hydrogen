#pragma once
#include <cstdint>
#include <string>

namespace h2 {

struct SourceLoc {
    uint32_t line = 1;
    uint32_t col = 1;
};

} // namespace h2
