#include "hydrogen/Toml.h"

#include <cctype>

namespace h2::toml {

const char* Value::typeName() const {
    switch (v.index()) {
    case 0: return "string";
    case 1: return "integer";
    case 2: return "boolean";
    case 3: return "array";
    default: return "table";
    }
}

namespace {

struct Error {
    std::string msg;
    int line;
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    Table run() {
        Table root;
        Table* current = &root;
        for (;;) {
            skipSpaceAndNewlines();
            if (atEnd()) return root;
            if (peek() == '[') {
                ++i_;
                std::vector<std::string> path = parseKeyPath(']');
                expect(']');
                current = &root;
                for (const auto& k : path) {
                    auto [it, inserted] = current->try_emplace(k);
                    if (inserted) {
                        it->second.v = std::make_shared<Table>();
                        it->second.line = line_;
                    } else if (!it->second.asTable()) {
                        throw Error{"'" + k + "' is already a " + it->second.typeName(), line_};
                    }
                    current = std::get<std::shared_ptr<Table>>(it->second.v).get();
                }
            } else {
                parseKeyValue(*current);
            }
            endOfLine();
        }
    }

private:
    const std::string& s_;
    size_t i_ = 0;
    int line_ = 1;

    bool atEnd() const { return i_ >= s_.size(); }
    char peek() const { return atEnd() ? '\0' : s_[i_]; }

    void skipSpace() {
        while (!atEnd() && (peek() == ' ' || peek() == '\t' || peek() == '\r')) ++i_;
        if (peek() == '#')
            while (!atEnd() && peek() != '\n') ++i_;
    }
    void skipSpaceAndNewlines() {
        for (;;) {
            skipSpace();
            if (peek() != '\n') return;
            ++i_;
            ++line_;
        }
    }
    void expect(char c) {
        skipSpace();
        if (peek() != c) throw Error{std::string("expected '") + c + "'", line_};
        ++i_;
    }
    void endOfLine() {
        skipSpace();
        if (!atEnd() && peek() != '\n') throw Error{"unexpected text after value", line_};
    }

    std::string parseKey() {
        skipSpace();
        if (peek() == '"') return parseString();
        std::string k;
        while (!atEnd() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_' || peek() == '-'))
            k += s_[i_++];
        if (k.empty()) throw Error{"expected a key", line_};
        return k;
    }

    std::vector<std::string> parseKeyPath(char terminator) {
        std::vector<std::string> path{parseKey()};
        skipSpace();
        while (peek() == '.') {
            ++i_;
            path.push_back(parseKey());
            skipSpace();
        }
        (void)terminator;
        return path;
    }

    void parseKeyValue(Table& t) {
        int keyLine = line_;
        std::vector<std::string> path = parseKeyPath('=');
        expect('=');
        Value v = parseValue();
        Table* target = &t;
        for (size_t k = 0; k + 1 < path.size(); ++k) {
            auto [it, inserted] = target->try_emplace(path[k]);
            if (inserted) it->second.v = std::make_shared<Table>();
            if (!it->second.asTable()) throw Error{"'" + path[k] + "' is not a table", keyLine};
            target = std::get<std::shared_ptr<Table>>(it->second.v).get();
        }
        if (target->count(path.back())) throw Error{"duplicate key '" + path.back() + "'", keyLine};
        (*target)[path.back()] = std::move(v);
    }

    std::string parseString() {
        ++i_; // opening quote
        std::string out;
        while (!atEnd() && peek() != '"') {
            char c = s_[i_++];
            if (c == '\n') throw Error{"unterminated string", line_};
            if (c != '\\') {
                out += c;
                continue;
            }
            if (atEnd()) throw Error{"unterminated string", line_};
            char e = s_[i_++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            default: throw Error{std::string("unknown escape '\\") + e + "'", line_};
            }
        }
        if (atEnd()) throw Error{"unterminated string", line_};
        ++i_;
        return out;
    }

    Value parseValue() {
        skipSpace();
        Value v;
        v.line = line_;
        char c = peek();
        if (c == '"') {
            v.v = parseString();
        } else if (c == '[') {
            ++i_;
            auto arr = std::make_shared<Array>();
            for (;;) {
                skipSpaceAndNewlines();
                if (peek() == ']') break;
                arr->push_back(parseValue());
                skipSpaceAndNewlines();
                if (peek() == ',') {
                    ++i_;
                    continue;
                }
                if (peek() != ']') throw Error{"expected ',' or ']' in array", line_};
            }
            ++i_;
            v.v = arr;
        } else if (c == '{') {
            ++i_;
            auto tbl = std::make_shared<Table>();
            skipSpace();
            if (peek() == '}') {
                ++i_;
            } else {
                for (;;) {
                    parseKeyValue(*tbl);
                    skipSpace();
                    if (peek() == ',') {
                        ++i_;
                        continue;
                    }
                    if (peek() == '}') {
                        ++i_;
                        break;
                    }
                    throw Error{"expected ',' or '}' in inline table", line_};
                }
            }
            v.v = tbl;
        } else if (s_.compare(i_, 4, "true") == 0) {
            i_ += 4;
            v.v = true;
        } else if (s_.compare(i_, 5, "false") == 0) {
            i_ += 5;
            v.v = false;
        } else if (c == '-' || c == '+' || std::isdigit(static_cast<unsigned char>(c))) {
            std::string num;
            if (c == '-' || c == '+') num += s_[i_++];
            while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')
                if (s_[i_++] != '_') num += s_[i_ - 1];
            if (num.empty() || num == "-" || num == "+") throw Error{"invalid number", line_};
            v.v = static_cast<int64_t>(std::stoll(num));
        } else {
            throw Error{"expected a value (string, number, true/false, [array] or {table})", line_};
        }
        return v;
    }
};

} // namespace

ParseResult parse(const std::string& text) {
    ParseResult r;
    try {
        r.root = Parser(text).run();
    } catch (const Error& e) {
        r.error = e.msg;
        r.errorLine = e.line;
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

} // namespace h2::toml
