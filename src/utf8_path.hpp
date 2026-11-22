#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace motrack
{

// Qt-free UTF-8 <-> std::filesystem::path conversions, for code without a QString (src/filesystem_path.hpp
// converts QString). A narrow std::filesystem::path(std::string) and path.string() use the ANSI code page on
// Windows, so they lose or misread names outside it (CONTRIBUTING.md, "Text encoding and locale").

/// The path named by UTF-8 @p text, such as a fixture root from CMake.
[[nodiscard]] inline std::filesystem::path utf8Path(std::string_view text)
{
    return std::u8string(text.begin(), text.end());
}

/// @p path as UTF-8 text with native separators.
[[nodiscard]] inline std::string utf8String(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

/// @p path as UTF-8 text with '/' separators.
[[nodiscard]] inline std::string genericUtf8String(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

} // namespace motrack
