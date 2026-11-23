#pragma once

#include "capture_dialog.hpp"
#include "desktop_launch.hpp"
#include "workspace_context.hpp"

#include <QFutureWatcher>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

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
    /// Write a Tracker tabset listing every open source's saved Tracker file, or its unchanged single movie or
    /// image, in tab order. Unsaved sources are offered Save/Skip/Cancel first. An empty path asks for one.
    bool saveTabset(const QString& path = {});
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

  Q_SIGNALS:
    /// Every listed file has finished opening, failed or been skipped, and the selection is final.
    void tabsetFinished(const QString& tabset);

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
    [[nodiscard]] std::optional<QUuid> requestInput(const QString& path, WorkspaceActivation activation);
    void beginQueuedTabset();
    [[nodiscard]] std::optional<QString> describeTabsetProblem(const TabsetItem& item) const;
    void beginTabset(const QString& tabset, const std::vector<TabsetItem>& items);
    void continueTabset();
    void completeTabsetInput(QUuid request, const QList<QUuid>& documents, const QString& problem, bool cancelled = false);
    [[nodiscard]] std::optional<QString> tabsetReference(QUuid source, QStringList& problems, QStringList& notices, bool& cancelled);
    /// The local paths of a source's files (source, standard and native paths) for tabset saving.
    [[nodiscard]] QStringList sourceLocalFiles(QUuid source) const;
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
    QAction* m_SaveTabset = nullptr;
    QAction* m_HideView = nullptr;
    QAction* m_Revert = nullptr;
    QAction* m_CloseSource = nullptr;
    QAction* m_Quit = nullptr;
    CameraSourceFactory m_CameraSources;
    QPointer<CaptureDialog> m_Capture; // Qt parent-owned; deletes itself when closed.
    QHash<QUuid, MainWindow*> m_Views; // Qt parent-owned; views hold their own shared context lease.
    QHash<QUuid, QWidget*> m_Pages;    // Lightweight placeholders until an experiment is first selected.
    QHash<QUuid, QListWidgetItem*> m_Opening;
    /// The window's one ordered open queue. Tracker opens a tabset's files one at a time, so tabs keep the listed
    /// order within the two-input bound. A queued file that turns out to be a tabset itself (a located file)
    /// expands in place through expandInPlace: its files open next, before the rest of the queue. Other ordered
    /// opens, such as files requested by the operating system, are meant to join this queue rather than run a
    /// second one that would compete for the same input slots; a tabset among them then expands in place too.
    /// A queued file, or one member of a queued archive (`archive.trz!/member.trk` in a tabset).
    struct QueuedFile
    {
        QString path;
        QString member{}; // As the tabset spells it; empty for the whole file.
    };
    struct OrderedOpens
    {
        QString tabset;          // The tabset that started the queue; named in its summary and tabsetFinished.
        QList<QueuedFile> files; // QList moves without throwing; std::deque may allocate in some libraries.
        QSet<QString> listed;    // tabsetTargetKey of every file or member queued or opened by this queue.
        std::optional<QUuid> request;
        QString current;
        QString currentMember; // The only member the current request shows, if it names one.
        bool requestIsProject = false;
        std::optional<QUuid> select;
        QStringList problems;           // Files that were not opened.
        QStringList notices;            // Files that opened with a limitation, such as B1's stale-link recovery.
        std::uint64_t check = 0;        // The request-worker check of files.front() that is running, if any.
        std::optional<QString> checked; // files.front().path once its check found it is not open yet.
        QSet<QUuid> partial;            // Archives this queue opened showing only their listed members.
    };
    /// The documents of an opened source that `queue`'s current request shows: every one, or the listed member's.
    /// The others are hidden, keeping their records for the archive's saves.
    [[nodiscard]] QList<QUuid>
    shownDocuments(OrderedOpens& queue, QUuid source, const QList<QUuid>& documents, const QList<UnreadableProject>& unreadable);
    /// The document of `member` among `documents`, comparing NFC names as archives do.
    [[nodiscard]] std::optional<QUuid> memberDocument(const std::vector<QUuid>& documents, const QString& member) const;
    /// The label of a queued file in problems and notices: its name, with the member for an archive member.
    [[nodiscard]] static QString queuedLabel(const QueuedFile& file);
    /// Queue the resolved files of a tabset that the queue's current request turned out to be, ahead of the
    /// remaining files, and record the others as problems. No prompts: they run only for the tabset the user opened.
    void expandInPlace(OrderedOpens& queue, const std::vector<TabsetItem>& items);
    void checkTabsetFile(OrderedOpens& pending, const QString& file);
    void applyTabsetFileCheck(std::uint64_t check, const QString& file, const RequestedFileCheck& result);
    /// Each open source with the files it owns: the one it was opened from and its native and standard destinations.
    [[nodiscard]] QList<std::pair<QUuid, QString>> openSourceFiles() const;
    /// A file or tabset is opening, or a tabset is prompting or opening its files; saving a tabset waits for it.
    [[nodiscard]] bool tabsetOpening() const;
    std::optional<OrderedOpens> m_Tabset;
    /// The last tabset opened, offered by Save tabset as… so that an edited set can be saved back over it.
    QString m_LastTabset;
    /// Files the user closed, by name key, with each one's identity when it closed. Saving over a tabset drops only
    /// unopened entries whose file still has both (see tabsetEntries).
    QHash<QString, std::optional<QString>> m_ClosedFiles;
    /// Expanded tabsets waiting for their missing-file prompts, which run after the context callback returns.
    struct ExpandedTabset
    {
        QString tabset;
        std::vector<TabsetItem> items;
    };
    std::vector<ExpandedTabset> m_ExpandedTabsets;
    bool m_TabsetPrompting = false;
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
    std::uint64_t m_LastCheck = 0; // The check m_Checks watches; a stopped batch no longer expects it.
    QString m_CheckedPath;
    QFutureWatcher<RequestedFileCheck> m_Checks;
    std::uint64_t m_TabsetCheck = 0; // The check m_TabsetChecks watches for the tabset queue.
    QString m_TabsetCheckedPath;
    QFutureWatcher<RequestedFileCheck> m_TabsetChecks;
    QLabel* m_DropOverlay = nullptr;
    bool m_Busy = false;
};

} // namespace motrack
