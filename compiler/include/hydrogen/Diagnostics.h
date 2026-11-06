#pragma once
#include "hydrogen/Source.h"

#include <ostream>
#include <string>
#include <vector>

namespace h2 {

struct Diagnostic {
    SourceLoc loc;
    std::string message;
};

class Diagnostics {
public:
    explicit Diagnostics(std::string fileName) : file_(std::move(fileName)) {}

    void error(SourceLoc loc, std::string message);
    bool hasErrors() const { return !errors_.empty(); }
    const std::vector<Diagnostic>& errors() const { return errors_; }
    const std::string& fileName() const { return file_; }

    // Prints "file:line:col: error: message" for each error.
    void print(std::ostream& os) const;

private:
    std::string file_;
    std::vector<Diagnostic> errors_;
};

} // namespace h2
