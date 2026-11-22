#pragma once

#include "capture_dialog.hpp"
#include "workspace_context.hpp"

#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <functional>
#include <optional>
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

  Q_SIGNALS:
    /// Every listed file has finished opening, failed or been skipped, and the selection is final.
    void tabsetFinished(const QString& tabset);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void addDocuments(const QList<QUuid>& documents);
    void createView(QUuid document);
    void removeView(QUuid document, bool activateRemaining = true);
    void refreshMenus();
    void refreshLabels();
    void rebuildExperimentsMenu();
    [[nodiscard]] QString documentLabel(QUuid document) const;
    [[nodiscard]] std::optional<QUuid> trackOpen(std::optional<QUuid> request, const QString& label);
    void finishOpen(QUuid request);
    /// Report skipped unsafe archive members, the ones a standard save omits, and unreadable projects.
    void reportArchiveLimits(const QList<QUuid>& documents, const QList<UnreadableProject>& unreadable);
    bool saveSource(QUuid source);
    [[nodiscard]] std::optional<QUuid> requestInput(const QString& path, WorkspaceActivation activation);
    void beginQueuedTabset();
    [[nodiscard]] std::optional<QString> describeTabsetProblem(const TabsetItem& item) const;
    void beginTabset(const QString& tabset, const std::vector<TabsetItem>& items);
    void continueTabset();
    void completeTabsetInput(QUuid request, const QList<QUuid>& documents, const QString& problem, bool cancelled = false);
    [[nodiscard]] std::optional<QString> tabsetReference(QUuid source, QStringList& problems, QStringList& notices, bool& cancelled);
    /// Identity keys of a source's files (source, standard and native paths) for tabset saving.
    [[nodiscard]] QStringList sourceFileKeys(QUuid source) const;
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
    struct OrderedOpens
    {
        QString tabset;       // The tabset that started the queue; named in its summary and tabsetFinished.
        QStringList files;    // QList moves without throwing; std::deque may allocate in some libraries.
        QSet<QString> listed; // Identity keys of every file queued or opened by this queue.
        std::optional<QUuid> request;
        QString current;
        bool requestIsProject = false;
        std::optional<QUuid> select;
        QStringList problems; // Files that were not opened.
        QStringList notices;  // Files that opened with a limitation, such as B1's stale-link recovery.
    };
    /// Queue the resolved files of a tabset that the queue's current request turned out to be, ahead of the
    /// remaining files, and record the others as problems. No prompts: they run only for the tabset the user opened.
    void expandInPlace(OrderedOpens& queue, const std::vector<TabsetItem>& items) const;
    /// A file or tabset is opening, or a tabset is prompting or opening its files; saving a tabset waits for it.
    [[nodiscard]] bool tabsetOpening() const;
    std::optional<OrderedOpens> m_Tabset;
    /// The last tabset opened, offered by Save tabset as… so that an edited set can be saved back over it.
    QString m_LastTabset;
    /// Identity keys of files the user closed; saving over a tabset drops only their entries among unopened ones.
    QSet<QString> m_ClosedFiles;
    /// Expanded tabsets waiting for their missing-file prompts, which run after the context callback returns.
    struct ExpandedTabset
    {
        QString tabset;
        std::vector<TabsetItem> items;
    };
    std::vector<ExpandedTabset> m_ExpandedTabsets;
    bool m_TabsetPrompting = false;
    QHash<QUuid, QList<UnreadableProject>> m_Unreadable; // Per source, listed in the Experiments menu.
    bool m_Busy = false;
};

} // namespace motrack
