#include "workspace_window.hpp"

#include "local_media_path.hpp"
#include "main_window.hpp"
#include "tracker_archive.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

namespace motrack
{
namespace
{

QStringList saveDestinations(const WorkspaceSession& session, QUuid source)
{
    const auto standard = localProjectPath(session.standardSavePath(source));
    if (!standard.isEmpty())
    {
        const QFileInfo file(standard);
        return {standard, file.dir().filePath(file.completeBaseName() + ".motrack")};
    }
    const auto native = localProjectPath(session.nativeSavePath(source));
    return native.isEmpty() ? QStringList{} : QStringList{native};
}

QMessageBox::StandardButton askQuestion(QWidget* parent,
                                        const QString& title,
                                        const QString& message,
                                        QMessageBox::StandardButtons buttons,
                                        QMessageBox::StandardButton defaultButton,
                                        const char* name)
{
    bool ownerAlive = true;
    bool dialogAlive = true;
    QObject lifetime;
    QObject::connect(parent, &QObject::destroyed, &lifetime, [&] { ownerAlive = false; });
    auto* dialog = new QMessageBox(QMessageBox::Question, title, message, buttons, parent);
    dialog->setObjectName(name);
    dialog->setDefaultButton(defaultButton);
    QObject::connect(dialog, &QObject::destroyed, &lifetime, [&] { dialogAlive = false; });
    (void)dialog->exec();
    const auto choice = ownerAlive && dialogAlive ? dialog->standardButton(dialog->clickedButton()) : QMessageBox::Cancel;
    if (dialogAlive)
        delete dialog;
    return choice;
}

/// Insert into a set and report whether the value was new.
[[nodiscard]] bool insertNew(QSet<QString>& set, const QString& value)
{
    const auto before = set.size();
    set.insert(value);
    return set.size() != before;
}

[[nodiscard]] bool isProjectFile(const QString& path)
{
    const auto suffix = QFileInfo(path).suffix();
    return std::ranges::any_of(std::array{"trk", "trz", "motrack"},
                               [&](const char* known) { return suffix.compare(QLatin1StringView(known), Qt::CaseInsensitive) == 0; });
}

/// Window-modal summary without a nested event loop; tests and users dismiss it independently.
void showProblems(QWidget* parent, const QString& text, const QStringList& problems, const char* name)
{
    constexpr qsizetype MAX_LISTED = 20;
    auto listed = problems.first(std::min(problems.size(), MAX_LISTED));
    if (const auto more = static_cast<int>(problems.size() - MAX_LISTED); more > 0)
        listed.push_back(QCoreApplication::translate("motrack::WorkspaceWindow", "…and %n more.", nullptr, more));
    auto* box = new QMessageBox(
        QMessageBox::Warning, QCoreApplication::translate("motrack::WorkspaceWindow", "Tabset"), text, QMessageBox::Ok, parent);
    box->setObjectName(name);
    box->setInformativeText(listed.join('\n'));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

} // namespace

WorkspaceWindow::WorkspaceWindow(std::shared_ptr<WorkspaceContext> context)
    : m_Context(context ? std::move(context) : std::make_shared<WorkspaceContext>())
{
    setObjectName("workspaceWindow");
    resize(1100, 760);
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    m_Empty = new QLabel(tr("Open a project, video, or image sequence to begin."), central);
    m_Empty->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_Empty, 1);
    m_Tabs = new QTabWidget(central);
    m_Tabs->setObjectName("experimentTabs");
    m_Tabs->setMovable(true);
    m_Tabs->setTabsClosable(true);
    layout->addWidget(m_Tabs, 1);
    m_Pending = new QListWidget(central);
    m_Pending->setObjectName("pendingWorkspaceInputs");
    m_Pending->setMaximumHeight(80);
    m_Pending->hide();
    layout->addWidget(m_Pending);
    auto* cancelOpen = new QPushButton(tr("Cancel selected open"), central);
    cancelOpen->setObjectName("cancelWorkspaceOpen");
    cancelOpen->hide();
    layout->addWidget(cancelOpen);
    setCentralWidget(central);
    connect(cancelOpen,
            &QPushButton::clicked,
            this,
            [this]
            {
                if (auto* item = m_Pending->currentItem())
                    if (m_Context->cancelOpen(item->data(Qt::UserRole).toUuid()))
                        item->setText(tr("Canceling %1…").arg(item->data(Qt::UserRole + 1).toString()));
            });
    connect(m_Pending->model(), &QAbstractItemModel::rowsInserted, cancelOpen, [cancelOpen] { cancelOpen->show(); });
    connect(m_Pending->model(),
            &QAbstractItemModel::rowsRemoved,
            cancelOpen,
            [this, cancelOpen] { cancelOpen->setVisible(m_Pending->count() > 0); });

    auto* openProjectAction = new QAction(tr("&Open project…"), this);
    openProjectAction->setObjectName("workspaceOpenProject");
    openProjectAction->setShortcut(QKeySequence::Open);
    auto* openMovieAction = new QAction(tr("Open &video…"), this);
    openMovieAction->setObjectName("workspaceOpenMovie");
    auto* openImagesAction = new QAction(tr("Open &images…"), this);
    openImagesAction->setObjectName("workspaceOpenImages");
    auto* captureAction = new QAction(tr("Capture from &camera…"), this);
    captureAction->setObjectName("workspaceCaptureCamera");
    captureAction->setToolTip(tr("Record a clip with a camera and open it as a new experiment."));
    m_OpenActions = {openProjectAction, openMovieAction, openImagesAction, captureAction};
    m_CameraSources = []
    {
        return std::make_unique<QtCameraSource>();
    };
    connect(captureAction, &QAction::triggered, this, [this] { (void)captureFromCamera(); });
    connect(openProjectAction,
            &QAction::triggered,
            this,
            [this]
            {
                bool alive = true;
                QObject lifetime;
                connect(this, &QObject::destroyed, &lifetime, [&] { alive = false; });
                const auto path =
                    QFileDialog::getOpenFileName(this, tr("Open project"), {}, tr("MoTrack and Tracker projects (*.motrack *.trk *.trz)"));
                if (alive && !path.isEmpty())
                    (void)openProject(path);
            });
    connect(openMovieAction,
            &QAction::triggered,
            this,
            [this]
            {
                bool alive = true;
                QObject lifetime;
                connect(this, &QObject::destroyed, &lifetime, [&] { alive = false; });
                const auto path = QFileDialog::getOpenFileName(this, tr("Open video"));
                if (alive && !path.isEmpty())
                    (void)openMovie(path);
            });
    connect(openImagesAction,
            &QAction::triggered,
            this,
            [this]
            {
                bool alive = true;
                QObject lifetime;
                connect(this, &QObject::destroyed, &lifetime, [&] { alive = false; });
                QStringList patterns;
                for (const auto& format : QImageReader::supportedImageFormats())
                    patterns.push_back("*." + QString::fromLatin1(format));
                auto paths = QFileDialog::getOpenFileNames(this, tr("Open image sequence"), {}, tr("Images (%1)").arg(patterns.join(' ')));
                if (!alive || paths.isEmpty())
                    return;
                bool accepted = false;
                const auto rate = QInputDialog::getDouble(
                    this, tr("Image sequence timing"), tr("Frames per second (0 for unknown)"), 30, 0, 1000000, 6, &accepted);
                if (!alive || !accepted)
                    return;
                QCollator collator;
                collator.setNumericMode(true);
                std::ranges::sort(paths,
                                  [&collator](const auto& left, const auto& right)
                                  {
                                      const auto order = collator.compare(left, right);
                                      return order < 0 || (order == 0 && left < right);
                                  });
                (void)openImages(paths, rate);
            });
    m_SaveAll = new QAction(tr("Save &all projects"), this);
    m_SaveAll->setObjectName("saveAllProjects");
    m_SaveTabset = new QAction(tr("Save &tabset as…"), this);
    m_SaveTabset->setObjectName("saveTrackerTabset");
    m_SaveTabset->setToolTip(tr("Save a Tracker tabset that reopens all open projects together."));
    connect(m_SaveTabset, &QAction::triggered, this, [this] { (void)saveTabset(); });
    m_HideView = new QAction(tr("&Hide experiment view"), this);
    m_HideView->setObjectName("hideExperimentView");
    m_HideView->setShortcut(QKeySequence::Close);
    m_Revert = new QAction(tr("&Revert experiment…"), this);
    m_Revert->setObjectName("revertExperiment");
    m_CloseSource = new QAction(tr("Close &project…"), this);
    m_CloseSource->setObjectName("closeWorkspaceProject");
    m_CloseSource->setToolTip(tr("Close all experiments belonging to this project, including hidden views."));
    m_Quit = new QAction(tr("&Quit"), this);
    m_Quit->setShortcut(QKeySequence::Quit);
    connect(m_SaveAll, &QAction::triggered, this, [this] { (void)saveAll(); });
    connect(m_Revert,
            &QAction::triggered,
            this,
            [this]
            {
                if (const auto document = currentDocument())
                    (void)revertDocument(*document);
            });
    connect(m_HideView,
            &QAction::triggered,
            this,
            [this]
            {
                if (const auto document = currentDocument())
                    (void)hideDocument(*document);
            });
    connect(m_CloseSource,
            &QAction::triggered,
            this,
            [this]
            {
                if (const auto document = currentDocument())
                    (void)closeSource(m_Context->session().sourceId(*document));
            });
    connect(m_Quit, &QAction::triggered, this, &QWidget::close);
    m_EmptyFileMenu = new QMenu(tr("&File"), this);
    m_EmptyFileMenu->addActions(m_OpenActions);
    m_EmptyFileMenu->addSeparator();
    m_EmptyFileMenu->addActions({m_SaveAll, m_SaveTabset, m_Revert, m_HideView, m_CloseSource, m_Quit});
    m_ExperimentsMenu = new QMenu(tr("&Experiments"), this);
    m_ExperimentsMenu->setObjectName("workspaceExperimentsMenu");
    m_ExperimentsMenu->setToolTipsVisible(true);
    connect(m_ExperimentsMenu, &QMenu::aboutToShow, this, &WorkspaceWindow::rebuildExperimentsMenu);
    auto* toolbar = addToolBar(tr("Workspace"));
    toolbar->addActions(m_OpenActions);
    toolbar->addAction(m_SaveAll);
    connect(m_Tabs,
            &QTabWidget::currentChanged,
            this,
            [this]
            {
                if (const auto document = currentDocument())
                    (void)showDocument(*document);
                refreshMenus();
            });
    connect(m_Tabs,
            &QTabWidget::tabCloseRequested,
            this,
            [this](int index)
            {
                const auto document = m_Pages.key(m_Tabs->widget(index));
                if (!document.isNull())
                    (void)hideDocument(document);
            });
    connect(m_Tabs->tabBar(),
            &QTabBar::tabMoved,
            this,
            [this](int, int to)
            {
                const auto document = m_Pages.key(m_Tabs->widget(to));
                if (!document.isNull())
                    m_Context->session().moveDocument(document, static_cast<std::size_t>(to));
            });
    connect(m_Context.get(), &WorkspaceContext::sourceChanged, this, [this] { refreshLabels(); });
    connect(m_Context.get(),
            &WorkspaceContext::inputOpened,
            this,
            [this](QUuid request, QUuid source, const QList<QUuid>& documents, const QList<UnreadableProject>& unreadable)
            {
                finishOpen(request);
                // Kept for the source's lifetime so the Experiments menu can list them, as Tracker keeps a failed tab.
                if (!unreadable.isEmpty())
                    m_Unreadable.insert(source, unreadable);
                addDocuments(documents);
                reportArchiveLimits(documents, unreadable);
                completeTabsetInput(request, documents, {});
            });
    connect(m_Context.get(),
            &WorkspaceContext::tabsetOpened,
            this,
            [this](QUuid request, const QString& tabset, const std::vector<TabsetItem>& items)
            {
                finishOpen(request);
                if (m_Tabset && m_Tabset->request == request)
                {
                    // A located file was itself a tabset: open its files in place.
                    expandInPlace(*m_Tabset, items);
                    completeTabsetInput(request, {}, {});
                    return;
                }
                // One tabset opens at a time; its prompts run after the context's completion handler has returned.
                m_ExpandedTabsets.push_back({.tabset = tabset, .items = items});
                QTimer::singleShot(0, this, &WorkspaceWindow::beginQueuedTabset);
                if (m_Tabset && !m_Tabset->request) // This open may have used the slot the active tabset was waiting for.
                    QTimer::singleShot(0, this, &WorkspaceWindow::continueTabset);
            });
    connect(m_Context.get(),
            &WorkspaceContext::inputFailed,
            this,
            [this](QUuid request, const QString& diagnostic)
            {
                finishOpen(request);
                statusBar()->showMessage(tr("Could not open: %1").arg(diagnostic));
                completeTabsetInput(request, {}, diagnostic);
            });
    connect(m_Context.get(),
            &WorkspaceContext::inputCancelled,
            this,
            [this](QUuid request)
            {
                finishOpen(request);
                statusBar()->showMessage(tr("Opening canceled."));
                completeTabsetInput(request, {}, {}, true);
            });
    QList<QUuid> visible;
    for (const auto document : m_Context->session().tabs())
        visible.push_back(document);
    addDocuments(visible);
}

WorkspaceWindow::~WorkspaceWindow()
{
    // Stops a recording and removes its temporary file before the window's other children go.
    delete m_Capture;
    disconnect(m_Tabs, nullptr, this, nullptr);
    disconnect(m_Tabs->tabBar(), nullptr, this, nullptr);
    disconnect(m_Context.get(), nullptr, this, nullptr);
    for (auto request = m_Opening.cbegin(); request != m_Opening.cend(); ++request)
        (void)m_Context->cancelOpen(request.key());
    // QObject deletes children after C++ members. Destroy borrowed editors while the registry/map
    // still exist, and prevent their destruction callbacks from scheduling work on this owner.
    menuBar()->clear();
    for (auto* editor : std::as_const(m_Views))
    {
        disconnect(editor, nullptr, this, nullptr);
        delete editor;
    }
    m_Views.clear();
    for (auto page = m_Pages.cbegin(); page != m_Pages.cend(); ++page)
        m_Context->session().hideDocument(page.key());
}

const std::shared_ptr<WorkspaceContext>& WorkspaceWindow::context() const
{
    return m_Context;
}

std::optional<QUuid> WorkspaceWindow::trackOpen(std::optional<QUuid> request, const QString& label)
{
    if (!request)
    {
        statusBar()->showMessage(tr("Two files are already opening. Wait for one to finish or cancel an open."));
        return std::nullopt;
    }
    auto* item = new QListWidgetItem(tr("Opening %1…").arg(label), m_Pending);
    item->setData(Qt::UserRole, *request);
    item->setData(Qt::UserRole + 1, label);
    m_Opening.insert(*request, item);
    m_Pending->setCurrentItem(item);
    m_Pending->show();
    return request;
}

std::optional<QUuid> WorkspaceWindow::openProject(const QString& path)
{
    return trackOpen(m_Context->openProject(path), QFileInfo(path).fileName());
}

std::optional<QUuid> WorkspaceWindow::openMovie(const QString& path)
{
    return trackOpen(m_Context->openMovie(path), QFileInfo(path).fileName());
}

void WorkspaceWindow::setCameraSourceFactory(CameraSourceFactory factory)
{
    m_CameraSources = std::move(factory);
}

CaptureDialog* WorkspaceWindow::captureFromCamera()
{
    if (m_Capture)
    {
        m_Capture->raise();
        m_Capture->activateWindow();
        return m_Capture;
    }
    if (!m_CameraSources)
        return nullptr;
    auto* dialog = new CaptureDialog(m_CameraSources(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog,
            &CaptureDialog::recordingSaved,
            this,
            [this](const QString& path)
            {
                const auto name = QDir::toNativeSeparators(path);
                if (openMovie(path))
                    statusBar()->showMessage(tr("Recording saved as %1. Opening it as a new experiment…").arg(name));
                else
                    statusBar()->showMessage(
                        tr("Recording saved as %1. Open it with Open video once the current files finish opening.").arg(name));
            });
    m_Capture = dialog;
    // Not modal: the workspace stays usable, and macOS shows a window rather than a sheet.
    dialog->show();
    return dialog;
}

std::optional<QUuid> WorkspaceWindow::openImages(const QStringList& paths, double frameRate)
{
    return trackOpen(m_Context->openImages(paths, frameRate), tr("%n images", nullptr, static_cast<int>(paths.size())));
}

void WorkspaceWindow::finishOpen(QUuid request)
{
    delete m_Opening.take(request);
    m_Pending->setVisible(m_Pending->count() > 0);
}

std::optional<QUuid> WorkspaceWindow::requestInput(const QString& path, WorkspaceActivation activation)
{
    if (isProjectFile(path))
        return m_Context->openProject(path, activation);
    // The worker decides between a still image and a movie; animated images keep their frames.
    return m_Context->openMediaFile(path, activation);
}

void WorkspaceWindow::beginQueuedTabset()
{
    // A queued tabset starts once the active one, including its prompts' nested event loops, has finished.
    if (m_ExpandedTabsets.empty() || m_Tabset || m_TabsetPrompting)
        return;
    auto next = std::move(m_ExpandedTabsets.front());
    m_ExpandedTabsets.erase(m_ExpandedTabsets.begin());
    beginTabset(next.tabset, next.items);
}

std::optional<QString> WorkspaceWindow::describeTabsetProblem(const TabsetItem& item) const
{
    const auto listed = QString::fromStdString(item.entry.relative);
    switch (item.status)
    {
    case TabsetItemStatus::Resolved:
    case TabsetItemStatus::Duplicate: // Tracker lists an archive once per experiment tab; open each file once silently.
        return std::nullopt;
    case TabsetItemStatus::Missing:
        return tr("%1: not found.").arg(listed);
    case TabsetItemStatus::Cycle:
        return tr("%1: a tabset that includes itself; skipped.").arg(listed);
    case TabsetItemStatus::Unreadable:
        return tr("%1: could not be read (%2).").arg(listed, QString::fromStdString(item.diagnostic));
    case TabsetItemStatus::TooDeep:
        return tr("%1: tabsets are nested too deeply; skipped.").arg(listed);
    case TabsetItemStatus::TooMany:
        return tr("Only the first %n files were opened.", nullptr, static_cast<int>(MAX_TABSET_ENTRIES));
    }
    return std::nullopt;
}

void WorkspaceWindow::beginTabset(const QString& tabset, const std::vector<TabsetItem>& items)
{
    bool alive = true;
    QObject lifetime;
    connect(this, &QObject::destroyed, &lifetime, [&] { alive = false; });
    m_TabsetPrompting = true;
    const auto prompting = qScopeGuard(
        [&]
        {
            if (alive)
                m_TabsetPrompting = false;
        });
    m_LastTabset = tabset;
    OrderedOpens pending;
    pending.tabset = tabset;
    // Every resolved file is canonical; a located file must not duplicate one of them or another located file.
    QSet<QString> resolved;
    for (const auto& item : items)
        if (item.status == TabsetItemStatus::Resolved)
            resolved.insert(tabsetIdentityKey(item.file));
    for (const auto& item : items)
    {
        const auto listed = QString::fromStdString(item.entry.relative);
        if (item.status == TabsetItemStatus::Resolved)
        {
            pending.files.push_back(item.file);
            pending.listed.insert(tabsetIdentityKey(item.file));
        }
        else if (item.status != TabsetItemStatus::Missing)
        {
            if (auto problem = describeTabsetProblem(item))
                pending.problems.push_back(*problem);
        }
        else
        {
            const auto answer =
                askQuestion(this,
                            tr("Tabset file not found"),
                            tr("“%1”, listed in %2, was not found. Locate it?").arg(listed, QFileInfo(item.tabset).fileName()),
                            QMessageBox::Yes | QMessageBox::No,
                            QMessageBox::Yes,
                            "workspaceTabsetMissing");
            if (!alive)
                return;
            const auto located =
                answer == QMessageBox::Yes
                    ? QFileDialog::getOpenFileName(this, tr("Locate %1").arg(QFileInfo(listed).fileName()), QFileInfo(item.tabset).path())
                    : QString{};
            if (!alive)
                return;
            const auto canonical = located.isEmpty() ? QString{} : QFileInfo(located).canonicalFilePath();
            const auto key = canonical.isEmpty() ? QString{} : tabsetIdentityKey(canonical);
            if (key.isEmpty())
                pending.problems.push_back(describeTabsetProblem(item).value_or(listed));
            else if (!resolved.contains(key) && insertNew(pending.listed, key))
                pending.files.push_back(canonical);
        }
    }
    m_Tabset = std::move(pending);
    continueTabset();
}

void WorkspaceWindow::expandInPlace(OrderedOpens& queue, const std::vector<TabsetItem>& items) const
{
    QStringList nested;
    for (const auto& item : items)
        if (item.status == TabsetItemStatus::Resolved)
        {
            // Skip files this queue already listed or opened, including earlier tabs.
            if (insertNew(queue.listed, tabsetIdentityKey(item.file)))
                nested.push_back(item.file);
        }
        else if (auto problem = describeTabsetProblem(item))
            queue.problems.push_back(*problem);
    queue.files = nested + queue.files;
    queue.requestIsProject = false;
}

void WorkspaceWindow::continueTabset()
{
    if (!m_Tabset || m_Tabset->request)
        return;
    if (auto& pending = *m_Tabset; !pending.files.isEmpty())
    {
        // Starting an open only queues a worker; no completion can reset the pending tabset here.
        const auto file = pending.files.front();
        const auto request = requestInput(file, WorkspaceActivation::KeepCurrent);
        if (!request)
            return; // Both input slots are busy; the next completion resumes the tabset in order.
        pending.files.removeFirst();
        pending.request = request;
        pending.current = QFileInfo(file).fileName();
        pending.requestIsProject = isProjectFile(file);
        (void)trackOpen(request, pending.current);
        return;
    }
    auto finished = std::move(*m_Tabset);
    m_Tabset.reset();
    // Tracker selects the first listed experiment file rather than a video.
    if (finished.select && m_Pages.contains(*finished.select))
        (void)showDocument(*finished.select);
    const auto name = QFileInfo(finished.tabset).fileName();
    statusBar()->showMessage(tr("Opened tabset %1.").arg(name));
    if (!finished.problems.isEmpty() || !finished.notices.isEmpty())
        showProblems(this,
                     finished.problems.isEmpty() ? tr("Some projects listed in %1 cannot be saved as Tracker files.").arg(name)
                                                 : tr("Some files listed in %1 were not opened.").arg(name),
                     finished.problems + finished.notices,
                     "workspaceTabsetProblems");
    QTimer::singleShot(0, this, &WorkspaceWindow::beginQueuedTabset);
    Q_EMIT tabsetFinished(finished.tabset);
}

void WorkspaceWindow::completeTabsetInput(QUuid request, const QList<QUuid>& documents, const QString& problem, bool cancelled)
{
    if (!m_Tabset)
        return;
    if (m_Tabset->request != request)
    {
        if (!m_Tabset->request) // Another input freed a slot the tabset was waiting for.
            QTimer::singleShot(0, this, &WorkspaceWindow::continueTabset);
        return;
    }
    m_Tabset->request.reset();
    if (m_Tabset->requestIsProject && !m_Tabset->select && !documents.isEmpty())
        m_Tabset->select = documents.front();
    // B1's native-file recovery: stale links to the Tracker source were reset, so the experiment opens but cannot
    // be exported. Its view shows the same note once it is selected.
    const auto& session = m_Context->session();
    if (std::ranges::any_of(documents, [&](QUuid document) { return session.document(document).trackerBindingsReset; }))
        m_Tabset->notices.push_back(tr("%1: %2").arg(m_Tabset->current,
                                                     QCoreApplication::translate("motrack::MainWindow",
                                                                                 "Tracker export unavailable: the links to this project's "
                                                                                 "Tracker tracks could not be restored. Measurements are "
                                                                                 "unchanged.")));
    if (!problem.isEmpty())
        m_Tabset->problems.push_back(tr("%1: %2").arg(m_Tabset->current, problem));
    if (cancelled)
    {
        m_Tabset->problems.push_back(tr("Opening canceled; the remaining tabset files were not opened."));
        m_Tabset->files.clear();
    }
    QTimer::singleShot(0, this, &WorkspaceWindow::continueTabset);
}

std::optional<QString> WorkspaceWindow::tabsetReference(QUuid source, QStringList& problems, bool& cancelled)
{
    auto& session = m_Context->session();
    const auto name = QFileInfo(session.sourcePath(source)).fileName();
    const auto standard = [&]
    {
        return localProjectPath(session.standardSavePath(source));
    };
    if (!session.nativeSavePath(source).isEmpty() || !standard().isEmpty())
    {
        if (standard().isEmpty())
        {
            const auto documents = session.sourceDocuments(source);
            // B1 reset this native file's stale links to its Tracker source, so Save Tracker project… is refused.
            if (std::ranges::any_of(documents, [&](QUuid document) { return session.document(document).trackerBindingsReset; }))
                problems.push_back(tr("%1: its links to the original Tracker tracks could not be restored, so it cannot be saved as "
                                      "a Tracker project. Open the original Tracker file again to include it.")
                                       .arg(name));
            else
                problems.push_back(tr("%1: saved only as a MoTrack project. Use Save Tracker project… to include it.").arg(name));
            return std::nullopt;
        }
        if (session.isModified(source))
        {
            const auto answer = askQuestion(this,
                                            tr("Save tabset"),
                                            tr("%1 has unsaved changes. Save it before adding it to the tabset?").arg(name),
                                            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                                            QMessageBox::Yes,
                                            "workspaceTabsetSaveQuestion");
            if (answer == QMessageBox::Cancel)
            {
                cancelled = true;
                return std::nullopt;
            }
            if (answer != QMessageBox::Yes)
            {
                problems.push_back(tr("%1: skipped because it has unsaved changes.").arg(name));
                return std::nullopt;
            }
            // Like Tracker, a failed or canceled component save abandons the whole tabset.
            if (!saveSource(source) || session.isModified(source) || standard().isEmpty())
            {
                cancelled = true;
                return std::nullopt;
            }
        }
        return standard();
    }
    // Raw media: Tracker records an unchanged video's own path. A sequence needs a saved project instead.
    if (session.isModified(source))
    {
        problems.push_back(tr("%1: changed media must be saved as a Tracker project before it can be included.").arg(name));
        return std::nullopt;
    }
    const auto documents = session.sourceDocuments(source);
    if (documents.size() != 1 || session.document(documents.front()).project.clip.mediaReferences.size() != 1)
    {
        problems.push_back(tr("%1: image sequences must be saved as a Tracker project before they can be included.").arg(name));
        return std::nullopt;
    }
    return localProjectPath(session.sourcePath(source));
}

bool WorkspaceWindow::saveTabset(const QString& path)
{
    if (m_Busy)
        return false;
    m_Busy = true;
    refreshLabels();
    bool alive = true;
    QObject lifetime;
    connect(this, &QObject::destroyed, &lifetime, [&] { alive = false; });
    const auto context = m_Context;
    auto& session = context->session();
    const auto finish = [&](const QString& message)
    {
        m_Busy = false;
        refreshLabels();
        if (!message.isEmpty())
            statusBar()->showMessage(message);
    };
    // Tracker lists tabs in order; hidden-only sources follow in workspace order.
    std::vector<QUuid> ordered;
    for (const auto document : session.tabs())
        if (const auto source = session.sourceId(document); std::ranges::find(ordered, source) == ordered.end())
            ordered.push_back(source);
    for (const auto source : session.sources())
        if (std::ranges::find(ordered, source) == ordered.end())
            ordered.push_back(source);
    QStringList problems;
    std::vector<std::pair<QUuid, QString>> references;
    for (const auto source : ordered)
    {
        bool cancelled = false;
        auto file = tabsetReference(source, problems, cancelled);
        if (!alive)
            return false;
        if (cancelled)
        {
            finish(tr("Tabset not saved."));
            return false;
        }
        if (file && !file->isEmpty())
            references.emplace_back(source, std::move(*file));
    }
    if (references.empty())
    {
        finish(tr("No open project has a saved Tracker file or unchanged video to include in a tabset."));
        if (!problems.isEmpty())
            showProblems(this, tr("No tabset was saved."), problems, "workspaceTabsetProblems");
        return false;
    }
    auto destination = path;
    if (destination.isEmpty())
    {
        // The default suffix is applied before the dialog's overwrite confirmation sees the filename.
        auto* dialog = new QFileDialog(this, tr("Save tabset"), {}, tr("Tracker tabsets (*.trk)"));
        dialog->setObjectName("workspaceTabsetSaveDialog");
        dialog->setAcceptMode(QFileDialog::AcceptSave);
        dialog->setDefaultSuffix("trk");
        // Offer the tabset opened last, so an edited set is saved back over the file it came from.
        if (!m_LastTabset.isEmpty())
            dialog->selectFile(m_LastTabset);
        bool dialogAlive = true;
        connect(dialog, &QObject::destroyed, &lifetime, [&] { dialogAlive = false; });
        const bool accepted = dialog->exec() == QDialog::Accepted;
        if (!alive)
            return false;
        if (dialogAlive)
        {
            if (accepted && !dialog->selectedFiles().isEmpty())
                destination = dialog->selectedFiles().front();
            delete dialog;
        }
        if (destination.isEmpty())
        {
            finish({});
            return false;
        }
    }
    const auto physical = localProjectPath(destination);
    if (physical.isEmpty())
    {
        finish(tr("The tabset folder is unavailable."));
        return false;
    }
    if (QFileInfo(physical).exists() && !isTrackerTabsetFile(physical))
    {
        finish(tr("%1 exists and is not a tabset. Choose another filename so it is not replaced.").arg(QFileInfo(physical).fileName()));
        return false;
    }
    // Never replace an experiment, companion or media file that belongs to an open source.
    for (const auto source : session.sources())
    {
        auto owned = saveDestinations(session, source);
        owned.push_back(localProjectPath(session.sourcePath(source)));
        if (std::ranges::any_of(owned, [&](const QString& file) { return !file.isEmpty() && sameLocalFile(file, physical); }))
        {
            finish(tr("%1 belongs to an open project. Choose another tabset filename.").arg(QFileInfo(physical).fileName()));
            return false;
        }
    }
    // Edits made while confirming or saving would otherwise be missing from files the tabset presents as saved.
    const auto sources = session.sources();
    if (std::ranges::any_of(
            references,
            [&](const auto& reference)
            { return std::ranges::find(sources, reference.first) == sources.end() || session.isModified(reference.first); }))
    {
        finish(tr("A project changed while the tabset was being prepared. Save the tabset again."));
        return false;
    }
    // Replacing a tabset edits it: entries that still name their file, and everything Tracker does not read, keep
    // their bytes. A tabset MoTrack cannot read is not replaced, since its content would be lost.
    std::optional<TrackerTabset> existing;
    if (QFileInfo::exists(physical))
    {
        auto read = readTrackerTabset(physical);
        if (!read)
        {
            finish(tr("%1 is a tabset that MoTrack cannot read (%2). Choose another filename so it is not replaced.")
                       .arg(QFileInfo(physical).fileName(), QString::fromStdString(read.error())));
            return false;
        }
        existing = std::move(*read);
    }
    std::vector<QString> files;
    files.reserve(references.size());
    for (const auto& [source, file] : references)
        files.push_back(file);
    const auto* baseline = existing ? &*existing : nullptr;
    const auto entries = tabsetEntries(files, physical, QDir::currentPath(), baseline);
    const auto written = saveTrackerTabset(physical, entries, baseline);
    if (!written)
    {
        finish(tr("Could not save the tabset: %1").arg(QString::fromStdString(written.error())));
        return false;
    }
    finish(tr("Saved tabset %1.").arg(QFileInfo(physical).fileName()));
    if (!problems.isEmpty())
        showProblems(
            this, tr("Some projects were not included in %1.").arg(QFileInfo(physical).fileName()), problems, "workspaceTabsetProblems");
    return true;
}

void WorkspaceWindow::reportArchiveLimits(const QList<QUuid>& documents, const QList<UnreadableProject>& unreadable)
{
    if (documents.isEmpty())
        return;
    QStringList notices;
    if (const auto& archive = m_Context->session().document(documents.front()).archivePayload)
    {
        const auto skipped = static_cast<int>(archive->skippedMembers().size());
        const auto omitted = static_cast<int>(archive->omittedOnSave().size());
        if (skipped > 0)
            notices.push_back(tr("%n archive member(s) with unsafe names were not extracted.", nullptr, skipped));
        if (omitted > 0)
            notices.push_back(tr("Saving this Tracker archive omits %n duplicate copy(ies) among them.", nullptr, omitted));
        if (skipped > omitted)
            notices.push_back(tr("Saved archives keep the other %n unchanged; their names may show the author's folder or user names.",
                                 nullptr,
                                 skipped - omitted));
    }
    // Name one project so a large archive cannot flood the status bar; the Experiments menu lists them all.
    if (unreadable.size() == 1)
        notices.push_back(tr("Could not read the Tracker project %1; see the Experiments menu.").arg(unreadable.front().member));
    else if (unreadable.size() > 1)
        notices.push_back(tr("Could not read %1 and %n other Tracker project(s); see the Experiments menu.",
                             nullptr,
                             static_cast<int>(unreadable.size() - 1))
                              .arg(unreadable.front().member));
    if (!notices.isEmpty())
        statusBar()->showMessage(notices.join(' '));
}

std::optional<QUuid> WorkspaceWindow::currentDocument() const
{
    const auto document = m_Pages.key(m_Tabs->currentWidget());
    return document.isNull() ? std::nullopt : std::optional(document);
}

QString WorkspaceWindow::documentLabel(QUuid document) const
{
    const auto& session = m_Context->session();
    const auto source = session.sourceId(document);
    const auto name = QFileInfo(session.sourcePath(source)).fileName();
    const auto& member = session.document(document).archiveMember;
    return member ? tr("%1 — %2").arg(name, QString::fromStdString(*member)) : name;
}

void WorkspaceWindow::addDocuments(const QList<QUuid>& documents)
{
    const auto selected = m_Context->session().activeDocument();
    {
        const QSignalBlocker block(m_Tabs);
        for (const auto document : documents)
        {
            if (m_Pages.contains(document))
                continue;
            auto* page = new QWidget(m_Tabs);
            m_Pages.insert(document, page);
            const auto index = m_Tabs->addTab(page, documentLabel(document));
            for (const auto side : {QTabBar::LeftSide, QTabBar::RightSide})
                if (auto* button = m_Tabs->tabBar()->tabButton(index, side))
                    button->setToolTip(tr("Hide view; keep this experiment in the workspace"));
        }
    }
    if (selected && m_Pages.contains(*selected))
        (void)showDocument(*selected);
    refreshMenus();
}

void WorkspaceWindow::createView(QUuid document)
{
    auto* editor = new MainWindow(m_Context, document);
    editor->setParent(m_Tabs, Qt::Widget);
    editor->menuBar()->setNativeMenuBar(false);
    editor->menuBar()->hide();
    auto* file = editor->menuBar()->actions().front()->menu();
    for (const auto* name : {"openProject", "openMovie", "openImages"})
        if (auto* action = editor->findChild<QAction*>(name))
        {
            action->setVisible(false);
            action->setShortcut({});
        }
    file->insertActions(file->actions().front(), m_OpenActions);
    file->addSeparator();
    file->addActions({m_SaveAll, m_SaveTabset, m_Revert, m_HideView, m_CloseSource, m_Quit});
    const QSignalBlocker block(m_Tabs);
    auto* placeholder = m_Pages.value(document);
    const auto index = m_Tabs->indexOf(placeholder);
    m_Tabs->removeTab(index);
    delete placeholder;
    m_Pages.insert(document, editor);
    m_Views.insert(document, editor);
    m_Tabs->insertTab(index, editor, documentLabel(document));
    for (const auto side : {QTabBar::LeftSide, QTabBar::RightSide})
        if (auto* button = m_Tabs->tabBar()->tabButton(index, side))
            button->setToolTip(tr("Hide view; keep this experiment in the workspace"));
    connect(editor,
            &QObject::destroyed,
            this,
            [this, document]
            {
                m_Views.remove(document);
                m_Pages.remove(document);
                QTimer::singleShot(0, this, &WorkspaceWindow::refreshMenus);
            });
}

bool WorkspaceWindow::showDocument(QUuid document)
{
    (void)m_Context->session().document(document);
    if (!m_Pages.contains(document))
        addDocuments({document});
    if (!m_Views.contains(document))
        createView(document);
    m_Tabs->setCurrentWidget(m_Views.value(document));
    m_Context->session().showDocument(document);
    refreshMenus();
    return true;
}

void WorkspaceWindow::removeView(QUuid document, bool activateRemaining)
{
    auto* page = m_Pages.take(document);
    if (!page)
        return;
    m_Views.remove(document);
    menuBar()->clear(); // Drop borrowed actions before destroying their owner.
    {
        const QSignalBlocker block(m_Tabs);
        m_Tabs->removeTab(m_Tabs->indexOf(page));
        delete page;
    }
    m_Context->session().hideDocument(document);
    if (activateRemaining)
        if (const auto active = currentDocument())
            (void)showDocument(*active);
    refreshMenus();
}

bool WorkspaceWindow::hideDocument(QUuid document)
{
    if (m_Busy || !m_Pages.contains(document))
        return false;
    removeView(document);
    statusBar()->showMessage(tr("View hidden. Reopen it from the Experiments menu; its edits are retained."));
    return true;
}

bool WorkspaceWindow::revertDocument(QUuid document)
{
    if (m_Busy)
        return false;
    const auto context = m_Context;
    auto& session = context->session();
    if (!session.canRestoreSavedDocument(document))
    {
        statusBar()->showMessage(tr("Cannot revert while a save is pending or the saved files need recovery or a fresh paired save."));
        return false;
    }
    m_Busy = true;
    refreshLabels();
    session.stopActivity(document);
    const auto checkpoint = session.documentCheckpoint(document);
    bool alive = true;
    QObject lifetime;
    connect(this, &QObject::destroyed, &lifetime, [&] { alive = false; });
    const auto answer = askQuestion(
        this,
        tr("Revert experiment?"),
        tr("Discard changes to %1 and restore its last saved or opened state? This clears its undo history.").arg(documentLabel(document)),
        QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel,
        "workspaceRevertQuestion");
    if (!alive)
        return false;
    m_Busy = false;
    refreshLabels();
    if (answer != QMessageBox::Discard)
        return false;
    const auto current = session.documentCheckpoint(document);
    if (current.identity != checkpoint.identity || current.revision != checkpoint.revision || !session.canRestoreSavedDocument(document))
    {
        statusBar()->showMessage(tr("The experiment changed during confirmation. Review it before reverting."));
        return false;
    }
    const auto selected = currentDocument();
    const auto position = m_Tabs->indexOf(m_Pages.value(document));
    removeView(document, false);
    const bool restored = session.restoreSavedDocument(document);
    if (position >= 0)
    {
        (void)showDocument(document);
        m_Tabs->tabBar()->moveTab(m_Tabs->indexOf(m_Pages.value(document)), position);
        if (selected && *selected != document)
            (void)showDocument(*selected);
    }
    context->notifySourceChanged(session.sourceId(document));
    refreshLabels();
    if (restored)
        statusBar()->showMessage(tr("Experiment restored. Other experiments retain their edits."));
    return restored;
}

void WorkspaceWindow::refreshMenus()
{
    menuBar()->clear();
    const auto document = currentDocument();
    auto* editor = document ? m_Views.value(*document) : nullptr;
    if (editor)
        menuBar()->addActions(editor->menuBar()->actions());
    else
        menuBar()->addMenu(m_EmptyFileMenu);
    menuBar()->addMenu(m_ExperimentsMenu);
    refreshLabels();
}

void WorkspaceWindow::refreshLabels()
{
    auto& session = m_Context->session();
    for (auto view = m_Pages.cbegin(); view != m_Pages.cend(); ++view)
    {
        const auto source = session.sourceId(view.key());
        const auto index = m_Tabs->indexOf(view.value());
        const auto label = documentLabel(view.key());
        m_Tabs->setTabText(index, session.isModified(source) ? tr("%1 *").arg(label) : label);
        m_Tabs->setTabToolTip(index, session.sourcePath(source));
    }
    m_Empty->setVisible(m_Tabs->count() == 0);
    m_Tabs->setVisible(m_Tabs->count() > 0);
    m_HideView->setEnabled(!m_Busy && currentDocument().has_value());
    m_Revert->setEnabled(!m_Busy && currentDocument().has_value());
    m_CloseSource->setEnabled(!m_Busy && currentDocument().has_value());
    const auto sources = session.sources();
    m_SaveAll->setEnabled(!m_Busy && !sources.empty());
    m_SaveTabset->setEnabled(!m_Busy && !sources.empty());
    const auto current = currentDocument();
    setWindowTitle(current ? tr("%1[*] — MoTrack").arg(documentLabel(*current)) : tr("MoTrack[*]"));
    setWindowModified(std::ranges::any_of(sources, [&session](QUuid source) { return session.isModified(source); }));
}

void WorkspaceWindow::rebuildExperimentsMenu()
{
    m_ExperimentsMenu->clear();
    for (const auto source : m_Context->session().sources())
        for (const auto document : m_Context->session().sourceDocuments(source))
        {
            const auto label = documentLabel(document);
            auto* action = m_ExperimentsMenu->addAction(m_Pages.contains(document) ? label : tr("%1 (hidden)").arg(label));
            action->setData(document);
            action->setCheckable(true);
            action->setChecked(currentDocument() == document);
            connect(action, &QAction::triggered, this, [this, document] { (void)showDocument(document); });
        }
    for (const auto source : m_Context->session().sources())
    {
        const auto name = QFileInfo(m_Context->session().sourcePath(source)).fileName();
        for (const auto& project : m_Unreadable.value(source))
        {
            // Choosing the entry states the reason; the text alone tells keyboard and screen-reader users.
            auto* action = m_ExperimentsMenu->addAction(tr("%1 — %2 (could not be read)").arg(name, project.member));
            action->setToolTip(project.reason);
            connect(action,
                    &QAction::triggered,
                    this,
                    [this, project] { statusBar()->showMessage(tr("Could not read %1: %2").arg(project.member, project.reason)); });
        }
    }
}

bool WorkspaceWindow::saveSource(QUuid source)
{
    const auto context = m_Context;
    const auto destinations = saveDestinations(context->session(), source);
    for (const auto other : context->session().sources())
        if (other != source && context->session().isModified(other))
            for (const auto& destination : saveDestinations(context->session(), other))
                if (std::ranges::any_of(destinations, [&](const auto& own) { return sameLocalFile(own, destination); }))
                {
                    statusBar()->showMessage(
                        tr("Two modified projects share %1. Save one to a different filename before saving or closing both.")
                            .arg(destination));
                    return false;
                }
    const auto documents = context->session().sourceDocuments(source);
    if (documents.empty())
        return false;
    const auto selected = context->session().activeDocument().value_or(QUuid{});
    MainWindow* editor = nullptr;
    for (const auto document : documents)
    {
        editor = m_Views.value(document);
        if (editor)
            break;
    }
    if (!editor && m_Pages.contains(documents.front()))
    {
        createView(documents.front());
        editor = m_Views.value(documents.front());
    }
    const bool temporary = !editor;
    if (temporary)
    {
        editor = new MainWindow(context, documents.front());
        editor->setParent(this, Qt::Widget);
        editor->hide();
    }
    bool ownerAlive = true;
    bool editorAlive = true;
    QObject lifetime;
    connect(this, &QObject::destroyed, &lifetime, [&] { ownerAlive = false; });
    connect(editor, &QObject::destroyed, &lifetime, [&] { editorAlive = false; });
    const bool saved = editor->saveCurrentWorkspace();
    const auto diagnostic = editorAlive ? editor->statusBar()->currentMessage() : QString{};
    if (temporary && editorAlive)
        delete editor;
    if (!ownerAlive)
        return false;
    if (!selected.isNull() && std::ranges::find(context->session().tabs(), selected) != context->session().tabs().end())
        context->session().showDocument(selected);
    // A temporary editor for hidden experiments is gone now; carry its result (such as omitted archive
    // members) to this window either way.
    if (!saved)
        statusBar()->showMessage(tr("Save canceled or failed: %1").arg(diagnostic));
    else if (!diagnostic.isEmpty())
        statusBar()->showMessage(diagnostic);
    refreshLabels();
    return saved;
}

bool WorkspaceWindow::saveAll()
{
    if (m_Busy)
        return false;
    m_Busy = true;
    refreshLabels();
    bool alive = true;
    QObject lifetime;
    connect(this, &QObject::destroyed, &lifetime, [&] { alive = false; });
    const auto context = m_Context;
    const auto sources = context->session().sources();
    bool saved = true;
    for (const auto source : sources)
        if (context->session().isModified(source))
        {
            saved = saveSource(source);
            if (!alive)
                return false;
            if (!saved)
                break;
        }
    m_Busy = false;
    refreshLabels();
    return saved;
}

std::optional<WorkspaceDiscard> WorkspaceWindow::confirmSourceClose(QUuid source)
{
    if (!m_Context->session().isModified(source))
        return WorkspaceDiscard::RejectModified;
    const auto name = QFileInfo(m_Context->session().sourcePath(source)).fileName();
    const auto choice =
        askQuestion(this,
                    tr("Close project?"),
                    tr("Save changes to %1? This includes every experiment in the project, including hidden views.").arg(name),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                    QMessageBox::Save,
                    "workspaceCloseQuestion");
    if (choice == QMessageBox::Discard)
        return WorkspaceDiscard::Confirmed;
    if (choice == QMessageBox::Save && saveSource(source))
        return WorkspaceDiscard::RejectModified;
    return std::nullopt;
}

bool WorkspaceWindow::releaseSource(QUuid source, WorkspaceDiscard discard)
{
    if (m_Context->session().hasPendingOperations(source) ||
        (discard == WorkspaceDiscard::RejectModified && m_Context->session().isModified(source)))
        return false;
    for (const auto document : m_Context->session().sourceDocuments(source))
        removeView(document, false);
    const bool released = m_Context->session().releaseSource(source, discard);
    if (released)
        m_Unreadable.remove(source);
    if (const auto active = currentDocument())
        (void)showDocument(*active);
    refreshMenus();
    return released;
}

bool WorkspaceWindow::closeSource(QUuid source)
{
    if (m_Busy || m_Context->session().hasPendingOperations(source))
        return false;
    m_Busy = true;
    for (const auto document : m_Context->session().sourceDocuments(source))
        m_Context->session().stopActivity(document);
    bool alive = true;
    QObject lifetime;
    connect(this, &QObject::destroyed, &lifetime, [&] { alive = false; });
    const auto discard = confirmSourceClose(source);
    if (!alive)
        return false;
    const bool closed = discard && releaseSource(source, *discard);
    m_Busy = false;
    refreshLabels();
    return closed;
}

void WorkspaceWindow::closeEvent(QCloseEvent* event)
{
    event->ignore();
    if (m_Busy)
        return;
    // Queued tabset work must not start new opens while the close prompts run, even if the close is then canceled.
    m_ExpandedTabsets.clear();
    if (m_Tabset && !m_Tabset->files.isEmpty())
    {
        m_Tabset->files.clear();
        m_Tabset->problems.push_back(tr("Closing the window stopped the remaining tabset files."));
    }
    if (m_Capture && (m_Capture->hasUnsavedRecording() || m_Capture->state() == CaptureDialog::State::Saving))
    {
        // A camera recording that is not saved yet is asked about first. Keeping it cancels this close:
        // the clip is saved and opened as an experiment, which the user then sees.
        m_Busy = true;
        bool alive = true;
        QObject lifetime;
        connect(this, &QObject::destroyed, &lifetime, [&] { alive = false; });
        const bool close = m_Capture->confirmClose();
        if (!alive)
            return;
        m_Busy = false;
        if (!close)
        {
            using enum CaptureDialog::State;
            if (m_Capture && (m_Capture->state() == Finishing || m_Capture->state() == Saving))
                statusBar()->showMessage(tr("The camera recording is kept: it opens as a new experiment once it is saved."));
            return;
        }
    }
    for (auto request = m_Opening.cbegin(); request != m_Opening.cend(); ++request)
        (void)m_Context->cancelOpen(request.key());
    const auto context = m_Context;
    const auto sources = context->session().sources();
    if (std::ranges::any_of(sources, [&](QUuid source) { return context->session().hasPendingOperations(source); }))
    {
        statusBar()->showMessage(tr("Wait for the current save to finish before closing the workspace."));
        return;
    }
    m_Busy = true;
    bool alive = true;
    QObject lifetime;
    connect(this, &QObject::destroyed, &lifetime, [&] { alive = false; });
    std::vector<std::pair<QUuid, WorkspaceDiscard>> decisions;
    QHash<QUuid, std::uint64_t> approvedRevisions;
    for (const auto source : sources)
        for (const auto document : context->session().sourceDocuments(source))
            context->session().stopActivity(document);
    for (const auto source : sources)
    {
        const auto discard = confirmSourceClose(source);
        if (!alive)
            return;
        if (!discard)
        {
            m_Busy = false;
            refreshLabels();
            return;
        }
        decisions.emplace_back(source, *discard);
        for (const auto document : context->session().sourceDocuments(source))
            approvedRevisions.insert(document, context->session().documentCheckpoint(document).revision);
    }
    if (context->session().sources() != sources ||
        std::ranges::any_of(sources, [&](QUuid source) { return context->session().hasPendingOperations(source); }))
    {
        m_Busy = false;
        refreshLabels();
        return;
    }
    // A later modal prompt can dispatch queued edits. Never release earlier sources against an
    // obsolete decision, and perform this complete preflight before releasing the first source.
    for (const auto source : sources)
        for (const auto document : context->session().sourceDocuments(source))
            if (!approvedRevisions.contains(document) ||
                approvedRevisions.value(document) != context->session().documentCheckpoint(document).revision)
            {
                m_Busy = false;
                refreshLabels();
                return;
            }
    for (const auto& [source, discard] : decisions)
        if (!releaseSource(source, discard))
        {
            m_Busy = false;
            return;
        }
    m_Busy = false;
    // The recording was confirmed for discarding above; its window removes the temporary file.
    if (m_Capture)
        m_Capture->discardAndClose();
    event->accept();
}

} // namespace motrack
