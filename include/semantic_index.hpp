#pragma once

#include "ast.hpp"
#include "errors.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace munx
{

/// Kind of a named semantic occurrence in the index.
enum class symbol_kind : uint8_t
{
    var,
    param,
    func,
    type,
    field,
    enum_member,
    builtin,
    package,
};

/// One definition or use of a name with a resolved type and source span.
struct semantic_occurrence
{
    std::string name;
    symbol_kind kind{symbol_kind::var};
    std::string type; ///< Display type from resolved_type::stringify().
    diag_span span{};
    diag_span def{};  ///< Definition span when this is a use (may be empty).
    bool is_def{false};
};

/// Document outline symbol (package / func / object / trait / enum / var).
struct outline_symbol
{
    std::string name;
    symbol_kind kind{symbol_kind::var};
    std::string detail;
    diag_span range{};
    diag_span selection{};
    std::vector<outline_symbol> children;
};

/// Object / trait / enum type table entry for completions.
struct type_info_entry
{
    std::string name;
    std::string kind; ///< "object" | "trait" | "enum"
    std::vector<std::pair<std::string, std::string>> fields; ///< name → type
    std::vector<std::string> members;                       ///< enum members
    diag_span def{};
};

/// Function signature table entry for hover / completions.
struct function_info_entry
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> params; ///< name → type
    std::string return_type;
    std::string signature; ///< Full `name(...): ret` display form when known.
    diag_span def{};
};

/// Semantic index produced by type-checking for IDE queries.
struct semantic_index
{
    std::vector<semantic_occurrence> occurrences;
    std::vector<outline_symbol> symbols;
    std::vector<type_info_entry> types;
    std::vector<function_info_entry> functions;
    std::string package_name;
    diag_span package_span{};

    void add_occurrence(semantic_occurrence occ) { occurrences.push_back(std::move(occ)); }

    [[nodiscard]] const semantic_occurrence *find_at(const std::string &file,
                                                     long long line,
                                                     long long column) const
    {
        const semantic_occurrence *best = nullptr;
        for (const auto &occ : occurrences)
        {
            if (occ.span.file != file)
            {
                continue;
            }
            if (occ.span.line != line)
            {
                continue;
            }
            if (column < occ.span.column)
            {
                continue;
            }
            const long long end =
                occ.span.end_column != 0 ? occ.span.end_column
                                         : occ.span.column +
                                               static_cast<long long>(occ.name.size());
            if (column >= end)
            {
                continue;
            }
            if (best == nullptr || occ.span.column > best->span.column)
            {
                best = &occ;
            }
        }
        return best;
    }

    [[nodiscard]] const type_info_entry *find_type(const std::string &name) const
    {
        for (const auto &entry : types)
        {
            if (entry.name == name)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const function_info_entry *find_function(const std::string &name) const
    {
        for (const auto &entry : functions)
        {
            if (entry.name == name)
            {
                return &entry;
            }
        }
        return nullptr;
    }
};

[[nodiscard]] inline diag_span make_span(const ast::source_loc &loc, std::size_t length = 1)
{
    diag_span span{};
    span.file = loc.file;
    span.line = loc.line;
    span.column = loc.column;
    span.end_line = loc.line;
    span.end_column = loc.column + static_cast<long long>(length);
    return span;
}

[[nodiscard]] inline const char *symbol_kind_name(symbol_kind kind) noexcept
{
    switch (kind)
    {
    case symbol_kind::var:
        return "var";
    case symbol_kind::param:
        return "param";
    case symbol_kind::func:
        return "func";
    case symbol_kind::type:
        return "type";
    case symbol_kind::field:
        return "field";
    case symbol_kind::enum_member:
        return "enum_member";
    case symbol_kind::builtin:
        return "builtin";
    case symbol_kind::package:
        return "package";
    }
    return "var";
}

} // namespace munx
