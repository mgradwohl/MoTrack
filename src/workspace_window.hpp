#pragma once

#include "desktop_launch.hpp"
#include "workspace_context.hpp"

#include <QMainWindow>
#include <QSet>

#include <optional>

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
    /// Open files from the command line, a drop or the operating system in the given order, through the same
    /// entry points as the menus (see launchFileKind). One requested file prepares at a time, so tabs keep this
    /// order while a menu open may use the other input slot. Requests arriving during a batch join it; a file
    /// already requested in the batch opens once. Missing, unsupported, remote and failed requests are reported
    /// together when the batch finishes.
    void openFiles(RequestedFiles requested);

  Q_SIGNALS:
    /// Every file of the batch has opened, failed or been skipped. Problems are user-visible sentences.
    void filesOpened(const QStringList& problems);

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
    /// Accept a drag of local files as a copy or link; a refused drag returns no files, only its remote links.
    [[nodiscard]] RequestedFiles acceptDrag(QDropEvent* event);
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
    QHash<QUuid, MainWindow*> m_Views; // Qt parent-owned; views hold their own shared context lease.
    QHash<QUuid, QWidget*> m_Pages;    // Lightweight placeholders until an experiment is first selected.
    QHash<QUuid, QListWidgetItem*> m_Opening;
    /// Files requested from outside the menus, opened one at a time in their requested order.
    struct RequestedBatch
    {
        QStringList files;    // QList moves without throwing.
        QSet<QString> listed; // Canonical identities started in this batch.
        qsizetype accepted = 0;
        std::optional<QUuid> request;
        QString current;
        QStringList problems;
    };
    std::optional<RequestedBatch> m_Requested;
    QLabel* m_DropOverlay = nullptr;
    bool m_Busy = false;
};

} // namespace motrack
