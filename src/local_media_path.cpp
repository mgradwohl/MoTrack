#include "local_media_path.hpp"

#include <QDir>
#include <QFileInfo>

#include <filesystem>
#include <system_error>
#include <utility>

namespace motrack
{
namespace
{
QString followFileAliases(QString path)
{
    // Follow even dangling file aliases; canonical/weakly_canonical alone leave those unresolved.
    for (int depth = 0; QFileInfo(path).isSymLink(); ++depth)
    {
        if (depth == 128)
            return {};
        path = QFileInfo(path).symLinkTarget();
    }
    return path;
}
} // namespace

QString localProjectPath(const QString& projectPath)
{
    const auto resolved = followFileAliases(projectPath);
    if (resolved.isEmpty())
        return {};
    const QFileInfo project(resolved);
    auto existing = project.canonicalFilePath();
    if (!existing.isEmpty())
        return existing;
    const auto parent = QFileInfo(project.path()).canonicalFilePath();
    return parent.isEmpty() ? QString{} : QDir(parent).filePath(project.fileName());
}

QString localProjectDirectory(const QString& projectPath)
{
    const auto physical = localProjectPath(projectPath);
    return physical.isEmpty() ? QString{} : QFileInfo(physical).path();
}

QString resolveLocalMediaPath(const QString& projectPath, const std::string& reference)
{
    auto path = QString::fromStdString(reference);
    if (path.isEmpty() || path.contains("!/") || path.contains("://") || path.startsWith(':'))
        return {};
    if (!QDir::isAbsolutePath(path))
    {
        const auto directory = localProjectDirectory(projectPath);
        if (directory.isEmpty())
            return {};
        path = directory + '/' + path;
    }
    return physicalLocalFilePath(std::move(path));
}

QString physicalLocalFilePath(QString path)
{
    path = followFileAliases(std::move(path));
    if (path.isEmpty())
        return {};
    auto existing = QFileInfo(path).canonicalFilePath();
    if (!existing.isEmpty())
        return existing;
    // Preserve the physical base even when a missing image is restored after its project directory
    // is removed. Resolve existing prefixes before normalizing '..', including directory aliases.
    const std::filesystem::path nativePath(path.toStdU16String());
    std::error_code error;
    static_cast<void>(std::filesystem::status(nativePath, error));
    // Some standard libraries discard a failing ancestor status in weakly_canonical. Missing files
    // are valid retained references; loops, inaccessible paths and non-directory ancestors are not.
    if (error && error != std::errc::no_such_file_or_directory)
        return {};
    error.clear();
    const auto normalized = std::filesystem::weakly_canonical(nativePath, error);
    return error ? QString{} : QString::fromStdU16String(normalized.u16string());
}

bool sameLocalFile(const QString& first, const QString& second)
{
    std::error_code error;
    const bool equivalent =
        std::filesystem::equivalent(std::filesystem::path(first.toStdU16String()), std::filesystem::path(second.toStdU16String()), error);
    if (!error)
        return equivalent;
    return QFileInfo(first).absoluteFilePath() == QFileInfo(second).absoluteFilePath();
}
} // namespace motrack
