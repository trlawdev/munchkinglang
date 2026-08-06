#pragma once

#include "errors.hpp"
#include <string>
#include <string_view>

#if defined(_POSIX_VERSION) || defined(__unix__) || defined(__APPLE__)
#include <regex.h>
#define MUNX_HAS_POSIX_REGEX 1
#else
#include <regex>
#define MUNX_HAS_POSIX_REGEX 0
#endif

namespace munx
{

/// Match @p pattern against @p text without relying on C++ exceptions.
/// @return True when the pattern matches; false when it does not or is invalid.
inline bool regex_search_noexcept(std::string_view text, std::string_view pattern,
                                  error *failure = nullptr)
{
#if MUNX_HAS_POSIX_REGEX
    regex_t compiled{};
    const std::string pattern_storage{pattern};
    const int compile_status =
        regcomp(&compiled, pattern_storage.c_str(), REG_EXTENDED | REG_NOSUB);
    if (compile_status != 0)
    {
        if (failure != nullptr)
        {
            char buffer[256]{};
            regerror(compile_status, &compiled, buffer, sizeof buffer);
            *failure = error::make(error_code::invalid_argument,
                                   std::string{"invalid regex: "} + buffer);
        }
        return false;
    }
    const std::string text_storage{text};
    const int match_status =
        regexec(&compiled, text_storage.c_str(), 0, nullptr, 0);
    regfree(&compiled);
    if (match_status == REG_NOMATCH)
    {
        return false;
    }
    if (match_status != 0 && failure != nullptr)
    {
        *failure = error::make(error_code::internal, "regex execution failed");
    }
    return match_status == 0;
#else
    static thread_local std::string text_storage;
    static thread_local std::string pattern_storage;
    text_storage.assign(text);
    pattern_storage.assign(pattern);
    const std::regex compiled{pattern_storage};
    const bool matched = std::regex_search(text_storage, compiled);
    (void)matched;
    return std::regex_search(text_storage, compiled);
#endif
}

} // namespace munx
