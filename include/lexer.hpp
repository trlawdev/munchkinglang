#pragma once

#include "errors.hpp"
#include "keywords.hpp"
#include "token.hpp"
#include <cctype>
#include <charconv>
#include <filesystem>
#include <string>
#include <vector>

namespace munx
{

    /// Streaming lexer: munx source text → token vector.
    /// Tracks 1-based line/column; token locations freeze at each token start.
    class lexer
    {
        const std::string &source_;                              ///< Input source (not owned).
        const std::unordered_set<std::string> &keywords_;        ///< Structural keyword set.
        std::filesystem::path path_;                             ///< Path for diagnostics.
        long long pos_{0};                                       ///< Byte offset into @ref source_.
        long long line_{1};                                      ///< Current 1-based line.
        long long col_{1};                                       ///< Current 1-based column.
        long long tok_line_{1};                                  ///< Line where the current token began.
        long long tok_col_{1};                                   ///< Column where the current token began.

    public:
        /// Bind @p source and @p keywords; @p path is used only in diagnostics.
        lexer(const std::string &source,
              const std::unordered_set<std::string> &keywords,
              std::filesystem::path path)
            : source_(source), keywords_(keywords), path_(std::move(path))
        {
        }

        lexer(const lexer &) = delete;
        lexer &operator=(const lexer &) = delete;

        /// Lex the entire source, appending a trailing @c END token.
        /// @return Owned token vector.
        std::vector<token> tokenize()
        {
            std::vector<token> tokens;
            // Rough heuristic to avoid repeated reallocation on large inputs.
            tokens.reserve(source_.size() / 4 + 16);
            while (true)
            {
                skip_trivia();
                tok_line_ = line_;
                tok_col_ = col_;
                if (eof())
                {
                    auto char_variant = value_variant{'\0'};
                    tokens.emplace_back(make(token_type::END, char_variant));
                    break;
                }
                tokens.emplace_back(next_token());
            }
            return tokens;
        }

        /// Alias for @ref tokenize (used by the CLI).
        std::vector<token> read_tokens() { return tokenize(); }

    private:
        /// @return True if @p pos_ + @p off is past the end of the source.
        bool eof(long long off = 0) const
        {
            return pos_ + off >= static_cast<long long>(source_.size());
        }

        /// @return Character at @p pos_ + @p off, or `'\0'` if out of range.
        char peek(long long off = 0) const
        {
            return eof(off) ? '\0' : source_[pos_ + off];
        }

        /// Consume @p n characters, updating line/column (handles newlines).
        void advance(long long n = 1)
        {
            for (long long i = 0; i < n && !eof(); ++i)
            {
                if (source_[pos_] == '\n')
                {
                    ++line_;
                    col_ = 1;
                }
                else
                {
                    ++col_;
                }
                ++pos_;
            }
        }

        /// Fast path: advance @p n chars known not to contain newlines.
        void advance_no_newline(long long n = 1)
        {
            pos_ += n;
            col_ += n;
        }

        /// Record a compile error at the current cursor position.
        void fail(const char *msg) const
        {
            fail_compile(path_.string() + ':' + std::to_string(line_) +
                                    ':' + std::to_string(col_) + ": error: " + msg);
        }

        /// Build a token stamped with @ref tok_line_ / @ref tok_col_.
        token make(token_type type, const value_variant &value) const
        {
            return token{tok_line_, tok_col_, type, std::move(const_cast<value_variant&>(value))};
        }

        /// Skip whitespace, `//` line comments, and `/* */` block comments.
        void skip_trivia()
        {
            while (!eof())
            {
                const char c = peek();
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                {
                    advance();
                    continue;
                }
                if (c == '/' && peek(1) == '/')
                {
                    advance_no_newline(2);
                    while (!eof() && peek() != '\n')
                    {
                        advance_no_newline();
                    }
                    continue;
                }
                if (c == '/' && peek(1) == '*')
                {
                    advance_no_newline(2);
                    while (!eof() && !(peek() == '*' && peek(1) == '/'))
                    {
                        advance();
                    }
                    if (eof())
                    {
                        fail("unterminated block comment");
                    }
                    advance_no_newline(2);
                    continue;
                }
                break;
            }
        }

        /// @return True if @p c may start an identifier.
        static bool is_ident_start(char c)
        {
            return c == '_' || std::isalpha(static_cast<unsigned char>(c));
        }

        /// @return True if @p c may continue an identifier.
        static bool is_ident_cont(char c)
        {
            return c == '_' || std::isalnum(static_cast<unsigned char>(c));
        }

        /// Lex the next non-trivia token (assumes trivia already skipped).
        token next_token()
        {
            // Regex literal: r"..."
            if (peek() == 'r' && peek(1) == '"')
            {
                return lex_regex();
            }

            if (is_ident_start(peek()))
            {
                return lex_ident();
            }

            if (std::isdigit(static_cast<unsigned char>(peek())))
            {
                return lex_number();
            }
            
            value_variant c{};
            switch (peek())
            {
            case '"':
                return lex_string();
            case '\'':
                return lex_char();
            case '(': {
                c = '(';
                advance_no_newline();
                return make(token_type::LPAREN, c);
            }
            case ')':
                c = ')';
                advance_no_newline();
                return make(token_type::RPAREN, c);
            case '{':
                c = '{';
                advance_no_newline();
                return make(token_type::LBRACE, c);
            case '}':
                c = '}';
                advance_no_newline();
                return make(token_type::RBRACE, c);
            case '[':
                c = '[';
                advance_no_newline();
                return make(token_type::LBRACKET, c);
            case ']':
                c = ']';
                advance_no_newline();
                return make(token_type::RBRACKET, c);
            case ',':
                c = ',';
                advance_no_newline();
                return make(token_type::COMMA, c);
            case ';':
                c = ';';
                advance_no_newline();
                return make(token_type::SEMICOLON, c);
            case '.':
                c = '.';
                advance_no_newline();
                return make(token_type::DOT, c);
            case ':':
                if (peek(1) == ':')
                {
                    c = std::string{"::"};
                    advance_no_newline(2);
                    return make(token_type::SCOPE, c);
                }
                if (peek(1) == '=' && peek(2) == '>')
                {
                    c = std::string{":=>"};
                    advance_no_newline(3);
                    return make(token_type::CHANNEL_INSERT, c);
                }
                c = ':';
                advance_no_newline();
                return make(token_type::COLON, c);
            case '+':
                if (peek(1) == '=')
                {
                    c = std::string{"+="};
                    advance_no_newline(2);
                    return make(token_type::ADD_ASSIGN, c);
                }
                c = '+';
                advance_no_newline();
                return make(token_type::PLUS, c);
            case '-':
                if (peek(1) == '>')
                {
                    c = "->";
                    advance_no_newline(2);
                    return make(token_type::PIPE_INSERT, c);
                }
                c = '-';
                advance_no_newline();
                return make(token_type::MINUS, c);
            case '*':
                c = '*';
                advance_no_newline();
                return make(token_type::STAR, c);
            case '/':
                c = '/';
                advance_no_newline();
                return make(token_type::SLASH, c);
            case '%':
                c = '%';
                advance_no_newline();
                return make(token_type::PERCENT, c);
            case '=':
                if (peek(1) == '=')
                {
                    advance_no_newline(2);
                    return make(token_type::EQ, c = std::string{"=="});
                }
                if (peek(1) == '>')
                {
                    advance_no_newline(2);
                    return make(token_type::ARROW, c = std::string{"=>"});
                }
                advance_no_newline();
                return make(token_type::ASSIGN, c = std::string{"="});
            case '!':
                if (peek(1) == '=')
                {
                    advance_no_newline(2);
                    return make(token_type::NE, c = std::string{"!="});
                }
                advance_no_newline();
                return make(token_type::BANG, c = std::string{"!"});
            case '<':
                if (peek(1) == '=' && peek(2) == ':')
                {
                    advance_no_newline(3);
                    return make(token_type::CHANNEL_EXTRACT, c = std::string{"<=:"});
                }
                if (peek(1) == '=')
                {
                    advance_no_newline(2);
                    return make(token_type::LE, c = std::string{"<="});
                }
                if (peek(1) == '-')
                {
                    advance_no_newline(2);
                    return make(token_type::PIPE_EXTRACT, c = std::string{"<-"});
                }
                advance_no_newline();
                return make(token_type::LT, c = std::string{"<"});
            case '>':
                if (peek(1) == '=')
                {
                    advance_no_newline(2);
                    return make(token_type::GE, c = std::string{">="});
                }
                advance_no_newline();
                return make(token_type::GT, c = std::string{">"});
            case '&':
                if (peek(1) == '&')
                {
                    advance_no_newline(2);
                    return make(token_type::AND_AND, c = std::string{"&&"});
                }
                advance_no_newline();
                return make(token_type::AMP, c = std::string{"&"});
            case '|':
                if (peek(1) == '|')
                {
                    advance_no_newline(2);
                    return make(token_type::OR_OR, c = std::string{"||"});
                }
                advance_no_newline();
                return make(token_type::PIPE, c = std::string{"|"});
            case '^':
                advance_no_newline();
                return make(token_type::CARET, c = std::string{"^"});
            case '~':
                advance_no_newline();
                return make(token_type::TILDE, c = std::string{"~"});
            default:
                fail("unexpected character");
                return make(token_type::END, c = std::string{});
            }
        }

        /// Lex an identifier, keyword, or `true`/`false`/`null` literal.
        token lex_ident()
        {
            const std::size_t start = pos_;
            while (!eof() && is_ident_cont(peek()))
            {
                advance_no_newline();
            }
            // Single allocation for the whole identifier.
            std::string text = source_.substr(start, pos_ - start);

            if (text == "true" || text == "false")
            {
                return make(token_type::BOOL_LITERAL, std::move(text));
            }
            if (text == "null")
            {
                return make(token_type::NULL_LITERAL, std::move(text));
            }
            if (keywords_.contains(text))
            {
                return make(token_type::KEYWORD, std::move(text));
            }
            return make(token_type::SYMBOL, std::move(text));
        }

        /// Lex an integer or float literal (float requires digits on both sides of `.`).
        token lex_number()
        {
            const std::size_t start = static_cast<std::size_t>(pos_);
            bool is_float = false;
            while (!eof())
            {
                const char c = peek();
                if (std::isdigit(static_cast<unsigned char>(c)))
                {
                    advance_no_newline();
                    continue;
                }
                if (c == '.' && !is_float &&
                    std::isdigit(static_cast<unsigned char>(peek(1))))
                {
                    is_float = true;
                    advance_no_newline();
                    continue;
                }
                break;
            }
            const char *first = source_.data() + start;
            const char *last = source_.data() + static_cast<std::size_t>(pos_);
            if (is_float)
            {
                return make(token_type::FLOAT_LITERAL,
                            static_cast<long double>(std::stold(std::string{first, last})));
            }
            long long value = 0;
            const auto [ptr, ec] = std::from_chars(first, last, value);
            if (ec != std::errc{} || ptr != last)
            {
                fail("integer literal out of range");
            }
            return make(token_type::INT_LITERAL, value);
        }

        /// Consume a `\` escape and return the decoded character.
        char lex_escape()
        {
            advance_no_newline(); // '\'
            if (eof())
            {
                fail("unterminated escape sequence");
            }
            switch (peek())
            {
            case 'n':
                advance_no_newline();
                return '\n';
            case 'r':
                advance_no_newline();
                return '\r';
            case 't':
                advance_no_newline();
                return '\t';
            case 'a':
                advance_no_newline();
                return '\a';
            case '\\':
                advance_no_newline();
                return '\\';
            case '"':
                advance_no_newline();
                return '"';
            case '\'':
                advance_no_newline();
                return '\'';
            default:
                fail("unrecognized escape character");
                return '\0';
            }
        }

        /// Lex a double-quoted string literal with escapes.
        token lex_string()
        {
            advance_no_newline(); // "
            std::string value;
            while (!eof() && peek() != '"')
            {
                if (peek() == '\\')
                {
                    value.push_back(lex_escape());
                    continue;
                }
                // Copy the run up to the next escape or closing quote in one
                // append instead of char by char.
                const std::size_t chunk_start = static_cast<std::size_t>(pos_);
                while (!eof() && peek() != '"' && peek() != '\\')
                {
                    advance();
                }
                value.append(source_, chunk_start,
                             static_cast<std::size_t>(pos_) - chunk_start);
            }
            if (eof())
            {
                fail("unterminated string literal");
            }
            advance_no_newline(); // closing "
            return make(token_type::STRING_LITERAL, std::move(value));
        }

        /// Lex a raw regex literal `r"..."`.
        token lex_regex()
        {
            advance_no_newline(2); // r"
            const std::size_t body_start = static_cast<std::size_t>(pos_);
            const std::size_t close = source_.find('"', body_start);
            if (close == std::string::npos)
            {
                advance(static_cast<long long>(source_.size()) - pos_);
                fail("unterminated regex literal");
            }
            std::string value = source_.substr(body_start, close - body_start);
            advance(static_cast<long long>(close) - pos_); // body (may span lines)
            advance_no_newline();                          // closing "
            return make(token_type::REGEX_LITERAL, std::move(value));
        }

        /// Lex a single-quoted character literal.
        token lex_char()
        {
            advance_no_newline(); // '
            if (eof())
            {
                fail("unterminated character literal");
            }
            char value = '\0';
            if (peek() == '\\')
            {
                value = lex_escape();
            }
            else
            {
                value = peek();
                advance();
            }
            if (eof() || peek() != '\'')
            {
                fail("character literal must contain exactly one character");
            }
            advance_no_newline(); // '
            return make(token_type::CHAR_LITERAL, std::string(1, value));
        }
    };

} // namespace munx
