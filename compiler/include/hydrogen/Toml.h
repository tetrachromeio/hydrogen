#pragma once
// A small TOML subset, enough for hy.toml:
//   [table] and [table.sub] headers, `key = value` pairs, # comments
//   values: "strings" (with \" \\ \n \t escapes), integers, true/false,
//           [arrays] and { inline = "tables" }

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace h2::toml {

struct Value;
using Table = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
    std::variant<std::string, int64_t, bool, std::shared_ptr<Array>, std::shared_ptr<Table>> v;
    int line = 0;

    const std::string* asString() const { return std::get_if<std::string>(&v); }
    const int64_t* asInt() const { return std::get_if<int64_t>(&v); }
    const bool* asBool() const { return std::get_if<bool>(&v); }
    const Table* asTable() const {
        auto p = std::get_if<std::shared_ptr<Table>>(&v);
        return p ? p->get() : nullptr;
    }
    const Array* asArray() const {
        auto p = std::get_if<std::shared_ptr<Array>>(&v);
        return p ? p->get() : nullptr;
    }
    const char* typeName() const;
};

struct ParseResult {
    Table root;
    std::string error; // empty on success
    int errorLine = 0;
};

ParseResult parse(const std::string& text);

} // namespace h2::toml
