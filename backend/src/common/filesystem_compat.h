// 统一不同 GCC 标准库版本的 filesystem 命名空间差异。
// 边界：保持低依赖、无业务状态，供上层单向复用。

#pragma once

#include <string>
#include <system_error>
#include <vector>

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 8
#include <experimental/filesystem>
namespace edge {
namespace fs = std::experimental::filesystem;

// 计算路径的绝对形式并返回错误码。
inline fs::path fs_absolute(const fs::path& path, std::error_code& ec)
{
    try {
        ec.clear();
        return fs::absolute(path);
    } catch (const fs::filesystem_error& error) {
        ec = error.code();
    } catch (...) {
        ec = std::make_error_code(std::errc::io_error);
    }
    return {};
}

// 按词法规则规范化路径。
inline fs::path fs_lexically_normal(const fs::path& path)
{
    const std::string text = path.string();
    if (text.empty()) {
        return {};
    }

    const bool is_absolute = text.front() == '/';
    std::vector<std::string> parts;

    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && text[index] == '/') {
            ++index;
        }
        const auto begin = index;
        while (index < text.size() && text[index] != '/') {
            ++index;
        }
        if (begin == index) {
            continue;
        }

        const auto part = text.substr(begin, index - begin);
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!is_absolute) {
                parts.push_back(part);
            }
            continue;
        }
        parts.push_back(part);
    }

    if (parts.empty()) {
        return is_absolute ? fs::path("/") : fs::path(".");
    }

    std::string normalized = is_absolute ? "/" : "";
    for (std::size_t part_index = 0; part_index < parts.size(); ++part_index) {
        if (part_index > 0) {
            normalized += "/";
        }
        normalized += parts[part_index];
    }
    return fs::path(normalized);
}

// 解析已存在路径的规范形式。
inline fs::path fs_canonical_existing(const fs::path& path, std::error_code& ec)
{
    try {
        ec.clear();
        return fs::canonical(path);
    } catch (const fs::filesystem_error& error) {
        ec = error.code();
    } catch (...) {
        ec = std::make_error_code(std::errc::io_error);
    }
    return {};
}
}
#else
#include <filesystem>
namespace edge {
namespace fs = std::filesystem;

// 计算路径的绝对形式并返回错误码。
inline fs::path fs_absolute(const fs::path& path, std::error_code& ec)
{
    return fs::absolute(path, ec);
}

// 按词法规则规范化路径。
inline fs::path fs_lexically_normal(const fs::path& path)
{
    return path.lexically_normal();
}

// 解析已存在路径的规范形式。
inline fs::path fs_canonical_existing(const fs::path& path, std::error_code& ec)
{
    return fs::canonical(path, ec);
}
}
#endif

namespace edge {

// canonical 后按路径分隔符边界判断包含关系，不依赖 GCC 7
// experimental filesystem 中缺失的新标准库路径 API。
inline bool fs_path_is_within(
    const fs::path& root,
    const fs::path& candidate,
    std::error_code& ec)
{
    const auto canonical_root = fs_canonical_existing(root, ec);
    if (ec) {
        return false;
    }
    const auto canonical_candidate = fs_canonical_existing(candidate, ec);
    if (ec) {
        return false;
    }

    auto root_text = canonical_root.generic_string();
    const auto candidate_text = canonical_candidate.generic_string();
    while (root_text.size() > 1 && root_text.back() == '/') {
        root_text.pop_back();
    }
    return candidate_text == root_text ||
           (candidate_text.size() > root_text.size() &&
            candidate_text.compare(0, root_text.size(), root_text) == 0 &&
            candidate_text[root_text.size()] == '/');
}

// 确认候选路径位于根目录内并返回相对路径。
inline fs::path fs_relative_path_within(
    const fs::path& root,
    const fs::path& candidate,
    std::error_code& ec)
{
    const auto canonical_root = fs_canonical_existing(root, ec);
    if (ec) {
        return {};
    }
    const auto canonical_candidate = fs_canonical_existing(candidate, ec);
    if (ec) {
        return {};
    }

    auto root_text = canonical_root.generic_string();
    const auto candidate_text = canonical_candidate.generic_string();
    while (root_text.size() > 1 && root_text.back() == '/') {
        root_text.pop_back();
    }
    if (candidate_text == root_text) {
        return fs::path(".");
    }
    if (candidate_text.size() <= root_text.size() ||
        candidate_text.compare(0, root_text.size(), root_text) != 0 ||
        candidate_text[root_text.size()] != '/') {
        ec = std::make_error_code(std::errc::permission_denied);
        return {};
    }
    ec.clear();
    return fs::path(candidate_text.substr(root_text.size() + 1));
}

}  // namespace edge
