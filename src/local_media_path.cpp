#include "local_media_path.hpp"

#include "filesystem_path.hpp"

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
    auto remaining = toFilesystemPath(path);
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
            const auto targetName = QFileInfo(fromFilesystemPath(prefix)).symLinkTarget();
            if (targetName.isEmpty())
                return {};
            auto target = toFilesystemPath(targetName);
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
            return fromFilesystemPath(remaining);
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
    const auto nativePath = toFilesystemPath(path);
    std::error_code error;
    static_cast<void>(std::filesystem::status(nativePath, error));
    // Some standard libraries discard a failing ancestor status in weakly_canonical. Missing files
    // are valid retained references; loops, inaccessible paths and non-directory ancestors are not.
    if (error && error != std::errc::no_such_file_or_directory)
        return {};
    error.clear();
    const auto normalized = std::filesystem::weakly_canonical(nativePath, error);
    return error ? QString{} : fromFilesystemPath(normalized);
}

bool sameLocalFile(const QString& first, const QString& second)
{
    std::error_code error;
    bool equivalent = std::filesystem::equivalent(toFilesystemPath(first), toFilesystemPath(second), error);
#ifdef Q_OS_WIN
    if (error)
    {
        // Qt can open paths beyond MAX_PATH even when the standard-library identity query
        // needs an extended Windows path. Resolve existing paths before adding that prefix,
        // which disables ordinary Win32 dot/parent normalization.
        const auto extendedPath = [](const QString& path)
        {
            auto canonical = QDir::toNativeSeparators(QFileInfo(path).canonicalFilePath());
            if (canonical.isEmpty())
                return std::filesystem::path{};
            if (!canonical.startsWith(R"(\\?\)"))
                canonical = canonical.startsWith(R"(\\)") ? R"(\\?\UNC\)" + canonical.mid(2) : R"(\\?\)" + canonical;
            return toFilesystemPath(canonical);
        };
        const auto left = extendedPath(first);
        const auto right = extendedPath(second);
        if (!left.empty() && !right.empty())
        {
            error.clear();
            equivalent = std::filesystem::equivalent(left, right, error);
        }
    }
#endif
    if (!error)
        return equivalent;
    return QFileInfo(first).absoluteFilePath() == QFileInfo(second).absoluteFilePath();
}
} // namespace motrack
