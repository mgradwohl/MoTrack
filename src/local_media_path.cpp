#include "local_media_path.hpp"

#include <QDir>
#include <QFileInfo>

namespace motrack
{
QString localProjectDirectory(const QString& projectPath)
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
