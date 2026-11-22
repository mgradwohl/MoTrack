#pragma once

// QString <-> std::filesystem::path, independent of the Windows ANSI code page and the C locale
// (CONTRIBUTING.md, "Text encoding and locale"). Every conversion between the two goes through these.
//
// On Windows a path holds UTF-16 and these pass it through unchanged; path(std::string) and path::string() would
// convert through the ANSI code page, which is UTF-8 only in motrack.exe (its manifest) and not in the test
// binaries. On POSIX a path holds bytes and these use UTF-8, as Qt's own std::filesystem interoperation does.

#include <QString>

#include <filesystem>

namespace motrack
{

/// The path @p text names: its UTF-16 on Windows, its UTF-8 bytes elsewhere.
[[nodiscard]] inline std::filesystem::path toFilesystemPath(const QString& text)
{
#ifdef _WIN32
    return {text.toStdWString()};
#else
    return {text.toStdString()};
#endif
}

/// @p path as text for Qt's file APIs and display. On POSIX, bytes that are not UTF-8 become U+FFFD, so such a
/// result cannot name that file again; on Windows an unpaired surrogate survives.
[[nodiscard]] inline QString fromFilesystemPath(const std::filesystem::path& path)
{
#ifdef _WIN32
    return QString::fromStdWString(path.native());
#else
    return QString::fromStdString(path.native());
#endif
}

} // namespace motrack
