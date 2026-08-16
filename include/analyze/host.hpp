#pragma once

#include "analyze/json_util.hpp"
#include "analyze/snapshot.hpp"
#include "analyze/vfs.hpp"
#include "bytecode_compiler.hpp"
#include "errors.hpp"
#include "generic_reflexpr.hpp"
#include "keywords.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "semantic_index.hpp"
#include "type_checker.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace munx::analyze
{

/// Cached parsed import package (clangd preamble analog).
struct preamble_entry
{
    std::filesystem::path path;
    std::size_t content_hash{0};
    std::shared_ptr<ast::program> program;
};

/// Analysis host: VFS overlays + revisioned snapshots (rust-analyzer AnalysisHost).
class analysis_host
{
    overlay_vfs vfs_;
    std::filesystem::path workspace_root_;
    std::uint64_t revision_{0};
    snapshot_ptr current_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, preamble_entry> preamble_cache_;
    std::uint64_t preamble_hits_{0};
    std::uint64_t preamble_misses_{0};

    [[nodiscard]] static std::string path_key(const std::filesystem::path &path)
    {
        std::error_code ec;
        const auto abs = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal().string();
        }
        return abs.lexically_normal().string();
    }

    [[nodiscard]] std::optional<ast::program>
    parse_source(const std::string &source, const std::filesystem::path &path,
                 bool recover) const
    {
        lexer lex{source, keywords(), path};
        std::vector<token> tokens = lex.read_tokens();
        parser parse{std::move(tokens), path, recover};
        ast::program program = parse.parse_program();
        expand_generics_and_reflexpr(program);
        return program;
    }

    /// Resolve imports using VFS overlays; reuse preamble cache when unchanged.
    void resolve_imports_vfs(const ast::program &main,
                             std::vector<std::shared_ptr<ast::program>> &imports)
    {
        const std::filesystem::path dir =
            workspace_root_.empty() ? std::filesystem::current_path()
                                    : workspace_root_;

        std::unordered_set<std::filesystem::path> dir_package_set{};
        std::error_code ec;
        for (auto &dir_entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (dir_entry.path().extension() == ".mx")
            {
                dir_package_set.emplace(dir_entry.path().filename());
            }
        }

        std::unordered_set<std::string> loaded{};
        loaded.insert(main.package_name);

        std::function<void(const ast::program &)> resolve =
            [&](const ast::program &program) {
                for (const auto &package_stmt : program.imports)
                {
                    if (loaded.contains(package_stmt.package))
                    {
                        continue;
                    }
                    const auto filename =
                        std::filesystem::path{package_stmt.package + ".mx"};
                    if (!dir_package_set.contains(filename))
                    {
                        fail_compile_at(package_stmt.loc.file, package_stmt.loc.line,
                                        package_stmt.loc.column,
                                        "failed to find package `" +
                                            package_stmt.package + "`");
                        continue;
                    }
                    loaded.insert(package_stmt.package);
                    const std::filesystem::path import_path = dir / filename;
                    const std::size_t hash = vfs_.content_hash(import_path);
                    const std::string key = path_key(import_path);
                    auto cached = preamble_cache_.find(key);
                    if (cached != preamble_cache_.end() &&
                        cached->second.content_hash == hash && cached->second.program)
                    {
                        ++preamble_hits_;
                        resolve(*cached->second.program);
                        imports.push_back(cached->second.program);
                        continue;
                    }
                    ++preamble_misses_;
                    const auto text = vfs_.read(import_path);
                    if (!text)
                    {
                        fail_compile_at(package_stmt.loc.file, package_stmt.loc.line,
                                        package_stmt.loc.column,
                                        "failed to resolve package `" +
                                            package_stmt.package + "`");
                        continue;
                    }
                    auto parsed = parse_source(*text, import_path, false);
                    if (!parsed)
                    {
                        fail_compile_at(package_stmt.loc.file, package_stmt.loc.line,
                                        package_stmt.loc.column,
                                        "failed to resolve package `" +
                                            package_stmt.package + "`");
                        continue;
                    }
                    auto shared = std::make_shared<ast::program>(std::move(*parsed));
                    resolve(*shared);
                    preamble_entry entry{};
                    entry.path = import_path;
                    entry.content_hash = hash;
                    entry.program = shared;
                    preamble_cache_[key] = entry;
                    imports.push_back(std::move(shared));
                }
            };

        resolve(main);
    }

public:
    void set_workspace_root(std::filesystem::path root)
    {
        std::lock_guard lock{mutex_};
        workspace_root_ = std::move(root);
    }

    [[nodiscard]] const std::filesystem::path &workspace_root() const noexcept
    {
        return workspace_root_;
    }

    overlay_vfs &vfs() noexcept { return vfs_; }

    [[nodiscard]] std::uint64_t preamble_hits() const noexcept { return preamble_hits_; }
    [[nodiscard]] std::uint64_t preamble_misses() const noexcept
    {
        return preamble_misses_;
    }

    void vfs_update(const std::filesystem::path &path, std::string_view kind,
                    std::optional<std::string> text)
    {
        std::lock_guard lock{mutex_};
        if (kind == "open" || kind == "change")
        {
            if (text)
            {
                if (kind == "open")
                {
                    vfs_.open(path, std::move(*text));
                }
                else
                {
                    vfs_.change(path, std::move(*text));
                }
            }
        }
        else if (kind == "close")
        {
            vfs_.close(path);
        }
        if (current_)
        {
            current_->cancel();
        }
        ++revision_;
    }

    /// Rebuild analysis for @p path (overlay or disk) and install a new snapshot.
    snapshot_ptr rebuild(const std::filesystem::path &path)
    {
        std::lock_guard lock{mutex_};
        const std::uint64_t rev = revision_;
        auto snap = std::make_shared<analysis_snapshot>();
        snap->revision = rev;
        snap->file = path_key(path);

        compile_context ctx;
        ctx.collect_all = true;
        compile_context_scope scope{ctx};
        ctx.collect_all = true;

        const auto text = vfs_.read(path);
        if (!text)
        {
            fail_compile("could not open source file: " + path.string());
            snap->ok = false;
            snap->diagnostics = ctx.diagnostics;
            current_ = snap;
            return snap;
        }

        auto program = parse_source(*text, path, true);
        if (!program)
        {
            snap->ok = false;
            snap->diagnostics = ctx.diagnostics;
            current_ = snap;
            return snap;
        }

        snap->package = program->package_name;
        std::vector<std::shared_ptr<ast::program>> imports;
        resolve_imports_vfs(*program, imports);

        semantic_index index;
        (void)type_checker::check_packages_indexed(
            path.parent_path().empty() ? workspace_root_ : path.parent_path(), *program,
            imports, index);

        snap->index = std::move(index);
        snap->diagnostics = ctx.diagnostics;
        snap->ok = !ctx.failed();
        current_ = snap;
        return snap;
    }

    [[nodiscard]] snapshot_ptr current_snapshot() const
    {
        std::lock_guard lock{mutex_};
        return current_;
    }

    /// Analyze a buffer once without mutating host state (batch `--analyze`).
    static snapshot_ptr analyze_buffer(const std::filesystem::path &path,
                                       const std::string &source,
                                       const std::filesystem::path &workspace_root)
    {
        analysis_host host;
        if (!workspace_root.empty())
        {
            host.set_workspace_root(workspace_root);
        }
        else if (!path.empty())
        {
            host.set_workspace_root(path.parent_path());
        }
        host.vfs_update(path, "open", source);
        return host.rebuild(path);
    }
};

} // namespace munx::analyze
