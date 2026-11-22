#pragma once

#include <QString>

#include <cstdint>
#include <optional>
#include <string>

namespace motrack
{
/// Physical save destination, including a file alias whose target does not exist yet.
/// Returns empty for cyclic aliases or missing parent directories.
[[nodiscard]] QString localProjectPath(const QString& projectPath);
/// Physical parent of an existing project, or its existing parent when the project has not been saved yet.
/// Resolves directory/file aliases before relative media paths are interpreted. Missing parents return empty.
[[nodiscard]] QString localProjectDirectory(const QString& projectPath);
/// Resolves local media using the physical project directory. Archive/URL references return empty.
/// Existing paths are canonicalized before '..' can be lexically collapsed across a symbolic link.
[[nodiscard]] QString resolveLocalMediaPath(const QString& projectPath, const std::string& reference);
/// Refresh an already resolved physical path, including missing targets and restored file aliases.
/// Unlike a serialized Tracker reference, a physical path may contain literal archive delimiters.
[[nodiscard]] QString physicalLocalFilePath(QString path);
/// The file system's identity of an existing file: its volume and file id, as std::filesystem::equivalent compares
/// them. Two paths share it exactly when they name one file, whether through case variants, aliases, hard links or
/// Windows short names. Empty for a missing or unreadable file, and where fileIdentityKey finds no identity.
/// sameLocalFile and tabsetIdentityKey both use it. An id names a file only while it exists: a later file can get the
/// id of a deleted one, so a stored identity must not stand alone for a file that may have gone since.
[[nodiscard]] std::optional<QString> localFileIdentity(const QString& path);
/// The identity key of a volume serial or device and a 128-bit file id (`high`, `low`). Empty when either is zero:
/// some network redirectors answer id 0 for every file, and a zero serial does not tell their shares apart, so such
/// files compare by name rather than all as one file.
[[nodiscard]] std::optional<QString> fileIdentityKey(std::uint64_t volume, std::uint64_t high, std::uint64_t low);
/// Compare existing file identities using the actual filesystem, preserving distinct case/Unicode spellings.
/// Missing paths compare by their absolute spelling; callers resolve directory aliases first.
[[nodiscard]] bool sameLocalFile(const QString& first, const QString& second);
} // namespace motrack
