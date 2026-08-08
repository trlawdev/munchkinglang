#pragma once

#include <string>
#include <variant>

namespace munx
{

    /// Lexical token kinds produced by the munx lexer.
    enum class token_type
    {
        // identifiers / reserved
        SYMBOL,  ///< User or builtin identifier (not a structural keyword).
        KEYWORD, ///< Structural keyword from munx::keywords().

        // literals
        INT_LITERAL,    ///< Signed 64-bit integer literal.
        FLOAT_LITERAL,  ///< Floating-point literal (digits on both sides of `.`).
        STRING_LITERAL, ///< Double-quoted string with escapes.
        CHAR_LITERAL,   ///< Single-quoted character with escapes.
        BOOL_LITERAL,   ///< `true` or `false`.
        NULL_LITERAL,   ///< `null`.
        REGEX_LITERAL,  ///< Raw regex `r"..."`.

        // punctuation
        LPAREN,    ///< `(`
        RPAREN,    ///< `)`
        LBRACE,    ///< `{`
        RBRACE,    ///< `}`
        LBRACKET,  ///< `[`
        RBRACKET,  ///< `]`
        COMMA,     ///< `,`
        COLON,     ///< `:`
        SEMICOLON, ///< `;`
        DOT,       ///< `.`
        SCOPE,     ///< `::`

        // operators
        ASSIGN,       ///< `=`
        ADD_ASSIGN,   ///< `+=`
        EQ,           ///< `==`
        NE,           ///< `!=`
        LT,           ///< `<`
        GT,           ///< `>`
        LE,           ///< `<=`
        GE,           ///< `>=`
        PLUS,         ///< `+`
        MINUS,        ///< `-`
        STAR,         ///< `*`
        SLASH,        ///< `/`
        PERCENT,      ///< `%`
        BANG,         ///< `!`
        AMP,          ///< `&` (lexed; no binary parse rule yet)
        PIPE,         ///< `|` (lexed; no binary parse rule yet)
        CARET,        ///< `^` (lexed; no binary parse rule yet)
        TILDE,        ///< `~`
        AND_AND,      ///< `&&`
        OR_OR,        ///< `||`
        ARROW,        ///< `=>` (lambda body)
        PIPE_INSERT,     ///< `->` (pipe write)
        PIPE_EXTRACT,    ///< `<-` (pipe read)
        CHANNEL_INSERT,  ///< `:=>` (channel write)
        CHANNEL_EXTRACT, ///< `<=:` (channel read)

        END, ///< Sentinel end-of-input token.
    };

    using value_variant = std::variant<char, long long, long double, std::string>;

    /// A single lexeme with 1-based source coordinates and a typed payload.
    struct token
    {
        long long line;   ///< 1-based line of the token start.
        long long column; ///< 1-based column of the token start.
        token_type type;  ///< Token kind.
        /// Payload: integer, float, or string (lexeme / decoded literal text).
        value_variant value;
    };

} // namespace munx
