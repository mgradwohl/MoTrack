#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>

#include <cstdint>

class QMimeData;

namespace motrack
{

class WorkspaceWindow;

/// Workspace entry point for one requested file, chosen from its suffix on the owner thread.
enum class LaunchFileKind : std::uint8_t
{
    Project, ///< .trk, .trz or .motrack, in any letter case.
    Image,   ///< A suffix Qt's image readers support; opens as a one-image sequence with unknown timing.
    Movie,   ///< Everything else; the movie indexer reports unsupported formats.
};

[[nodiscard]] LaunchFileKind launchFileKind(const QString& path);

/// Files and links requested by the operating system or a drop, kept in their delivered order.
struct RequestedFiles
{
    QStringList files{};  ///< Absolute local paths with their original spelling; aliases are resolved when opened.
    QStringList remote{}; ///< Links that are not local files, for display. They are reported, never fetched.
};

/// Process arguments after QApplication has removed its own options.
struct LaunchArguments
{
    RequestedFiles requested;
    QStringList unknownOptions; ///< Ignored with a console warning; `--` makes later arguments files.
    bool smokeTest = false;     ///< `--smoke-test`: report the opened sources and exit after the files finish.
};

/// Absolute local spelling of a path handed over by a launcher. Relative paths use the launch directory.
/// Windows extended-length prefixes are removed because Qt adds its own for long paths; nothing is canonicalized.
[[nodiscard]] QString launchFilePath(QString path, const QString& workingDirectory);
/// Argument 0 is the program. Local `file:` URLs become paths; other URLs are remote. Empty arguments and the
/// legacy macOS `-psn_` process serial number are ignored.
[[nodiscard]] LaunchArguments parseLaunchArguments(const QStringList& arguments, const QString& workingDirectory);
/// Local files and refused links in a drag. Data without URLs yields nothing.
[[nodiscard]] RequestedFiles droppedFiles(const QMimeData& data);

/// Application event filter for operating-system open requests: Finder double-click, Open With and Dock
/// drops arrive as QFileOpenEvent on macOS. Requests that arrive before a window is attached, including
/// during launch, are retained in order and delivered on attach. The window is borrowed and may be destroyed;
/// later requests are then retained again.
class FileOpenRequests final : public QObject
{
    Q_OBJECT

  public:
    using QObject::QObject;
    void attach(WorkspaceWindow* window);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void deliver();

    QPointer<WorkspaceWindow> m_Window;
    RequestedFiles m_Pending;
};

} // namespace motrack
