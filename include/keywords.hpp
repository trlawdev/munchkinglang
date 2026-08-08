#pragma once

#include <string>
#include <unordered_set>

namespace munx
{

    /// Structural / declaration keywords observed in `sample/**/*.mx`.
    /// Builtins used as callables (`print`, `open`, `thread`, …) stay SYMBOLS so
    /// they can also appear as assignment targets (e.g. `print = fix(...)`).
    /// @return Reference to the process-lifetime keyword set.
    inline const std::unordered_set<std::string> &keywords()
    {
        static const std::unordered_set<std::string> k{
            "package",
            "load_package",
            "load_packages",
            "func",
            "lambda",
            "return",
            "if",
            "else",
            "loop",
            "break",
            "match",
            "case",
            "enum",
            "object",
            "tuple",
            "alloc",
            "delete",
            "free",
            "lock",
            "acquire",
            "release",
            "join",
            "monitor",
            "trap",
            "cast",
            "simd",
            "fail",
            "map",
            "get",
            "insert",
            "env",
            "channel",
            "reflexpr",
            "members",
            "meta_params",
            "params",
            "reflect_for",
            "construct",
            "typeid",
            "default",
            "of",
        };
        return k;
    }

    /// @param name Candidate type identifier.
    /// @return True if @p name is a built-in primitive type name
    ///         (`int`, `file`, `socket`, …).
    inline bool is_type_name(const std::string &name)
    {
        static const std::unordered_set<std::string> types{
            "int", "float", "bool", "string", "character", "void",
            "socket", "file", "term", "exception",
        };
        return types.contains(name);
    }

} // namespace munx
