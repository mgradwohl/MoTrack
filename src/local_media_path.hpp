#pragma once

#include <QString>

#include <string>

namespace motrack
{
/// Physical parent of an existing project, or its existing parent when the project has not been saved yet.
/// Resolves directory/file aliases before relative media paths are interpreted. Missing parents return empty.
[[nodiscard]] QString localProjectDirectory(const QString& projectPath);
/// Resolves local media using the physical project directory. Archive/URL references return empty.
/// Existing paths are canonicalized before '..' can be lexically collapsed across a symbolic link.
[[nodiscard]] QString resolveLocalMediaPath(const QString& projectPath, const std::string& reference);
} // namespace motrack
