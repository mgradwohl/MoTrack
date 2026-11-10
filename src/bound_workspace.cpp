#include "main_window.hpp"

#include <QAction>
#include <QDialog>
#include <QEventLoop>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <exception>
#include <utility>

namespace motrack
{
namespace
{

QDialog* saveProgress(QWidget* parent, const QString& title, const QString& message, const std::shared_ptr<std::atomic_bool>& cancelled)
{
    auto* progress = new QDialog(parent);
    progress->setObjectName("pairedSaveProgress");
    progress->setWindowTitle(title);
    progress->setWindowModality(Qt::WindowModal);
    auto* layout = new QVBoxLayout(progress);
    auto* label = new QLabel(message, progress);
    auto* cancel = new QPushButton(QCoreApplication::translate("MainWindow", "Cancel"), progress);
    cancel->setObjectName("cancelPairedSave");
    layout->addWidget(label);
    layout->addWidget(cancel);
    const auto stop = [progress, label, cancel, cancelled]
    {
        *cancelled = true;
        label->setText(QCoreApplication::translate("MainWindow", "Canceling save…"));
        cancel->setEnabled(false);
        progress->show();
    };
    QObject::connect(cancel, &QPushButton::clicked, progress, stop);
    QObject::connect(progress, &QDialog::rejected, progress, stop);
    progress->show();
    return progress;
}

} // namespace

MainWindow::MainWindow(std::shared_ptr<WorkspaceContext> workspace, QUuid document)
    : MainWindow(workspace ? workspace->services() : std::shared_ptr<WorkspaceServices>{})
{
    if (!workspace)
        throw ProjectError("A retained view requires a workspace.");
    workspace->attachView(document);
    m_Workspace = std::move(workspace);
    m_WorkspaceDocument = document;
    m_FrameDocument = document;
    for (auto* action : m_OpenActions)
        action->setEnabled(false);
    connect(m_Workspace.get(),
            &WorkspaceContext::sourceChanged,
            this,
            [this](QUuid source, bool mediaBindings)
            {
                if (source == m_Workspace->session().sourceId(m_WorkspaceDocument))
                {
                    setWindowModified(viewModified());
                    if (mediaBindings)
                        refreshImageMediaLocations();
                }
            });
    connect(m_Workspace.get(),
            &WorkspaceContext::jobsCancelled,
            this,
            [this](QUuid cancelledDocument)
            {
                if (cancelledDocument != m_WorkspaceDocument)
                    return;
                // Run identities survive frame/match waits. A delayed notice must not stop a newer run.
                if (m_TrackingJob && m_TrackingJob->cancelled->load())
                    stopAutoTracking();
                if (m_PlaybackJob && m_PlaybackJob->cancelled->load())
                    setPlaying(false);
            });
    m_MediaProjectPath = m_Workspace->session().mediaProjectPath(document);
    const auto& model = viewDocument();
    m_Sequence = model.movie ? std::nullopt : ImageSequence::open(m_MediaProjectPath, model.project.clip, model.mediaLocations);
    const auto& clip = model.project.clip;
    if (!m_Sequence && clip.mediaReferences.size() == 1 && clip.stepCount.value_or(0) > 0)
    {
        const auto path = workspaceMediaPath(model, m_MediaProjectPath, clip.mediaReferences.front());
        if (QFileInfo(path).isFile())
            m_MoviePath = path;
    }
    if (model.movieTimeline && matchesMovieTimeline(*model.movieTimeline,
                                                    m_MoviePath,
                                                    model.useDisplayOrientation ? VideoOrientation::Display : VideoOrientation::Encoded))
        m_MovieTimeline = model.movieTimeline;
    activateProject(viewSourcePath());
    if (!m_MoviePath.isEmpty() && !m_MovieTimeline)
        scheduleMovieIndex(m_MoviePath, false);
}

const WorkspaceDocument& MainWindow::viewDocument() const
{
    return m_Workspace ? m_Workspace->session().document(m_WorkspaceDocument) : m_Session.document;
}

LegacyProject& MainWindow::viewProject()
{
    return m_Workspace ? m_Workspace->session().project(m_WorkspaceDocument) : m_Session.document.project;
}

const LegacyProject& MainWindow::viewProject() const
{
    return viewDocument().project;
}

TrackEdits& MainWindow::viewHistory()
{
    return m_Workspace ? m_Workspace->session().history(m_WorkspaceDocument) : m_Session.history;
}

bool MainWindow::viewModified() const
{
    return m_Workspace ? m_Workspace->session().isModified(m_Workspace->session().sourceId(m_WorkspaceDocument)) : m_Session.isModified();
}

bool MainWindow::viewRequiresPairedSave() const
{
    return m_Workspace ? m_Workspace->session().requiresPairedSave(m_Workspace->session().sourceId(m_WorkspaceDocument))
                       : m_Session.requiresPairedSave();
}

QString MainWindow::viewSourcePath() const
{
    return m_Workspace ? m_Workspace->session().sourcePath(m_Workspace->session().sourceId(m_WorkspaceDocument)) : m_Session.sourcePath();
}

QString MainWindow::viewNativeSavePath() const
{
    return m_Workspace ? m_Workspace->session().nativeSavePath(m_Workspace->session().sourceId(m_WorkspaceDocument))
                       : m_Session.nativeSavePath();
}

QString MainWindow::viewStandardSavePath() const
{
    return m_Workspace ? m_Workspace->session().standardSavePath(m_Workspace->session().sourceId(m_WorkspaceDocument))
                       : m_Session.standardSavePath();
}

QString MainWindow::viewRecoveryPath() const
{
    return m_Workspace ? m_Workspace->session().recoveryPath(m_Workspace->session().sourceId(m_WorkspaceDocument))
                       : m_Session.recoveryPath();
}

DocumentSession::SaveCheckpoint MainWindow::viewCheckpoint() const
{
    return m_Workspace ? m_Workspace->session().documentCheckpoint(m_WorkspaceDocument) : m_Session.saveCheckpoint();
}

bool MainWindow::recoverBoundWorkspace()
{
    const auto workspace = m_Workspace; // Survives destruction of the view inside the nested event loop.
    auto pending = workspace->session().beginRecovery(m_WorkspaceDocument);
    if (!pending)
    {
        statusBar()->showMessage(tr("The workspace already has two pending saves. Wait for one to finish."));
        return false;
    }
    const auto request = std::make_shared<WorkspaceRecoveryRequest>(std::move(*pending));
    m_SaveCancelled = request->operation.cancelled;
    m_Saving = true;
    bool alive = true;
    QEventLoop loop;
    connect(this, &QObject::destroyed, &loop, [&alive] { alive = false; });
    auto* progress = saveProgress(this, tr("Saving project files"), tr("Saving the Tracker file and MoTrack companion…"), m_SaveCancelled);
    QFutureWatcher<PairedSaveResult> watcher;
    connect(&watcher, &QFutureWatcher<PairedSaveResult>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(m_SavePool, [request] { return recoverWorkspacePair(*request); }));
    loop.exec();
    const auto result = watcher.result();
    const bool cancelled = request->operation.cancelled->load();
    const auto outcome = workspace->session().completeRecovery(*request, result);
    // Every completion reaches its retained source, even after its view was closed or destroyed.
    workspace->notifySourceChanged(request->operation.source, true);
    if (!alive)
        return false;
    delete progress;
    finishSaving();
    setWindowModified(viewModified());
    if (outcome != SaveOutcome::Recovered)
    {
        statusBar()->showMessage(tr("Could not finish the interrupted save: %1").arg(result.diagnostic));
        return false;
    }
    return !cancelled;
}

bool MainWindow::saveBoundWorkspace(const QString& path, bool paired, bool recoverPending)
{
    if (m_Saving)
        return false;
    const auto suffix =
        paired ? (viewDocument().archivePayload ? QStringLiteral(".trz") : QStringLiteral(".trk")) : QStringLiteral(".motrack");
    if (viewSourcePath().isEmpty() || !path.endsWith(suffix, Qt::CaseInsensitive) || (paired && !viewDocument().trackerSource))
    {
        statusBar()->showMessage(paired ? tr("Choose a %1 filename for an imported Tracker project.").arg(suffix)
                                        : tr("Choose a project filename ending in .motrack."));
        return false;
    }
    const bool recovering = paired && !viewRecoveryPath().isEmpty();
    if (recovering)
    {
        if (!recoverPending)
        {
            statusBar()->showMessage(tr("Finish the interrupted save before saving this project again."));
            return false;
        }
        if (!recoverBoundWorkspace())
            return false;
    }
    const auto workspace = m_Workspace;
    auto pending = workspace->session().beginSave(m_WorkspaceDocument);
    if (!pending)
    {
        statusBar()->showMessage(tr("The workspace already has two pending saves. Wait for one to finish."));
        return false;
    }
    const auto request = std::make_shared<WorkspaceSaveRequest>(std::move(*pending));
    m_SaveCancelled = request->operation.cancelled;
    m_Saving = true;
    bool alive = true;
    QEventLoop loop;
    connect(this, &QObject::destroyed, &loop, [&alive] { alive = false; });
    auto* progress = saveProgress(this,
                                  paired ? tr("Saving project files") : tr("Saving project…"),
                                  paired ? tr("Saving the Tracker file and MoTrack companion…") : tr("Saving project…"),
                                  m_SaveCancelled);
    struct Result
    {
        NativeSaveResult native;
        PairedSaveResult pair;
        QString diagnostic;
        std::optional<SaveDestinationConflict> conflict;
    };
    QFutureWatcher<Result> watcher;
    connect(&watcher, &QFutureWatcher<Result>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(m_SavePool,
                                        [request, path, paired]
                                        {
                                            Result result;
                                            try
                                            {
                                                if (paired)
                                                    result.pair = writeWorkspacePair(*request, path);
                                                else
                                                    result.native = writeWorkspaceNative(*request, path);
                                            }
                                            catch (const SaveDestinationError& error)
                                            {
                                                result.conflict = error.conflict();
                                            }
                                            catch (const std::exception& error)
                                            {
                                                result.diagnostic = QString::fromUtf8(error.what());
                                            }
                                            return result;
                                        }));
    loop.exec();
    const auto result = watcher.result();
    const bool cancelled = request->operation.cancelled->load();
    const auto outcome = paired ? workspace->session().completePairedSave(*request, result.pair)
                                : workspace->session().completeSave(*request, path, result.native);
    workspace->notifySourceChanged(request->operation.source, true);
    if (!alive)
        return false;
    delete progress;
    finishSaving();
    setWindowModified(viewModified());
    if (outcome == SaveOutcome::Partial)
    {
        statusBar()->showMessage(tr("An interrupted save needs recovery before continuing."));
        return false;
    }
    if (outcome == SaveOutcome::Failed || outcome == SaveOutcome::Replaced || outcome == SaveOutcome::Superseded)
    {
        statusBar()->showMessage(
            result.conflict == SaveDestinationConflict::PendingRecovery
                ? tr("An interrupted paired save needs recovery. Use Save Tracker project for this destination, or save a copy elsewhere.")
            : result.conflict == SaveDestinationConflict::Busy
                ? tr("Another save is using this destination. Wait for it to finish and retry, or save a copy elsewhere.")
            : cancelled ? (recovering ? tr("Interrupted save recovered. Saving the current edits was canceled.")
                                      : tr("Save canceled. Existing files were kept."))
            : paired    ? tr("Could not save the Tracker project: %1").arg(result.pair.diagnostic + result.diagnostic)
                        : tr("Could not save this project."));
        return false;
    }
    setWindowTitle(tr("%1[*] — MoTrack").arg(QFileInfo(path).fileName()));
    if (outcome == SaveOutcome::Stale)
    {
        statusBar()->showMessage(tr("Project changed while saving. Save again to keep the latest edits."));
        return false;
    }
    statusBar()->showMessage(paired           ? tr("Tracker project and MoTrack companion saved.")
                             : viewModified() ? tr("MoTrack copy saved. Save the Tracker project to keep both files current.")
                                              : tr("Project saved."));
    return true;
}

} // namespace motrack
