#pragma once

#include "movie_worker.hpp"

#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QThreadPool>

namespace motrack
{

/// GUI-thread owned execution services shared by all views in one workspace. Destroy after views
/// cancel their jobs. Workers own snapshots; no worker calls into a view during shutdown.
class WorkspaceServices final : public QObject
{
    Q_OBJECT

  public:
    WorkspaceServices();
    ~WorkspaceServices() override;
    /// One active decode plus one replaceable pending request per document, in fair document order.
    /// Replacing one document's request never cancels another's request.
    void requestFrame(const MovieFrameRequest& request);
    /// Cancel and asynchronously release this document's decoder/cache; source ownership survives until then.
    void clearFrames(QUuid document);
    /// Borrowed pools; production callers must not change their limits. Analysis has two workers,
    /// saving one. Callers retain per-document cancellation/revision checks and bound submitted snapshots.
    [[nodiscard]] QThreadPool& analysisPool();
    [[nodiscard]] QThreadPool& savePool();

  Q_SIGNALS:
    void frameDecoded(const motrack::MovieFrameResult& result);

  private:
    void dispatchFrame();
    QThreadPool m_FramePool;
    QThreadPool m_AnalysisPool;
    QThreadPool m_SavePool;
    std::shared_ptr<MovieWorker> m_MovieWorker;
    QFutureWatcher<MovieFrameResult> m_FrameWatcher;
    bool m_FrameBusy = false;
    std::optional<MovieFrameRequest> m_ActiveFrame;
    QHash<QUuid, MovieFrameRequest> m_PendingFrames;
    QList<QUuid> m_FrameOrder;
    QSet<QUuid> m_ClearFrames;
    QSet<QUuid> m_FrameDocuments;
    bool m_HadFrames = false;
};

} // namespace motrack
