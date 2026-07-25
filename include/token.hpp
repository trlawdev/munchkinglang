#pragma once

#include <string>
#include <variant>

namespace munx
{

    enum class token_type
    {
        KEYWORD,
        SYMBOL,

        OPEN_SQUARE_BRACE,
        CLOSE_SQUARE_BRACE,
        COLON,
        OPEN_CURLY_BRACE,
        CLOSE_CURLY_BRACE,
        ASSIGN,
        EQUAL,
        NOT_EQUAL,
        AND,
        OR,
        NOT,
        BITWISE_OR,
        BITWISE_AND,
        BITWISE_NOT,
        BITWISE_XOR,

        CHAR_LITERAL,
        INT_LITERAL,
        STRING_LITERAL,
        FLOAT_LITERAL,
        BOOLEAN_LITERAL,

        ENUM_SCOPE_RESOLUTION_OPERATOR, //::
        OPEN_BRACE,
        CLOSE_BRACE,
        LINE_TERMINATOR,
        COMMA,
        FIELD_ACCESS_OPERATOR,
        MODULO_OPERATOR,

        ADD,
        MUL,
        DIV,
        SUB,

        IF,
        IF_ELSE,
        ELSE,
        LOOP,
    };

    struct token
    {
        token_type type;
        std::variant<long long, long double, std::string> value;
    };

}
