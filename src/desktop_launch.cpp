#include "desktop_launch.hpp"

#include "workspace_window.hpp"

#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QImageReader>
#include <QMimeData>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>
#include <array>
#include <utility>

namespace motrack
{
namespace
{

/// Desktop launchers and drops may hand over a local `file:` URL; other URLs are links to remote content.
/// A one-letter scheme is a Windows drive, so remote links need a scheme of at least two characters.
void addRequest(RequestedFiles& requested, const QString& argument, const QString& workingDirectory)
{
    if (argument.startsWith(QLatin1StringView("file:"), Qt::CaseInsensitive))
    {
        const QUrl url(argument);
        if (url.isValid() && url.isLocalFile() && !url.toLocalFile().isEmpty())
            requested.files.push_back(launchFilePath(url.toLocalFile(), workingDirectory));
        else
            requested.remote.push_back(argument);
        return;
    }
    static const QRegularExpression remoteLink(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]+://"));
    if (remoteLink.match(argument).hasMatch())
        requested.remote.push_back(QUrl(argument).toDisplayString(QUrl::RemoveUserInfo));
    else
        requested.files.push_back(launchFilePath(argument, workingDirectory));
}

} // namespace

LaunchFileKind launchFileKind(const QString& path)
{
    const auto suffix = QFileInfo(path).suffix();
    constexpr std::array PROJECT_SUFFIXES{"trk", "trz", "motrack"};
    if (std::ranges::any_of(PROJECT_SUFFIXES,
                            [&](const char* known) { return suffix.compare(QLatin1StringView(known), Qt::CaseInsensitive) == 0; }))
        return LaunchFileKind::Project;
    if (!suffix.isEmpty() && QImageReader::supportedImageFormats().contains(suffix.toLower().toLatin1()))
        return LaunchFileKind::Image;
    return LaunchFileKind::Movie;
}

QString launchFilePath(QString path, const QString& workingDirectory)
{
#ifdef Q_OS_WIN
    // Shells may pass Win32 extended-length spellings. Qt's file engine adds its own prefix to long
    // absolute paths, so retaining this one would give it an unusable doubled prefix.
    path = QDir::fromNativeSeparators(path);
    if (path.startsWith(QLatin1StringView("//?/UNC/"), Qt::CaseInsensitive))
        path = "//" + path.mid(8);
    else if (path.startsWith(QLatin1StringView("//?/")))
        path = path.mid(4);
#endif
    return QDir::isAbsolutePath(path) ? path : QDir(workingDirectory).absoluteFilePath(path);
}

LaunchArguments parseLaunchArguments(const QStringList& arguments, const QString& workingDirectory)
{
    LaunchArguments result;
    bool options = true;
    for (qsizetype i = 1; i < arguments.size(); ++i)
    {
        const auto& argument = arguments[i];
        if (argument.isEmpty())
            continue;
        if (options && argument.startsWith('-'))
        {
            if (argument == QLatin1StringView("--"))
                options = false;
            else if (argument == QLatin1StringView("--smoke-test"))
                result.smokeTest = true;
            else if (!argument.startsWith(QLatin1StringView("-psn_"))) // Older macOS launches add a process serial number.
                result.unknownOptions.push_back(argument);
            continue;
        }
        addRequest(result.requested, argument, workingDirectory);
    }
    return result;
}

RequestedFiles droppedFiles(const QMimeData& data)
{
    RequestedFiles result;
    if (!data.hasUrls())
        return result;
    for (const auto& url : data.urls())
    {
        if (url.isLocalFile() && !url.toLocalFile().isEmpty())
            result.files.push_back(launchFilePath(url.toLocalFile(), QDir::currentPath()));
        else if (!url.isEmpty())
            result.remote.push_back(url.toDisplayString(QUrl::RemoveUserInfo));
    }
    return result;
}

void FileOpenRequests::attach(WorkspaceWindow* window)
{
    m_Window = window;
    deliver();
}

bool FileOpenRequests::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::FileOpen)
        return QObject::eventFilter(watched, event);
    // A file event carries its path as a local URL; an "open URL" request for another scheme is remote.
    const auto url = static_cast<const QFileOpenEvent&>(*event).url();
    if (url.isLocalFile() && !url.toLocalFile().isEmpty())
        m_Pending.files.push_back(launchFilePath(url.toLocalFile(), QDir::currentPath()));
    else if (!url.isEmpty())
        m_Pending.remote.push_back(url.toDisplayString(QUrl::RemoveUserInfo));
    deliver();
    return true;
}

void FileOpenRequests::deliver()
{
    if (!m_Window || (m_Pending.files.isEmpty() && m_Pending.remote.isEmpty()))
        return;
    m_Window->openFiles(std::exchange(m_Pending, {}));
}

} // namespace motrack
