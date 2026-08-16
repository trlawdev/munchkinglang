#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace munx::analyze
{

/// Overlay virtual file system: editor buffers win over disk (rust-analyzer VFS).
class overlay_vfs
{
    std::unordered_map<std::string, std::string> overlays_;

    [[nodiscard]] static std::string normalize(const std::filesystem::path &path)
    {
        std::error_code ec;
        const auto abs = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal().string();
        }
        return abs.lexically_normal().string();
    }

public:
    void open(const std::filesystem::path &path, std::string text)
    {
        overlays_[normalize(path)] = std::move(text);
    }

    void change(const std::filesystem::path &path, std::string text)
    {
        overlays_[normalize(path)] = std::move(text);
    }

    void close(const std::filesystem::path &path) { overlays_.erase(normalize(path)); }

    [[nodiscard]] bool has_overlay(const std::filesystem::path &path) const
    {
        return overlays_.contains(normalize(path));
    }

    [[nodiscard]] std::optional<std::string> read(const std::filesystem::path &path) const
    {
        const std::string key = normalize(path);
        const auto found = overlays_.find(key);
        if (found != overlays_.end())
        {
            return found->second;
        }
        std::ifstream input{path};
        if (!input)
        {
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    [[nodiscard]] std::size_t content_hash(const std::filesystem::path &path) const
    {
        const auto text = read(path);
        if (!text)
        {
            return 0;
        }
        return std::hash<std::string>{}(*text);
    }
};

} // namespace munx::analyze
