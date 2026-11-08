#include "local_media_path.hpp"

#include <QDir>
#include <QFileInfo>

namespace motrack
{
QString localProjectDirectory(const QString& projectPath)
{
    const QFileInfo project(projectPath);
    const auto existing = project.canonicalFilePath();
    return existing.isEmpty() ? QFileInfo(project.path()).canonicalFilePath() : QFileInfo(existing).path();
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
    const auto existing = QFileInfo(path).canonicalFilePath();
    return existing.isEmpty() ? path : existing;
}
} // namespace motrack
