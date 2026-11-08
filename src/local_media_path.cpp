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
QString followFileAliases(const QString& path)
{
    // Resolve each ancestor before canonicalization: a dangling directory alias still has a
    // meaningful target, and some canonical-path implementations return the input for loops.
    std::filesystem::path remaining(path.toStdU16String());
    for (int depth = 0; depth < 128; ++depth)
    {
        auto prefix = remaining.root_path();
        const auto relative = remaining.relative_path();
        bool followed = false;
        for (auto part = relative.begin(); part != relative.end(); ++part)
        {
            prefix /= *part;
            std::error_code error;
            const auto status = std::filesystem::symlink_status(prefix, error);
            if (error && error != std::errc::no_such_file_or_directory)
                return {};
            if (!std::filesystem::is_symlink(status))
                continue;
#ifdef Q_OS_WIN
            // Qt resolves Windows reparse-point targets without the MSVC bitmask analyzer false positive.
            const auto targetName = QFileInfo(QString::fromStdU16String(prefix.u16string())).symLinkTarget();
            if (targetName.isEmpty())
                return {};
            std::filesystem::path target(targetName.toStdU16String());
#else
            auto target = std::filesystem::read_symlink(prefix, error);
            if (error)
                return {};
#endif
            if (target.is_relative())
                target = prefix.parent_path() / target;
            while (++part != relative.end())
                target /= *part;
            remaining = std::move(target);
            followed = true;
            break;
        }
        if (!followed)
            return QString::fromStdU16String(remaining.u16string());
    }
    return {};
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
    path = followFileAliases(path);
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
