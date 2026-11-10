#include "workspace_services.hpp"

#include <QtConcurrent>

#include <utility>

namespace motrack
{
namespace
{

void cancel(const MovieFrameRequest& request)
{
    if (request.token.cancelled)
        request.token.cancelled->store(true);
}

} // namespace

WorkspaceServices::WorkspaceServices() : m_MovieWorker(std::make_shared<MovieWorker>())
{
    m_FramePool.setMaxThreadCount(1);
    m_FramePool.setExpiryTimeout(-1); // Live decoders stay on the same thread until explicitly closed.
    m_AnalysisPool.setMaxThreadCount(2);
    m_SavePool.setMaxThreadCount(1);
    connect(&m_FrameWatcher,
            &QFutureWatcher<MovieFrameResult>::finished,
            this,
            [this]
            {
                const auto result = m_FrameWatcher.result();
                m_FrameBusy = false;
                m_ActiveFrame.reset();
                // The synchronous destruction notification protects this owner-thread borrow.
                // Disconnect the observer before its stack flag leaves scope.
                bool alive = true;
                QObject lifetime;
                connect(this, &QObject::destroyed, &lifetime, [&alive] { alive = false; });
                if (result.request.token.cancelled && !result.request.token.cancelled->load())
                    Q_EMIT frameDecoded(result);
                if (alive)
                    dispatchFrame();
            });
}

WorkspaceServices::~WorkspaceServices()
{
    if (m_ActiveFrame)
        cancel(*m_ActiveFrame);
    for (const auto& request : std::as_const(m_PendingFrames))
        cancel(request);
    // Close decoder handles on their worker thread before the last archive owner can be destroyed.
    if (m_HadFrames)
        (void)QtConcurrent::run(&m_FramePool, [worker = m_MovieWorker] { worker->clearAll(); });
    m_FramePool.waitForDone();
    m_AnalysisPool.waitForDone();
    m_SavePool.waitForDone();
}

QThreadPool& WorkspaceServices::analysisPool()
{
    return m_AnalysisPool;
}

QThreadPool& WorkspaceServices::savePool()
{
    return m_SavePool;
}

void WorkspaceServices::requestFrame(const MovieFrameRequest& request)
{
    if (request.token.document.isNull() || request.token.kind != WorkspaceJob::Frame || !request.token.cancelled ||
        request.token.cancelled->load())
        return;
    const auto id = request.token.document;
    m_HadFrames = true;
    m_FrameDocuments.insert(id);
    if (m_ActiveFrame && m_ActiveFrame->token.document == id)
        cancel(*m_ActiveFrame);
    if (auto pending = m_PendingFrames.find(id); pending != m_PendingFrames.end())
        cancel(pending.value());
    else
        m_FrameOrder.push_back(id);
    m_PendingFrames.insert(id, request);
    dispatchFrame();
}

void WorkspaceServices::clearFrames(QUuid document)
{
    if (!m_FrameDocuments.remove(document))
        return;
    if (m_ActiveFrame && m_ActiveFrame->token.document == document)
        cancel(*m_ActiveFrame);
    if (const auto pending = m_PendingFrames.constFind(document); pending != m_PendingFrames.cend())
        cancel(pending.value());
    m_PendingFrames.remove(document);
    m_FrameOrder.removeAll(document);
    m_ClearFrames.insert(document);
    dispatchFrame();
}

void WorkspaceServices::dispatchFrame()
{
    if (m_FrameBusy || (m_FrameOrder.isEmpty() && m_ClearFrames.isEmpty()))
        return;
    std::optional<MovieFrameRequest> request;
    if (!m_FrameOrder.isEmpty())
        request = m_PendingFrames.take(m_FrameOrder.takeFirst());
    auto clears = std::exchange(m_ClearFrames, {});
    m_ActiveFrame = request;
    m_FrameBusy = true;
    m_FrameWatcher.setFuture(QtConcurrent::run(&m_FramePool,
                                               [worker = m_MovieWorker, request = std::move(request), clears = std::move(clears)]() mutable
                                               {
                                                   for (const auto id : clears)
                                                       worker->clear(id);
                                                   return request ? worker->readFrame(std::move(*request)) : MovieFrameResult{};
                                               }));
}

} // namespace motrack
