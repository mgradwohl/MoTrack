#pragma once

#include "workspace_services.hpp"
#include "workspace_session.hpp"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>

namespace motrack
{

/// Owner-thread bridge for retained records and their editing views. Records outlive views;
/// destruction cancels registry work before the shared execution owner can drain its pools.
class WorkspaceContext final : public QObject
{
    Q_OBJECT

  public:
    explicit WorkspaceContext(std::shared_ptr<WorkspaceServices> services = {});
    ~WorkspaceContext() override;
    /// Prepare at most two independent project inputs on shared workers. Busy returns no request ID.
    /// Cancellation retains the slot until its worker completes; other input/jobs remain unaffected.
    [[nodiscard]] std::optional<QUuid> openProject(const QString& path, WorkspaceActivation activation = WorkspaceActivation::SelectNew);
    [[nodiscard]] bool cancelOpen(QUuid request);
    [[nodiscard]] WorkspaceSession& session();
    [[nodiscard]] const std::shared_ptr<WorkspaceServices>& services() const;
    /// One experiment editor per record. Additional plot/table views use that editor's model.
    void attachView(QUuid document);
    void detachView(QUuid document);
    void notifySourceChanged(QUuid source, bool mediaBindings = false);

  Q_SIGNALS:
    void sourceChanged(QUuid source, bool mediaBindings);
    void jobsCancelled(QUuid document);
    void inputOpened(QUuid request, QUuid source, QList<QUuid> documents);
    void inputFailed(QUuid request, QString diagnostic);
    void inputCancelled(QUuid request);

  private:
    Q_SIGNAL void sourceChangeQueued(QUuid source, bool mediaBindings);
    Q_SIGNAL void jobsCancellationQueued(QUuid document);
    std::shared_ptr<WorkspaceServices> m_Services;
    WorkspaceSession m_Session;
    QSet<QUuid> m_Views;
    QHash<QUuid, std::shared_ptr<std::atomic_bool>> m_Opening;
};

} // namespace motrack
