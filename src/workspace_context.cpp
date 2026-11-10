#include "workspace_context.hpp"

#include "workspace_input.hpp"

#include <QFutureWatcher>
#include <QtConcurrentRun>

#include <exception>
#include <utility>

namespace motrack
{

WorkspaceContext::WorkspaceContext(std::shared_ptr<WorkspaceServices> services)
    : m_Services(services ? std::move(services) : std::make_shared<WorkspaceServices>()),
      m_Session([this](QUuid document) { Q_EMIT jobsCancellationQueued(document); })
{
    connect(this, &WorkspaceContext::sourceChangeQueued, this, &WorkspaceContext::sourceChanged, Qt::QueuedConnection);
    connect(this, &WorkspaceContext::jobsCancellationQueued, this, &WorkspaceContext::jobsCancelled, Qt::QueuedConnection);
}

WorkspaceContext::~WorkspaceContext()
{
    for (const auto& cancelled : m_Opening)
        cancelled->store(true);
}

std::optional<QUuid> WorkspaceContext::openProject(const QString& path, WorkspaceActivation activation)
{
    constexpr qsizetype MAX_PENDING_INPUTS = 2;
    if (m_Opening.size() >= MAX_PENDING_INPUTS)
        return std::nullopt;
    const auto request = QUuid::createUuid();
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    const auto input = std::make_shared<PreparedWorkspaceInput>();
    auto* watcher = new QFutureWatcher<QString>(this);
    m_Opening.insert(request, cancelled);
    connect(watcher,
            &QFutureWatcher<QString>::finished,
            this,
            [this, watcher, request, cancelled, activation, input]
            {
                auto diagnostic = watcher->result();
                watcher->deleteLater();
                m_Opening.remove(request);
                if (cancelled->load())
                {
                    Q_EMIT inputCancelled(request);
                    return;
                }
                std::optional<WorkspaceSession::OpenedSource> opened;
                try
                {
                    if (diagnostic.isEmpty())
                        opened = adoptWorkspaceInput(m_Session, *input, activation);
                }
                catch (const std::exception& error)
                {
                    diagnostic = QString::fromUtf8(error.what());
                }
                // Notifications are last: observers may destroy this context or immediately open another source.
                if (opened)
                {
                    QList<QUuid> documents;
                    for (const auto id : opened->documents)
                        documents.push_back(id);
                    Q_EMIT inputOpened(request, opened->source, documents);
                }
                else
                    Q_EMIT inputFailed(request, diagnostic);
            });
    try
    {
        watcher->setFuture(QtConcurrent::run(&m_Services->analysisPool(),
                                             [path, cancelled, input]
                                             {
                                                 try
                                                 {
                                                     *input = prepareWorkspaceInput(path, cancelled.get());
                                                     return QString{};
                                                 }
                                                 catch (const std::exception& error)
                                                 {
                                                     return QString::fromUtf8(error.what());
                                                 }
                                             }));
    }
    catch (...)
    {
        cancelled->store(true);
        m_Opening.remove(request);
        delete watcher;
        throw;
    }
    return request;
}

bool WorkspaceContext::cancelOpen(QUuid request)
{
    const auto found = m_Opening.constFind(request);
    if (found == m_Opening.cend())
        return false;
    (*found)->store(true);
    return true;
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
