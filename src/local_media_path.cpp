#include "local_media_path.hpp"

#include <QDir>
#include <QFileInfo>

#include <filesystem>
#include <system_error>

namespace motrack
{
QString localProjectPath(const QString& projectPath)
{
    QFileInfo project(projectPath);
    // Match QSaveFile's destination semantics, including a link to a not-yet-created file.
    for (int depth = 0; project.isSymLink(); ++depth)
    {
        if (depth == 128)
            return {};
        project.setFile(project.symLinkTarget());
    }
    const auto existing = project.canonicalFilePath();
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
    auto existing = QFileInfo(path).canonicalFilePath();
    if (!existing.isEmpty())
        return existing;
    // Preserve the physical base even when a missing image is restored after its project directory
    // is removed. Resolve existing prefixes before normalizing '..', including directory aliases.
    std::error_code error;
    const auto normalized = std::filesystem::weakly_canonical(std::filesystem::path(path.toStdU16String()), error);
    return error ? path : QString::fromStdU16String(normalized.u16string());
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
