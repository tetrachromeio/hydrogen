#pragma once
#include <filesystem>
#include <vector>

namespace h2 {

// Runs a Language Server Protocol server on stdin/stdout until the client
// sends `exit`. Provides diagnostics, go-to-definition and hover.
int runLanguageServer(const std::vector<std::filesystem::path>& stdlibPaths);

} // namespace h2
