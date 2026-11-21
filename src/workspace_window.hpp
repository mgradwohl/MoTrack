#pragma once

#include "workspace_context.hpp"

#include <QMainWindow>
#include <QSet>
#include <QStringList>

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
    bool saveSource(QUuid source);
    [[nodiscard]] std::optional<QUuid> requestInput(const QString& path, WorkspaceActivation activation);
    void beginQueuedTabset();
    [[nodiscard]] std::optional<QString> describeTabsetProblem(const TabsetItem& item) const;
    void beginTabset(const QString& tabset, const std::vector<TabsetItem>& items);
    void continueTabset();
    void completeTabsetInput(QUuid request, const QList<QUuid>& documents, const QString& problem, bool cancelled = false);
    [[nodiscard]] std::optional<QString> tabsetReference(QUuid source, QStringList& problems, bool& cancelled);
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
    QHash<QUuid, MainWindow*> m_Views; // Qt parent-owned; views hold their own shared context lease.
    QHash<QUuid, QWidget*> m_Pages;    // Lightweight placeholders until an experiment is first selected.
    QHash<QUuid, QListWidgetItem*> m_Opening;
    /// Tracker opens a tabset's files one at a time; tabs keep the listed order.
    struct PendingTabset
    {
        QString tabset;
        QStringList files;    // QList moves without throwing; std::deque may allocate in some libraries.
        QSet<QString> listed; // Identity keys of every file queued or opened for this tabset.
        std::optional<QUuid> request;
        QString current;
        bool requestIsProject = false;
        std::optional<QUuid> select;
        QStringList problems;
    };
    std::optional<PendingTabset> m_Tabset;
    /// Expanded tabsets waiting for their missing-file prompts, which run after the context callback returns.
    struct ExpandedTabset
    {
        QString tabset;
        std::vector<TabsetItem> items;
    };
    std::vector<ExpandedTabset> m_ExpandedTabsets;
    bool m_TabsetPrompting = false;
    bool m_Busy = false;
};

} // namespace motrack
