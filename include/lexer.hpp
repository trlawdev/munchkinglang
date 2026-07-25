#pragma once

#include "token.hpp"
#include "errors.hpp"
#include <iostream>
#include <unordered_set>
#include <vector>

namespace munx
{

    class lexer
    {
        long long position_;
        const std::string &content_;
        const std::unordered_set<std::string> &keywords_;
        std::ostream& output_stream_handle_ = std::cout;

    public:
        lexer(const std::string &content,
              const std::unordered_set<std::string> &keywords) noexcept
            : position_(0), content_(content), keywords_(keywords) {}

        lexer() = delete ("lexer obejct is not default constructible");
        lexer(lexer &&) = delete ("lexer object cannot be constructible");
        lexer(const lexer &) = delete ("lexer object is not copy constructible");
        lexer &operator=(const lexer &) = delete ("lexer object is copy assignable");
        lexer &operator=(lexer &&) = delete ("lexer object is not move assignable");

        std::vector<token> read_tokens()
        {
            return {};
        }

    private:
        char peek(long long offset = 0) const {
            if (is_eof(offset)) {
                
            }
        }
        
        inline void advance(long offset = 1) noexcept
        {
            position_ += offset;
        }

        bool is_eof(long long offset = 0) const noexcept
        {
            return (position_ + offset) >= content_.length();
        }

        bool terminate(const std::string &err_msg) const
        {
            throw compilation_error{err_msg.c_str()};
        }

        const char* handle_escape_sequences(const char *fmt) const
        {
            switch (*(fmt + 1))
            {
            case '\\':
                output_stream_handle_ << '\\';
                fmt += 2;
                break;
            case 'a':
                output_stream_handle_ << '\a';
                fmt += 2;
                break;
            case 'n':
                output_stream_handle_ << '\n';
                fmt += 2;
                break;
            case 't':
                output_stream_handle_ << '\t';
                fmt += 2;
                break;
            default:
                throw compilation_error{"unrecognized escape character"};
            }

            return fmt;
        }

        std::string parse_string_literal()
        {
            advance(); // skip starting "
            while (!is_eof()) {
                if 
            }
        }
    };

}
