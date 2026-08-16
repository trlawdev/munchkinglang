#pragma once

#include "analyze/host.hpp"
#include "analyze/json_util.hpp"
#include "analyze/snapshot.hpp"
#include "errors.hpp"
#include "semantic_index.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace munx::analyze
{

inline void append_span_json(std::string &out, const diag_span &span)
{
    out += "{\"file\":";
    json::append_escaped(out, span.file);
    out += ",\"start\":{\"line\":";
    out += std::to_string(span.line);
    out += ",\"column\":";
    out += std::to_string(span.column);
    out += "},\"end\":{\"line\":";
    out += std::to_string(span.end_line != 0 ? span.end_line : span.line);
    out += ",\"column\":";
    out += std::to_string(span.end_column != 0 ? span.end_column
                                              : (span.column != 0 ? span.column + 1 : 0));
    out += "}}";
}

inline void append_occurrence_json(std::string &out, const semantic_occurrence &occ)
{
    out += "{\"name\":";
    json::append_escaped(out, occ.name);
    out += ",\"kind\":";
    json::append_escaped(out, symbol_kind_name(occ.kind));
    out += ",\"type\":";
    json::append_escaped(out, occ.type);
    out += ",\"is_def\":";
    out += occ.is_def ? "true" : "false";
    out += ",\"span\":";
    append_span_json(out, occ.span);
    if (occ.def.line != 0)
    {
        out += ",\"def\":";
        append_span_json(out, occ.def);
    }
    out += '}';
}

inline void append_outline_json(std::string &out, const outline_symbol &sym)
{
    out += "{\"name\":";
    json::append_escaped(out, sym.name);
    out += ",\"kind\":";
    json::append_escaped(out, symbol_kind_name(sym.kind));
    out += ",\"detail\":";
    json::append_escaped(out, sym.detail);
    out += ",\"range\":";
    append_span_json(out, sym.range);
    out += ",\"selection\":";
    append_span_json(out, sym.selection);
    out += ",\"children\":[";
    for (std::size_t i = 0; i < sym.children.size(); ++i)
    {
        if (i != 0)
        {
            out += ',';
        }
        append_outline_json(out, sym.children[i]);
    }
    out += "]}";
}

[[nodiscard]] inline std::string snapshot_to_analyze_json(const analysis_snapshot &snap)
{
    std::string out;
    out.reserve(4096);
    out += "{\"schema\":\"munx.analyze.v1\",\"file\":";
    json::append_escaped(out, snap.file);
    out += ",\"package\":";
    json::append_escaped(out, snap.package);
    out += ",\"ok\":";
    out += snap.ok ? "true" : "false";
    out += ",\"revision\":";
    out += std::to_string(snap.revision);
    out += ",\"diagnostics\":[";
    for (std::size_t i = 0; i < snap.diagnostics.size(); ++i)
    {
        if (i != 0)
        {
            out += ',';
        }
        const auto &d = snap.diagnostics[i];
        out += "{\"severity\":";
        json::append_escaped(
            out, d.severity == diagnostic_severity::warning ? "warning" : "error");
        out += ",\"message\":";
        json::append_escaped(out, d.message);
        out += ",\"span\":";
        append_span_json(out, d.span);
        out += '}';
    }
    out += "],\"symbols\":[";
    for (std::size_t i = 0; i < snap.index.symbols.size(); ++i)
    {
        if (i != 0)
        {
            out += ',';
        }
        append_outline_json(out, snap.index.symbols[i]);
    }
    out += "],\"idents\":[";
    for (std::size_t i = 0; i < snap.index.occurrences.size(); ++i)
    {
        if (i != 0)
        {
            out += ',';
        }
        append_occurrence_json(out, snap.index.occurrences[i]);
    }
    out += "],\"types\":[";
    for (std::size_t i = 0; i < snap.index.types.size(); ++i)
    {
        if (i != 0)
        {
            out += ',';
        }
        const auto &t = snap.index.types[i];
        out += "{\"name\":";
        json::append_escaped(out, t.name);
        out += ",\"kind\":";
        json::append_escaped(out, t.kind);
        out += ",\"fields\":[";
        for (std::size_t j = 0; j < t.fields.size(); ++j)
        {
            if (j != 0)
            {
                out += ',';
            }
            out += "{\"name\":";
            json::append_escaped(out, t.fields[j].first);
            out += ",\"type\":";
            json::append_escaped(out, t.fields[j].second);
            out += '}';
        }
        out += "],\"members\":[";
        for (std::size_t j = 0; j < t.members.size(); ++j)
        {
            if (j != 0)
            {
                out += ',';
            }
            json::append_escaped(out, t.members[j]);
        }
        out += "]}";
    }
    out += "],\"functions\":[";
    for (std::size_t i = 0; i < snap.index.functions.size(); ++i)
    {
        if (i != 0)
        {
            out += ',';
        }
        const auto &f = snap.index.functions[i];
        out += "{\"name\":";
        json::append_escaped(out, f.name);
        out += ",\"return_type\":";
        json::append_escaped(out, f.return_type);
        if (!f.signature.empty())
        {
            out += ",\"signature\":";
            json::append_escaped(out, f.signature);
        }
        out += ",\"params\":[";
        for (std::size_t j = 0; j < f.params.size(); ++j)
        {
            if (j != 0)
            {
                out += ',';
            }
            out += "{\"name\":";
            json::append_escaped(out, f.params[j].first);
            out += ",\"type\":";
            json::append_escaped(out, f.params[j].second);
            out += '}';
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

[[nodiscard]] inline std::string hover_response(const analysis_snapshot &snap,
                                                long long line, long long column)
{
    if (snap.is_cancelled())
    {
        return {};
    }
    const semantic_occurrence *occ = snap.index.find_at(snap.file, line, column);
    // Also try matching by basename if absolute path differs.
    if (occ == nullptr)
    {
        for (const auto &candidate : snap.index.occurrences)
        {
            if (candidate.span.line != line)
            {
                continue;
            }
            if (column < candidate.span.column)
            {
                continue;
            }
            const long long end =
                candidate.span.end_column != 0
                    ? candidate.span.end_column
                    : candidate.span.column +
                          static_cast<long long>(candidate.name.size());
            if (column < end)
            {
                occ = &candidate;
            }
        }
    }
    std::string out = "{\"contents\":";
    if (occ == nullptr)
    {
        out += "null}";
        return out;
    }

    std::string display = occ->type;
    if (occ->kind == symbol_kind::func || occ->kind == symbol_kind::builtin)
    {
        // Prefer the occurrence's stored signature (includes builtin display forms).
        if (display.find('(') == std::string::npos)
        {
            if (const function_info_entry *fn = snap.index.find_function(occ->name))
            {
                if (!fn->signature.empty())
                {
                    display = fn->signature;
                }
                else
                {
                    display = fn->name;
                    display += '(';
                    bool first = true;
                    for (const auto &param : fn->params)
                    {
                        if (param.first == "..." && param.second.empty())
                        {
                            if (!first)
                            {
                                display += ", ";
                            }
                            display += "...";
                            first = false;
                            continue;
                        }
                        if (!first)
                        {
                            display += ", ";
                        }
                        first = false;
                        if (!param.first.empty())
                        {
                            display += param.first;
                            display += ": ";
                        }
                        display += param.second;
                    }
                    display += "): ";
                    display += fn->return_type;
                }
            }
        }
    }

    std::string md;
    if (occ->kind == symbol_kind::func || occ->kind == symbol_kind::builtin)
    {
        md = "```munx\nfunc " + display + "\n```\n\n";
        md += occ->kind == symbol_kind::builtin ? "builtin" : "function";
    }
    else
    {
        md = "**" + occ->name + "**";
        if (!display.empty() && display != "any" && display != "<unknown>")
        {
            md += ": `" + display + "`";
        }
        md += "\n\n";
        md += symbol_kind_name(occ->kind);
    }
    out += json::quote(md);
    out += ",\"range\":";
    append_span_json(out, occ->span);
    out += '}';
    return out;
}

[[nodiscard]] inline std::string definition_response(const analysis_snapshot &snap,
                                                     long long line, long long column)
{
    if (snap.is_cancelled())
    {
        return {};
    }
    const semantic_occurrence *occ = nullptr;
    for (const auto &candidate : snap.index.occurrences)
    {
        if (candidate.span.line != line)
        {
            continue;
        }
        if (column < candidate.span.column)
        {
            continue;
        }
        const long long end =
            candidate.span.end_column != 0
                ? candidate.span.end_column
                : candidate.span.column + static_cast<long long>(candidate.name.size());
        if (column < end)
        {
            occ = &candidate;
        }
    }
    if (occ == nullptr || occ->def.line == 0)
    {
        return "{\"location\":null}";
    }
    std::string out = "{\"location\":";
    append_span_json(out, occ->def);
    out += '}';
    return out;
}

[[nodiscard]] inline std::string symbols_response(const analysis_snapshot &snap)
{
    std::string out = "{\"symbols\":[";
    for (std::size_t i = 0; i < snap.index.symbols.size(); ++i)
    {
        if (i != 0)
        {
            out += ',';
        }
        append_outline_json(out, snap.index.symbols[i]);
    }
    out += "]}";
    return out;
}

[[nodiscard]] inline std::string completion_response(const analysis_snapshot &snap,
                                                     long long /*line*/,
                                                     long long /*column*/,
                                                     std::string_view trigger)
{
    std::string out = "{\"items\":[";
    bool first = true;
    auto push = [&](const std::string &label, const std::string &detail,
                    const std::string &kind) {
        if (!first)
        {
            out += ',';
        }
        first = false;
        out += "{\"label\":";
        json::append_escaped(out, label);
        out += ",\"detail\":";
        json::append_escaped(out, detail);
        out += ",\"kind\":";
        json::append_escaped(out, kind);
        out += '}';
    };

    if (trigger == ".")
    {
        for (const auto &t : snap.index.types)
        {
            for (const auto &field : t.fields)
            {
                push(field.first, field.second, "field");
            }
        }
        push("len", "int", "field");
    }
    else if (trigger == "::")
    {
        for (const auto &t : snap.index.types)
        {
            if (t.kind != "enum")
            {
                continue;
            }
            for (const auto &member : t.members)
            {
                push(member, t.name, "enum_member");
            }
        }
        for (const char *dir : {"reflexpr", "members", "function_members", "params",
                                "meta_params", "reflect_for", "construct", "match",
                                "typeid"})
        {
            push(dir, "compiler", "keyword");
        }
    }
    else
    {
        for (const auto &t : snap.index.types)
        {
            push(t.name, t.kind, "type");
        }
        for (const auto &f : snap.index.functions)
        {
            push(f.name,
                 !f.signature.empty() ? f.signature : f.return_type,
                 "func");
        }
        for (const auto &occ : snap.index.occurrences)
        {
            if (occ.is_def &&
                (occ.kind == symbol_kind::var || occ.kind == symbol_kind::param))
            {
                push(occ.name, occ.type, symbol_kind_name(occ.kind));
            }
        }
    }
    out += "]}";
    return out;
}

[[nodiscard]] inline std::string diagnostics_notification(const analysis_snapshot &snap)
{
    std::string out = "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
                      "\"params\":{\"path\":";
    json::append_escaped(out, snap.file);
    out += ",\"diagnostics\":[";
    for (std::size_t i = 0; i < snap.diagnostics.size(); ++i)
    {
        if (i != 0)
        {
            out += ',';
        }
        const auto &d = snap.diagnostics[i];
        out += "{\"severity\":";
        json::append_escaped(
            out, d.severity == diagnostic_severity::warning ? "warning" : "error");
        out += ",\"message\":";
        json::append_escaped(out, d.message);
        out += ",\"span\":";
        append_span_json(out, d.span);
        out += '}';
    }
    out += "]}}";
    return out;
}

/// Run one-shot `--analyze` for @p path (optional stdin overlay via @p source).
inline int run_analyze_batch(const std::filesystem::path &path,
                             const std::optional<std::string> &source_overlay,
                             std::ostream &out)
{
    std::string source;
    if (source_overlay)
    {
        source = *source_overlay;
    }
    else
    {
        analysis_host tmp;
        const auto text = tmp.vfs().read(path);
        if (!text)
        {
            std::cerr << "could not open source file: " << path.string() << '\n';
            return 1;
        }
        source = *text;
    }
    const auto snap =
        analysis_host::analyze_buffer(path, source, path.parent_path());
    out << snapshot_to_analyze_json(*snap) << '\n';
    return 0;
}

/// Persistent JSON-RPC analyze server on stdio (newline-delimited).
inline int run_analyze_server(std::istream &in, std::ostream &out)
{
    analysis_host host;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
        {
            continue;
        }
        json::object_view root{line};
        const std::string id = root.string_field("id");
        const std::string method = root.string_field("method");

        auto reply_result = [&](const std::string &result_json) {
            out << "{\"jsonrpc\":\"2.0\",\"id\":";
            if (id.empty())
            {
                out << "null";
            }
            else if (id.find_first_not_of("0123456789-") == std::string::npos)
            {
                out << id;
            }
            else
            {
                out << json::quote(id);
            }
            out << ",\"result\":" << result_json << "}\n";
            out.flush();
        };
        auto reply_error = [&](int code, const std::string &message) {
            out << "{\"jsonrpc\":\"2.0\",\"id\":";
            if (id.empty())
            {
                out << "null";
            }
            else if (id.find_first_not_of("0123456789-") == std::string::npos)
            {
                out << id;
            }
            else
            {
                out << json::quote(id);
            }
            out << ",\"error\":{\"code\":" << code << ",\"message\":"
                << json::quote(message) << "}}\n";
            out.flush();
        };

        if (method == "workspace/setRoot")
        {
            const std::string path = root.string_field("path");
            host.set_workspace_root(path);
            reply_result("{\"ok\":true}");
            continue;
        }
        if (method == "vfs/update")
        {
            const std::string path = root.string_field("path");
            const std::string kind = root.string_field("kind", "change");
            std::optional<std::string> text;
            if (root.has("text"))
            {
                text = root.string_field("text");
            }
            host.vfs_update(path, kind, text);
            if (kind == "open" || kind == "change")
            {
                auto snap = host.rebuild(path);
                out << diagnostics_notification(*snap) << '\n';
                out.flush();
                reply_result("{\"ok\":true,\"revision\":" +
                             std::to_string(snap->revision) +
                             ",\"preamble_hits\":" +
                             std::to_string(host.preamble_hits()) +
                             ",\"preamble_misses\":" +
                             std::to_string(host.preamble_misses()) + "}");
            }
            else
            {
                reply_result("{\"ok\":true}");
            }
            continue;
        }
        if (method == "textDocument/hover" || method == "textDocument/completion" ||
            method == "textDocument/definition" ||
            method == "textDocument/documentSymbol")
        {
            const std::string path = root.string_field("path");
            auto snap = host.current_snapshot();
            if (!snap || snap->file.find(std::filesystem::path{path}.filename().string()) ==
                             std::string::npos)
            {
                // Ensure we have a snapshot for this path.
                if (host.vfs().has_overlay(path) ||
                    std::filesystem::exists(path))
                {
                    snap = host.rebuild(path);
                }
            }
            if (!snap)
            {
                reply_error(-32000, "no snapshot");
                continue;
            }
            if (snap->is_cancelled())
            {
                reply_error(-32800, "Cancelled");
                continue;
            }
            const long long line = root.int_field("line");
            const long long column = root.int_field("column");
            if (method == "textDocument/hover")
            {
                const std::string result = hover_response(*snap, line, column);
                if (result.empty())
                {
                    reply_error(-32800, "Cancelled");
                }
                else
                {
                    reply_result(result);
                }
            }
            else if (method == "textDocument/definition")
            {
                reply_result(definition_response(*snap, line, column));
            }
            else if (method == "textDocument/documentSymbol")
            {
                reply_result(symbols_response(*snap));
            }
            else
            {
                const std::string trigger = root.string_field("trigger");
                reply_result(completion_response(*snap, line, column, trigger));
            }
            continue;
        }
        if (method == "shutdown")
        {
            reply_result("null");
            break;
        }
        reply_error(-32601, "Method not found: " + method);
    }
    return 0;
}

} // namespace munx::analyze
