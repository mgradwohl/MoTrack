#include "workspace_window.hpp"

#include "local_media_path.hpp"
#include "main_window.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QCollator>
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
                finishOpen(request);
                statusBar()->showMessage(tr("Could not open: %1").arg(diagnostic));
            });
    connect(m_Context.get(),
            &WorkspaceContext::inputCancelled,
            this,
            [this](QUuid request)
            {
                finishOpen(request);
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

void WorkspaceWindow::finishOpen(QUuid request)
{
    delete m_Opening.take(request);
    m_Pending->setVisible(m_Pending->count() > 0);
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
    const auto selected = context->session().activeDocument();
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
    if (selected && std::ranges::find(context->session().tabs(), *selected) != context->session().tabs().end())
        context->session().showDocument(*selected);
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
