#pragma once

#include "capture_dialog.hpp"
#include "workspace_context.hpp"

#include <QMainWindow>
#include <QPointer>

#include <functional>

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
    bool m_Busy = false;
};

} // namespace motrack
