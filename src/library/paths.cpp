#include "library/paths.hpp"

#include "library/java_string.hpp"

#include <algorithm>
#include <cstddef>

namespace motrack::library
{
namespace
{

void replaceAll(std::string& text, std::string_view from, std::string_view to)
{
    std::size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos)
    {
        text.replace(position, from.size(), to);
        position += to.size();
    }
}

/// Absolute in the sense of XML.getPathRelativeTo: a leading '/' or any ':' (drive letters, URLs).
bool isRootedForRelativePath(std::string_view path) noexcept
{
    return path.starts_with('/') || path.find(':') != std::string_view::npos;
}

} // namespace

std::string forwardSlash(std::string_view path)
{
    std::string result(path);
    std::ranges::replace(result, '\\', '/');
    return result;
}

std::string_view fileName(std::string_view path) noexcept
{
    auto separator = path.rfind('/');
    if (separator == std::string_view::npos)
    {
        separator = path.rfind('\\');
    }
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

std::optional<std::string_view> extension(std::string_view path) noexcept
{
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= path.size())
    {
        return std::nullopt;
    }
    // forwardSlash(fileName).lastIndexOf('/'): either separator counts.
    const auto separator = path.find_last_of("/\\");
    if (separator != std::string_view::npos && separator > dot)
    {
        return std::nullopt;
    }
    return path.substr(dot + 1);
}

std::string stripExtension(std::string_view path)
{
    auto result = forwardSlash(path);
    const auto separator = result.rfind('/');
    const std::size_t nameStart = separator == std::string::npos ? 0 : separator + 1;
    const auto dot = result.rfind('.');
    if (dot != std::string::npos && dot > nameStart && dot + 1 < result.size())
    {
        result.erase(dot);
    }
    while (!result.empty() && result.back() == '.')
    {
        result.pop_back();
    }
    return result;
}

std::string directoryPath(std::string_view path)
{
    auto result = forwardSlash(path);
    const auto separator = result.rfind('/');
    if (separator == std::string::npos)
    {
        return {};
    }
    result.erase(separator);
    return result;
}

std::string resolvedPath(std::string_view relative, std::string_view base)
{
    if (base.ends_with('/'))
    {
        base.remove_suffix(1);
    }
    auto path = forwardSlash(relative);
    if (path.starts_with('/') || path.find(":/") != std::string::npos)
    {
        return path;
    }
    auto root = forwardSlash(base);
    std::string_view remaining = path;
    while (remaining.starts_with("../") && !root.empty())
    {
        if (root.find('/') == std::string::npos)
        {
            root.insert(root.begin(), '/');
        }
        remaining.remove_prefix(3);
        root.erase(root.rfind('/'));
    }
    if (remaining.starts_with("./"))
    {
        remaining.remove_prefix(2);
    }
    if (remaining == ".")
    {
        remaining = {};
    }
    if (root.empty())
    {
        return std::string(remaining);
    }
    if (root.ends_with('/') || remaining.starts_with('&'))
    {
        return root + std::string(remaining);
    }
    return root + "/" + std::string(remaining);
}

std::string relativePath(std::string_view absolute, std::string_view base)
{
    auto path = forwardSlash(absolute);
    auto directory = forwardSlash(base);
    if (!isRootedForRelativePath(path) || !isRootedForRelativePath(directory))
    {
        return path;
    }
    if (directory.ends_with('/'))
    {
        directory.pop_back();
    }
    std::string climb;
    for (int level = 0; level < 6; ++level)
    {
        if (level > 0)
        {
            const auto separator = directory.rfind('/');
            if (separator != std::string::npos)
            {
                directory.erase(separator);
            }
            else if (!directory.empty())
            {
                directory.clear();
            }
            else
            {
                break;
            }
            climb += "../";
        }
        // Only whole segments match: "C:/lib" is not an ancestor of "C:/library/a.html".
        if (!directory.empty() && path.starts_with(directory) && (path.size() == directory.size() || path[directory.size()] == '/'))
        {
            auto rest = std::string_view(path).substr(directory.size());
            if (rest.starts_with('/'))
            {
                rest.remove_prefix(1);
            }
            return climb + std::string(rest);
        }
    }
    return path;
}

bool isHttp(std::string_view path) noexcept
{
    return path.starts_with("http:") || path.starts_with("https:");
}

bool isJarZipTrz(std::string_view path) noexcept
{
    return path.ends_with(".jar") || path.ends_with(".zip") || path.ends_with(".trz");
}

std::optional<ArchiveMember> splitArchiveMember(std::string_view path) noexcept
{
    const auto marker = path.find("!/");
    if (marker == std::string_view::npos)
    {
        return std::nullopt;
    }
    return ArchiveMember{.archive = path.substr(0, marker), .member = path.substr(marker + 2)};
}

std::string nonUriPath(std::string_view uriPath)
{
    if (uriPath.starts_with("jar:"))
    {
        uriPath.remove_prefix(4);
    }
    if (uriPath.starts_with("file:"))
    {
        uriPath.remove_prefix(5);
    }
    if (uriPath.starts_with('/') && uriPath.find(':') != std::string_view::npos)
    {
        uriPath.remove_prefix(1);
    }
    std::string result(uriPath);
    replaceAll(result, "%20", " ");
    std::ranges::replace(result, '&', '_');
    std::ranges::replace(result, '?', '_');
    return result;
}

std::string uriPath(std::string_view path, UriPlatform platform)
{
    auto result = forwardSlash(javaTrimmed(path));
    if (!result.empty() && !extension(result) && !result.ends_with('/'))
    {
        result += '/';
    }
    if (isHttp(result))
    {
        replaceAll(result, " ", "%20");
    }
    if (result.starts_with('/'))
    {
        return "file:" + result;
    }
    if (!result.empty() && !isHttp(result) && !result.starts_with("jar:") && !result.starts_with("file:/"))
    {
        result.insert(0, platform == UriPlatform::Windows ? "file:/" : "file://");
    }
    return result;
}

} // namespace motrack::library
