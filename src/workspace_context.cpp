#include "workspace_context.hpp"

#include <utility>

namespace motrack
{

WorkspaceContext::WorkspaceContext(std::shared_ptr<WorkspaceServices> services)
    : m_Services(services ? std::move(services) : std::make_shared<WorkspaceServices>())
{
    connect(this, &WorkspaceContext::sourceChangeQueued, this, &WorkspaceContext::sourceChanged, Qt::QueuedConnection);
}

WorkspaceSession& WorkspaceContext::session()
{
    return m_Session;
}

const std::shared_ptr<WorkspaceServices>& WorkspaceContext::services() const
{
    return m_Services;
}

void WorkspaceContext::attachView(QUuid document)
{
    (void)m_Session.document(document);
    if (m_Views.contains(document))
        throw ProjectError("An editor already owns this document view.");
    m_Views.insert(document);
    m_Session.showDocument(document);
}

void WorkspaceContext::detachView(QUuid document)
{
    if (m_Views.remove(document))
    {
        m_Session.cancelJobs(document);
        m_Session.hideDocument(document);
        m_Services->clearFrames(document);
    }
}

void WorkspaceContext::notifySourceChanged(QUuid source, bool mediaBindings)
{
    // Deliver after the current editor operation returns; observers may close or create views.
    Q_EMIT sourceChangeQueued(source, mediaBindings);
}

} // namespace motrack
