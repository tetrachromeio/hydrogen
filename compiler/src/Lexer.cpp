#include "hydrogen/Lexer.h"

#include <cctype>
#include <unordered_map>

namespace h2 {

std::string_view tokName(Tok k) {
    switch (k) {
    case Tok::Eof: return "end of file";
    case Tok::Error: return "error";
    case Tok::Ident: return "identifier";
    case Tok::IntLit: return "integer literal";
    case Tok::FloatLit: return "float literal";
    case Tok::StringLit: return "string literal";
    case Tok::CharLit: return "char literal";
#define X(name, text) case Tok::Kw##name: return "'" text "'";
        H2_KEYWORDS(X)
#undef X
#define X(name, text) case Tok::name: return "'" text "'";
        H2_PUNCT(X)
#undef X
    }
    return "?";
}

namespace {

const std::unordered_map<std::string_view, Tok>& keywords() {
    static const std::unordered_map<std::string_view, Tok> map = {
#define X(name, text) {text, Tok::Kw##name},
        H2_KEYWORDS(X)
#undef X
    };
    return map;
}

class Lexer {
public:
    Lexer(std::string_view src, Diagnostics& diags) : src_(src), diags_(diags) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        bool sawNewline = true; // start of file counts as a line start
        for (;;) {
            bool sawSpace = skipTrivia(sawNewline);
            Token t;
            t.loc = here();
            t.newlineBefore = sawNewline;
            t.joint = !sawSpace && !out.empty();
            if (atEnd()) {
                t.kind = Tok::Eof;
                out.push_back(std::move(t));
                return out;
            }
            if (!lexToken(t)) {
                sawNewline = false;
                continue; // error already reported, char skipped
            }
            out.push_back(std::move(t));
            sawNewline = false;
        }
    }

private:
    std::string_view src_;
    Diagnostics& diags_;
    size_t pos_ = 0;
    uint32_t line_ = 1, col_ = 1;

    bool atEnd() const { return pos_ >= src_.size(); }
    char peek(size_t ahead = 0) const {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }
    SourceLoc here() const { return {line_, col_}; }

    char advance() {
        char c = src_[pos_++];
        if (c == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        return c;
    }

    // Skips whitespace and comments. Returns true if anything was skipped.
    bool skipTrivia(bool& sawNewline) {
        bool skipped = false;
        while (!atEnd()) {
            char c = peek();
            if (c == '\n') {
                sawNewline = true;
                advance();
            } else if (c == ' ' || c == '\t' || c == '\r') {
                advance();
            } else if (c == '/' && peek(1) == '/') {
                while (!atEnd() && peek() != '\n') advance();
            } else if (c == '/' && peek(1) == '*') {
                SourceLoc start = here();
                advance(), advance();
                int depth = 1;
                while (!atEnd() && depth > 0) {
                    if (peek() == '/' && peek(1) == '*') {
                        advance(), advance(), ++depth;
                    } else if (peek() == '*' && peek(1) == '/') {
                        advance(), advance(), --depth;
                    } else {
                        if (peek() == '\n') sawNewline = true;
                        advance();
                    }
                }
                if (depth > 0) diags_.error(start, "unterminated block comment");
            } else {
                break;
            }
            skipped = true;
        }
        return skipped;
    }

    bool lexToken(Token& t) {
        char c = peek();
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            lexIdent(t);
            return true;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            lexNumber(t);
            return true;
        }
        if (c == '"') return lexString(t);
        if (c == '\'') return lexChar(t);
        return lexPunct(t);
    }

    void lexIdent(Token& t) {
        size_t start = pos_;
        while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') advance();
        std::string_view word = src_.substr(start, pos_ - start);
        auto it = keywords().find(word);
        t.kind = it != keywords().end() ? it->second : Tok::Ident;
        t.text = std::string(word);
    }

    void lexNumber(Token& t) {
        SourceLoc start = here();
        std::string digits;
        auto takeDigits = [&](auto isDigit) {
            while (isDigit(peek()) || peek() == '_') {
                char ch = advance();
                if (ch != '_') digits += ch;
            }
        };
        auto isDec = [](char ch) { return ch >= '0' && ch <= '9'; };

        t.kind = Tok::IntLit;
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'b' || peek(1) == 'o')) {
            char base = peek(1);
            digits += advance();
            digits += advance();
            size_t before = digits.size();
            if (base == 'x')
                takeDigits([](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; });
            else if (base == 'b')
                takeDigits([](char ch) { return ch == '0' || ch == '1'; });
            else
                takeDigits([](char ch) { return ch >= '0' && ch <= '7'; });
            if (digits.size() == before) diags_.error(start, "number literal has no digits after base prefix");
        } else {
            takeDigits(isDec);
            // `1..5` is a range, not a float: require a digit after the dot.
            if (peek() == '.' && isDec(peek(1))) {
                t.kind = Tok::FloatLit;
                digits += advance();
                takeDigits(isDec);
            }
            if ((peek() == 'e' || peek() == 'E') &&
                (isDec(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && isDec(peek(2))))) {
                t.kind = Tok::FloatLit;
                digits += advance();
                if (peek() == '+' || peek() == '-') digits += advance();
                takeDigits(isDec);
            }
        }
        if (std::isalpha(static_cast<unsigned char>(peek())) || peek() == '_') {
            diags_.error(here(), "invalid character in number literal");
            while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') advance();
        }
        t.text = std::move(digits);
    }

    // Reads one escape sequence after the backslash. Returns false on error.
    bool readEscape(std::string& out) {
        SourceLoc at = here();
        if (atEnd()) return false;
        char e = advance();
        switch (e) {
        case 'n': out += '\n'; return true;
        case 't': out += '\t'; return true;
        case 'r': out += '\r'; return true;
        case '0': out += '\0'; return true;
        case '\\': out += '\\'; return true;
        case '\'': out += '\''; return true;
        case '"': out += '"'; return true;
        case 'x': {
            int v = 0;
            for (int i = 0; i < 2; ++i) {
                char h = peek();
                if (!std::isxdigit(static_cast<unsigned char>(h))) {
                    diags_.error(at, "\\x escape needs two hex digits");
                    return false;
                }
                advance();
                v = v * 16 + (std::isdigit(static_cast<unsigned char>(h)) ? h - '0' : (std::tolower(h) - 'a' + 10));
            }
            out += static_cast<char>(v);
            return true;
        }
        default:
            diags_.error(at, std::string("unknown escape sequence '\\") + e + "'");
            return false;
        }
    }

    bool lexString(Token& t) {
        SourceLoc start = here();
        advance(); // opening quote
        std::string value;
        while (!atEnd() && peek() != '"' && peek() != '\n') {
            if (peek() == '\\') {
                advance();
                readEscape(value);
            } else {
                value += advance();
            }
        }
        if (peek() != '"') {
            diags_.error(start, "unterminated string literal");
            t.kind = Tok::StringLit;
            t.text = std::move(value);
            return true;
        }
        advance();
        t.kind = Tok::StringLit;
        t.text = std::move(value);
        return true;
    }

    bool lexChar(Token& t) {
        SourceLoc start = here();
        advance();
        std::string value;
        if (peek() == '\\') {
            advance();
            readEscape(value);
        } else if (!atEnd() && peek() != '\'' && peek() != '\n') {
            value += advance();
        }
        if (peek() != '\'') {
            diags_.error(start, "char literal must contain exactly one character");
            while (!atEnd() && peek() != '\'' && peek() != '\n') advance();
            if (peek() == '\'') advance();
        } else {
            advance();
            if (value.empty()) diags_.error(start, "empty char literal");
        }
        t.kind = Tok::CharLit;
        t.text = std::move(value);
        return true;
    }

    bool lexPunct(Token& t) {
        char c = peek(), n = peek(1), n2 = peek(2);
        auto take = [&](Tok k, int len) {
            for (int i = 0; i < len; ++i) advance();
            t.kind = k;
            return true;
        };
        switch (c) {
        case '(': return take(Tok::LParen, 1);
        case ')': return take(Tok::RParen, 1);
        case '{': return take(Tok::LBrace, 1);
        case '}': return take(Tok::RBrace, 1);
        case '[': return take(Tok::LBracket, 1);
        case ']': return take(Tok::RBracket, 1);
        case ',': return take(Tok::Comma, 1);
        case ':': return take(Tok::Colon, 1);
        case ';': return take(Tok::Semi, 1);
        case '?': return take(Tok::Question, 1);
        case '~': return take(Tok::Tilde, 1);
        case '.': return n == '.' ? take(Tok::DotDot, 2) : take(Tok::Dot, 1);
        case '+': return n == '=' ? take(Tok::PlusAssign, 2) : take(Tok::Plus, 1);
        case '-':
            if (n == '>') return take(Tok::Arrow, 2);
            return n == '=' ? take(Tok::MinusAssign, 2) : take(Tok::Minus, 1);
        case '*': return n == '=' ? take(Tok::StarAssign, 2) : take(Tok::Star, 1);
        case '/': return n == '=' ? take(Tok::SlashAssign, 2) : take(Tok::Slash, 1);
        case '%': return n == '=' ? take(Tok::PercentAssign, 2) : take(Tok::Percent, 1);
        case '^': return n == '=' ? take(Tok::CaretAssign, 2) : take(Tok::Caret, 1);
        case '=':
            if (n == '=') return take(Tok::EqEq, 2);
            return n == '>' ? take(Tok::FatArrow, 2) : take(Tok::Assign, 1);
        case '!': return n == '=' ? take(Tok::NotEq, 2) : take(Tok::Bang, 1);
        case '&':
            if (n == '&') return take(Tok::AmpAmp, 2);
            return n == '=' ? take(Tok::AmpAssign, 2) : take(Tok::Amp, 1);
        case '|':
            if (n == '|') return take(Tok::PipePipe, 2);
            return n == '=' ? take(Tok::PipeAssign, 2) : take(Tok::Pipe, 1);
        case '<':
            if (n == '<') return n2 == '=' ? take(Tok::ShlAssign, 3) : take(Tok::Shl, 2);
            return n == '=' ? take(Tok::LessEq, 2) : take(Tok::Less, 1);
        case '>':
            // Never combine '>' with a following '>' (see Token.h).
            return n == '=' ? take(Tok::GreaterEq, 2) : take(Tok::Greater, 1);
        default: break;
        }
        diags_.error(here(), std::string("unexpected character '") + c + "'");
        advance();
        return false;
    }
};

} // namespace

std::vector<Token> lex(std::string_view src, Diagnostics& diags) {
    return Lexer(src, diags).run();
}

} // namespace h2
