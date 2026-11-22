#include "workspace_window.hpp"

#include "local_media_path.hpp"
#include "main_window.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QCollator>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
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

/// A file manager deletes the original after a reported move, so a drop only ever copies or links.
Qt::DropAction safeDropAction(Qt::DropActions possible)
{
    if (possible.testFlag(Qt::CopyAction))
        return Qt::CopyAction;
    return possible.testFlag(Qt::LinkAction) ? Qt::LinkAction : Qt::IgnoreAction;
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
    // Experiment views ignore file drags (their QMainWindow base takes only dock-widget drags), so Qt offers
    // files dropped on them to this window too.
    setAcceptDrops(true);
    m_DropOverlay = new QLabel(central);
    m_DropOverlay->setObjectName("workspaceDropOverlay");
    m_DropOverlay->setAlignment(Qt::AlignCenter);
    m_DropOverlay->setWordWrap(true);
    m_DropOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_DropOverlay->hide();
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
    m_OpenActions = {openProjectAction, openMovieAction, openImagesAction};
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
    m_EmptyFileMenu->addActions({m_SaveAll, m_Revert, m_HideView, m_CloseSource, m_Quit});
    m_ExperimentsMenu = new QMenu(tr("&Experiments"), this);
    m_ExperimentsMenu->setObjectName("workspaceExperimentsMenu");
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
            [this](QUuid request, QUuid, const QList<QUuid>& documents)
            {
                finishOpen(request);
                addDocuments(documents);
            });
    connect(m_Context.get(),
            &WorkspaceContext::inputFailed,
            this,
            [this](QUuid request, const QString& diagnostic)
            {
                finishOpen(request, diagnostic);
                statusBar()->showMessage(tr("Could not open: %1").arg(diagnostic));
            });
    connect(m_Context.get(),
            &WorkspaceContext::inputCancelled,
            this,
            [this](QUuid request)
            {
                finishOpen(request, {}, true);
                statusBar()->showMessage(tr("Opening canceled."));
            });
    QList<QUuid> visible;
    for (const auto document : m_Context->session().tabs())
        visible.push_back(document);
    addDocuments(visible);
}

WorkspaceWindow::~WorkspaceWindow()
{
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

std::optional<QUuid> WorkspaceWindow::openImages(const QStringList& paths, double frameRate)
{
    return trackOpen(m_Context->openImages(paths, frameRate), tr("%n images", nullptr, static_cast<int>(paths.size())));
}

void WorkspaceWindow::finishOpen(QUuid request, const QString& problem, bool cancelled)
{
    delete m_Opening.take(request);
    m_Pending->setVisible(m_Pending->count() > 0);
    if (!m_Requested)
        return;
    if (auto& batch = *m_Requested; batch.request == request)
    {
        batch.request.reset();
        if (!problem.isEmpty())
            batch.problems.push_back(tr("%1 could not be opened: %2").arg(batch.current, problem));
        // Canceling one requested file stops its batch, which is otherwise not visible to cancel.
        if (cancelled && !batch.files.isEmpty())
        {
            batch.problems.push_back(
                tr("Opening was canceled; %n remaining file(s) were not opened.", nullptr, static_cast<int>(batch.files.size())));
            batch.files.clear();
        }
    }
    // Any completion may free the input slot the batch is waiting for; continue after this notification returns.
    QTimer::singleShot(0, this, &WorkspaceWindow::continueRequestedFiles);
}

void WorkspaceWindow::openFiles(RequestedFiles requested)
{
    constexpr qsizetype MAX_REQUESTED_FILES = 256;
    if (!m_Requested)
        m_Requested.emplace();
    auto& batch = *m_Requested;
    for (const auto& link : requested.remote)
        batch.problems.push_back(tr("%1 is not a local file. Download it, then open the downloaded copy.").arg(link));
    for (auto& file : requested.files)
    {
        if (batch.accepted == MAX_REQUESTED_FILES)
        {
            batch.problems.push_back(tr(
                "Only %n files can be requested at a time; the others were not opened.", nullptr, static_cast<int>(MAX_REQUESTED_FILES)));
            ++batch.accepted;
        }
        if (batch.accepted > MAX_REQUESTED_FILES)
            continue;
        batch.files.push_back(std::move(file));
        ++batch.accepted;
    }
    // Start after the caller returns: a drop or file-open event finishes before any summary is shown.
    QTimer::singleShot(0, this, &WorkspaceWindow::continueRequestedFiles);
}

void WorkspaceWindow::continueRequestedFiles()
{
    if (!m_Requested || m_Requested->request)
        return;
    auto& batch = *m_Requested;
    while (!batch.files.isEmpty())
    {
        if (!m_Context->canOpen())
            return; // Menu opens hold both slots; their completion resumes this batch in order.
        const auto path = batch.files.takeFirst();
        const QFileInfo file(path);
        const auto shown = QDir::toNativeSeparators(path);
        if (!file.exists())
        {
            batch.problems.push_back(tr("%1 was not found.").arg(shown));
            continue;
        }
        if (file.isDir())
        {
            batch.problems.push_back(tr("%1 is a folder. Use Open images… to choose its images.").arg(shown));
            continue;
        }
        // As in Tracker, one batch opens each file once; aliases of an already requested file are skipped.
        const auto identity = file.canonicalFilePath();
        if (batch.listed.contains(identity))
            continue;
        batch.listed.insert(identity);
        const auto name = file.fileName();
        std::optional<QUuid> request;
        switch (launchFileKind(path))
        {
        case LaunchFileKind::Project:
            request = m_Context->openProject(path);
            break;
        case LaunchFileKind::Image:
            request = m_Context->openImages({path}, 0.0);
            break;
        case LaunchFileKind::Movie:
            request = m_Context->openMovie(path);
            break;
        }
        batch.request = trackOpen(request, name);
        batch.current = name;
        return;
    }
    auto finished = std::move(batch);
    m_Requested.reset();
    if (!finished.problems.isEmpty() && isVisible())
    {
        constexpr qsizetype MAX_LISTED = 20;
        auto listed = finished.problems.first(std::min(finished.problems.size(), MAX_LISTED));
        if (const auto more = static_cast<int>(finished.problems.size() - MAX_LISTED); more > 0)
            listed.push_back(tr("…and %n more problem(s).", nullptr, more));
        // Window-modal without a nested event loop: later requests and completions continue independently.
        auto* box =
            new QMessageBox(QMessageBox::Warning, tr("Open files"), tr("Some requested files were not opened."), QMessageBox::Ok, this);
        box->setObjectName("workspaceRequestedFileProblems");
        box->setInformativeText(listed.join('\n'));
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open();
    }
    Q_EMIT filesOpened(finished.problems);
}

RequestedFiles WorkspaceWindow::acceptDrag(QDropEvent* event)
{
    auto dropped = event->mimeData() ? droppedFiles(*event->mimeData()) : RequestedFiles{};
    const auto action = safeDropAction(event->possibleActions());
    if (action == Qt::IgnoreAction || dropped.files.isEmpty())
    {
        event->ignore();
        dropped.files.clear();
        return dropped;
    }
    event->setDropAction(action);
    event->accept();
    return dropped;
}

void WorkspaceWindow::dragEnterEvent(QDragEnterEvent* event)
{
    const auto dropped = acceptDrag(event);
    if (dropped.files.isEmpty())
    {
        if (!dropped.remote.isEmpty())
            statusBar()->showMessage(tr("Only local files can be dropped here. Download a linked file first."));
        return;
    }
    auto text = tr("Drop to open %n file(s)", nullptr, static_cast<int>(dropped.files.size()));
    if (!dropped.remote.isEmpty())
        text += '\n' + tr("%n link(s) to files that are not local will be skipped.", nullptr, static_cast<int>(dropped.remote.size()));
    // Follow the current palette, including a theme change since the last drag.
    const auto accent = palette().color(QPalette::Highlight);
    auto fill = palette().color(QPalette::Base);
    fill.setAlpha(230);
    m_DropOverlay->setStyleSheet(QStringLiteral("QLabel#workspaceDropOverlay { border: 3px dashed %1; border-radius: 12px; "
                                                "background-color: rgba(%2, %3, %4, %5); padding: 24px; }")
                                     .arg(accent.name())
                                     .arg(fill.red())
                                     .arg(fill.green())
                                     .arg(fill.blue())
                                     .arg(fill.alpha()));
    auto overlayFont = font();
    if (overlayFont.pointSizeF() > 0)
        overlayFont.setPointSizeF(overlayFont.pointSizeF() * 1.4);
    m_DropOverlay->setFont(overlayFont);
    m_DropOverlay->setText(text);
    m_DropOverlay->setGeometry(centralWidget()->rect().adjusted(12, 12, -12, -12));
    m_DropOverlay->raise();
    m_DropOverlay->show();
}

void WorkspaceWindow::dragMoveEvent(QDragMoveEvent* event)
{
    // The enter event already examined the data; keep its decision and only refuse a move-only drag.
    if (m_DropOverlay->isVisible() && safeDropAction(event->possibleActions()) != Qt::IgnoreAction)
    {
        event->setDropAction(safeDropAction(event->possibleActions()));
        event->accept();
    }
    else
        event->ignore();
}

void WorkspaceWindow::dragLeaveEvent(QDragLeaveEvent* event)
{
    m_DropOverlay->hide();
    event->accept();
}

void WorkspaceWindow::dropEvent(QDropEvent* event)
{
    m_DropOverlay->hide();
    if (auto dropped = acceptDrag(event); !dropped.files.isEmpty())
        openFiles(std::move(dropped));
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
    file->addActions({m_SaveAll, m_Revert, m_HideView, m_CloseSource, m_Quit});
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
    if (!saved)
        statusBar()->showMessage(tr("Save canceled or failed: %1").arg(diagnostic));
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
    // Requested files must not start opens while the close prompts run, even if the close is then canceled.
    if (m_Requested && !m_Requested->files.isEmpty())
    {
        m_Requested->problems.push_back(
            tr("Closing the window stopped %n requested file(s) from opening.", nullptr, static_cast<int>(m_Requested->files.size())));
        m_Requested->files.clear();
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
    event->accept();
}

} // namespace motrack
