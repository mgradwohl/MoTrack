#pragma once

#include "capture_dialog.hpp"
#include "desktop_launch.hpp"
#include "workspace_context.hpp"

#include <QMainWindow>
#include <QPointer>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

class QAction;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QTabWidget;

namespace motrack
{

class MainWindow;

/// Desktop owner for retained experiment views. Hiding a view retains edits; closing a source
/// explicitly resolves its entire archive group's save/discard decision, including hidden members.
class WorkspaceWindow final : public QMainWindow
{
    Q_OBJECT

  public:
    using CameraSourceFactory = std::function<std::unique_ptr<CameraSource>()>;

    explicit WorkspaceWindow(std::shared_ptr<WorkspaceContext> context = {});
    ~WorkspaceWindow() override;
    [[nodiscard]] const std::shared_ptr<WorkspaceContext>& context() const;
    [[nodiscard]] std::optional<QUuid> openProject(const QString& path);
    [[nodiscard]] std::optional<QUuid> openMovie(const QString& path);
    [[nodiscard]] std::optional<QUuid> openImages(const QStringList& paths, double frameRate);
    [[nodiscard]] std::optional<QUuid> currentDocument() const;
    bool showDocument(QUuid document);
    bool hideDocument(QUuid document);
    bool revertDocument(QUuid document);
    bool saveAll();
    bool closeSource(QUuid source);
    /// Cameras for Capture from camera; Qt Multimedia's by default. Tests supply cameras without hardware.
    void setCameraSourceFactory(CameraSourceFactory factory);
    /// Opens the capture window, or raises the open one. A clip it saves opens as a new movie experiment.
    CaptureDialog* captureFromCamera();
    /// Open files from the command line, a drop or the operating system in the given order, through the same
    /// entry points as the menus (see launchFileKind). One requested file prepares at a time, so tabs keep this
    /// order while a menu open may use the other input slot. Requests arriving during a batch join it; a file
    /// already requested in the batch opens once. Missing, unsupported, remote and failed requests are reported
    /// together when the batch finishes.
    void openFiles(RequestedFiles requested);

  Q_SIGNALS:
    /// Every file of the batch has opened, failed or been skipped. Problems are files that did not open or opened
    /// incompletely (an unreadable archive member); notices are limits of files that opened, such as skipped
    /// unsafe archive members. Both are user-visible sentences.
    void filesOpened(const QStringList& problems, const QStringList& notices);

  protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    void addDocuments(const QList<QUuid>& documents);
    void createView(QUuid document);
    void removeView(QUuid document, bool activateRemaining = true);
    void refreshMenus();
    void refreshLabels();
    void rebuildExperimentsMenu();
    [[nodiscard]] QString documentLabel(QUuid document) const;
    [[nodiscard]] std::optional<QUuid> trackOpen(std::optional<QUuid> request, const QString& label);
    void finishOpen(QUuid request, const QString& problem = {}, bool cancelled = false);
    void continueRequestedFiles();
    /// Drop the batch's files that have not started; a running check's result is then ignored.
    void stopRequestedFiles();
    /// Requested files wait while a save, revert or close prompt runs its nested event loop.
    void setBusy(bool busy);
    /// Accept a drag of local files as a copy or link; a refused drag returns no files, only its remote links.
    [[nodiscard]] RequestedFiles acceptDrag(QDropEvent* event);
    /// What reportArchiveLimits showed: informational limits, and unreadable projects (an incomplete import).
    struct ArchiveNotices
    {
        QStringList limits;
        QString unreadable;
    };
    /// Report skipped unsafe archive members, the ones a standard save omits, and unreadable projects.
    /// Returns them too, so a batch of requested files can keep them for its summary.
    [[nodiscard]] ArchiveNotices reportArchiveLimits(const QList<QUuid>& documents, const QList<UnreadableProject>& unreadable);
    bool saveSource(QUuid source);
    [[nodiscard]] std::optional<WorkspaceDiscard> confirmSourceClose(QUuid source);
    bool releaseSource(QUuid source, WorkspaceDiscard discard);

    std::shared_ptr<WorkspaceContext> m_Context;
    QTabWidget* m_Tabs = nullptr;
    QLabel* m_Empty = nullptr;
    QListWidget* m_Pending = nullptr;
    QMenu* m_EmptyFileMenu = nullptr;
    QMenu* m_ExperimentsMenu = nullptr;
    QList<QAction*> m_OpenActions;
    QAction* m_SaveAll = nullptr;
    QAction* m_HideView = nullptr;
    QAction* m_Revert = nullptr;
    QAction* m_CloseSource = nullptr;
    QAction* m_Quit = nullptr;
    CameraSourceFactory m_CameraSources;
    QPointer<CaptureDialog> m_Capture; // Qt parent-owned; deletes itself when closed.
    QHash<QUuid, MainWindow*> m_Views; // Qt parent-owned; views hold their own shared context lease.
    QHash<QUuid, QWidget*> m_Pages;    // Lightweight placeholders until an experiment is first selected.
    QHash<QUuid, QListWidgetItem*> m_Opening;
    QHash<QUuid, QList<UnreadableProject>> m_Unreadable; // Per source, listed in the Experiments menu.
    /// Files requested from outside the menus, opened one at a time in their requested order. Each file is
    /// first checked on a worker; `check` identifies the running check, `ready` a checked file awaiting a slot
    /// and `select` an already-open source to show, both deferred while a modal prompt runs.
    struct RequestedBatch
    {
        QStringList files;   // QList moves without throwing.
        QStringList started; // Files opened or selected in this batch, for duplicate detection.
        qsizetype accepted = 0;
        std::uint64_t check = 0;
        std::optional<QString> ready;
        std::optional<std::pair<QUuid, QString>> select;
        std::optional<QUuid> request;
        QString current;
        QStringList problems;
        QStringList notices;
    };
    void checkNextRequestedFile(RequestedBatch& batch);
    void applyRequestedFileCheck(std::uint64_t check, const QString& path, const RequestedFileCheck& result);
    void startRequestedOpen(RequestedBatch& batch, const QString& path);
    std::optional<RequestedBatch> m_Requested;
    std::uint64_t m_LastCheck = 0;
    QLabel* m_DropOverlay = nullptr;
    bool m_Busy = false;
};

} // namespace motrack
