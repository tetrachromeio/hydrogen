#pragma once
#include "hydrogen/Source.h"

#include <ostream>
#include <string>
#include <vector>

namespace h2 {

struct Diagnostic {
    std::string file;
    SourceLoc loc;
    std::string message;
};

// Collects errors across all files of a compilation.
// `setFile` selects the file that error(loc, msg) reports against.
class Diagnostics {
public:
    explicit Diagnostics(std::string fileName = {}) : file_(std::move(fileName)) {}

    void setFile(std::string fileName) { file_ = std::move(fileName); }
    const std::string& fileName() const { return file_; }

    void error(SourceLoc loc, std::string message) { error(file_, loc, std::move(message)); }
    void error(const std::string& file, SourceLoc loc, std::string message);

    bool hasErrors() const { return !errors_.empty(); }
    const std::vector<Diagnostic>& errors() const { return errors_; }

    // Prints "file:line:col: error: message" for each error.
    void print(std::ostream& os) const;

private:
    std::string file_;
    std::vector<Diagnostic> errors_;
};

} // namespace h2
