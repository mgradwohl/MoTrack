#include "main_window.hpp"
#include "track_identity.hpp"
#include "workspace_input.hpp"
#include "workspace_window.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSlider>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

#include <functional>
#include <limits>
#include <stdexcept>

namespace
{

QString fixture(const char* name = "linked-images/linked-images.trk")
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/" + name;
}

QString nativeCopy(const QTemporaryDir& temporary, const QString& name)
{
    auto input = motrack::prepareWorkspaceInput(fixture());
    auto document = input.document->document;
    const auto path = temporary.filePath(name + ".motrack");
    motrack::rebaseWorkspaceMedia(document, input.mediaProjectPath, path);
    motrack::writeWorkspaceDocument(path, document);
    return path;
}

bool openProject(motrack::WorkspaceWindow& window, const QString& path)
{
    QSignalSpy opened(window.context().get(), &motrack::WorkspaceContext::inputOpened);
    return window.openProject(path).has_value() && QTest::qWaitFor([&] { return opened.count() == 1; });
}

QUuid currentDocument(const motrack::WorkspaceWindow& window)
{
    if (const auto document = window.currentDocument())
        return *document;
    throw std::runtime_error("Expected a selected experiment.");
}

QTabWidget* tabs(motrack::WorkspaceWindow& window)
{
    return window.findChild<QTabWidget*>("experimentTabs");
}

motrack::MainWindow* editor(motrack::WorkspaceWindow& window)
{
    return static_cast<motrack::MainWindow*>(tabs(window)->currentWidget());
}

bool editMass(motrack::WorkspaceWindow& window, double mass)
{
    auto* view = editor(window);
    auto* action = view->findChild<QAction*>("setPointMassValue");
    if (!action || !action->isEnabled())
        return false;
    bool accepted = false;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog,
                     &QTimer::timeout,
                     &window,
                     []
                     {
                         if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                             dialog->reject();
                     });
    watchdog.start(2000);
    QTimer::singleShot(0,
                       &window,
                       [&]
                       {
                           if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                           {
                               dialog->setTextValue(QLocale().toString(mass));
                               accepted = true;
                               dialog->accept();
                           }
                       });
    action->trigger();
    return accepted;
}

// Respond to real modal controls; keep a bounded fallback if an unexpected dialog is shown.
class CloseChoices
{
  public:
    explicit CloseChoices(std::vector<QMessageBox::StandardButton> choices) : m_Choices(std::move(choices))
    {
        QObject::connect(&m_Timer,
                         &QTimer::timeout,
                         [&]
                         {
                             if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                                 dialog && dialog->objectName() == "workspaceCloseQuestion")
                             {
                                 const auto choice = m_Next < m_Choices.size() ? m_Choices[m_Next++] : QMessageBox::Cancel;
                                 dialog->button(choice)->click();
                             }
                         });
        m_Timer.start(1);
        m_Timeout.setSingleShot(true);
        QObject::connect(&m_Timeout,
                         &QTimer::timeout,
                         []
                         {
                             if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                                 dialog->reject();
                         });
        m_Timeout.start(3000);
    }
    [[nodiscard]] std::size_t count() const
    {
        return m_Next;
    }

  private:
    QTimer m_Timer;
    QTimer m_Timeout;
    std::vector<QMessageBox::StandardButton> m_Choices;
    std::size_t m_Next = 0;
};

TEST(WorkspaceShell, TabsKeepPlayheadsHistoriesMenusAndKeyboardEditsIndependent)
{
    QTemporaryDir temporary;
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openProject(window, nativeCopy(temporary, "one")));
    const auto first = currentDocument(window);
    auto* firstView = editor(window);
    const auto original = window.context()->session().project(first).pointMasses.front().mass;
    ASSERT_TRUE(editMass(window, 7));
    firstView->findChild<QSlider*>("stepSlider")->setValue(2);
    ASSERT_TRUE(openProject(window, nativeCopy(temporary, "two")));
    const auto second = currentDocument(window);
    auto* secondView = editor(window);
    ASSERT_NE(firstView, secondView);
    ASSERT_TRUE(editMass(window, 9));
    secondView->findChild<QSlider*>("stepSlider")->setValue(1);
    ASSERT_EQ(tabs(window)->count(), 2);
    tabs(window)->setCurrentIndex(0);
    EXPECT_EQ(window.currentDocument(), first);
    EXPECT_EQ(firstView->findChild<QSlider*>("stepSlider")->value(), 2);
    EXPECT_EQ(secondView->findChild<QSlider*>("stepSlider")->value(), 1);
    window.activateWindow();
    firstView->findChild<QSlider*>("stepSlider")->setFocus();
    ASSERT_TRUE(QTest::qWaitFor([&] { return window.isActiveWindow(); }));
    QTest::keySequence(firstView->findChild<QSlider*>("stepSlider"), QKeySequence::Undo);
    EXPECT_DOUBLE_EQ(window.context()->session().project(first).pointMasses.front().mass, original);
    EXPECT_DOUBLE_EQ(window.context()->session().project(second).pointMasses.front().mass, 9);
    QTest::keySequence(firstView->findChild<QSlider*>("stepSlider"), QKeySequence::Redo);
    EXPECT_DOUBLE_EQ(window.context()->session().project(first).pointMasses.front().mass, 7);
    tabs(window)->tabBar()->moveTab(0, 1);
    EXPECT_EQ(window.context()->session().tabs(), (std::vector<QUuid>{second, first}));
    EXPECT_EQ(window.currentDocument(), first);
    EXPECT_FALSE(firstView->findChild<QAction*>("openProject")->isVisible());
    EXPECT_TRUE(window.menuBar()->actions().contains(firstView->menuBar()->actions().front()));
    ASSERT_TRUE(window.saveAll());
    EXPECT_FALSE(window.context()->session().isModified(window.context()->session().sourceId(first)));
    EXPECT_FALSE(window.context()->session().isModified(window.context()->session().sourceId(second)));
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(temporary.filePath("one.motrack")).project.pointMasses.front().mass, 7);
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(temporary.filePath("two.motrack")).project.pointMasses.front().mass, 9);
}

TEST(WorkspaceShell, ArchiveTabsAreLazyAndHiddenEditsSaveOnceWithTheWholeSource)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("group.trz");
    ASSERT_TRUE(QFile::copy(fixture("linked-images/multitab-images.trz"), path));
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openProject(window, path));
    ASSERT_EQ(tabs(window)->count(), 2);
    // Only the selected experiment has an editor; unopened members remain part of every save.
    EXPECT_EQ(window.findChildren<QSlider*>("stepSlider").size(), 1);
    const auto source = window.context()->session().sources().front();
    const auto documents = window.context()->session().sourceDocuments(source);
    ASSERT_TRUE(window.showDocument(documents[0]));
    ASSERT_TRUE(editMass(window, 7));
    ASSERT_TRUE(window.showDocument(documents[1]));
    ASSERT_TRUE(editMass(window, 9));
    ASSERT_TRUE(window.hideDocument(documents[0]));
    ASSERT_TRUE(window.hideDocument(documents[1]));
    EXPECT_EQ(tabs(window)->count(), 0);
    EXPECT_FALSE(window.currentDocument());
    EXPECT_TRUE(window.context()->session().isModified(source));
    ASSERT_TRUE(window.saveAll());
    EXPECT_TRUE(window.context()->session().tabs().empty());
    EXPECT_FALSE(window.context()->session().isModified(source));
    const auto saved = motrack::readArchiveGroup(temporary.filePath("group.motrack"));
    ASSERT_EQ(saved.members.size(), 2u);
    EXPECT_DOUBLE_EQ(saved.members[0].document.project.pointMasses.front().mass, 7);
    EXPECT_DOUBLE_EQ(saved.members[1].document.project.pointMasses.front().mass, 9);
    const auto archive = motrack::TrackerArchive::open(path);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(archive.projectPath(archive.projects()[0])).project.pointMasses.front().mass, 7);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(archive.projectPath(archive.projects()[1])).project.pointMasses.front().mass, 9);
    auto* menu = window.findChild<QMenu*>("workspaceExperimentsMenu");
    ASSERT_NE(menu, nullptr);
    Q_EMIT menu->aboutToShow();
    ASSERT_EQ(menu->actions().size(), 2);
    menu->actions().front()->trigger();
    EXPECT_EQ(window.currentDocument(), documents[0]);
    editor(window)->findChild<QAction*>("undoEdit")->trigger();
    EXPECT_TRUE(window.context()->session().isModified(source));
    EXPECT_DOUBLE_EQ(window.context()->session().project(documents[1]).pointMasses.front().mass, 9);
    EXPECT_FALSE(window.hideDocument(documents[1]));
}

TEST(WorkspaceShell, CloseProjectCancelSaveAndDiscardIncludeHiddenMembers)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("group.trz");
    ASSERT_TRUE(QFile::copy(fixture("linked-images/multitab-images.trz"), path));
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openProject(window, path));
    const auto source = window.context()->session().sources().front();
    const auto documents = window.context()->session().sourceDocuments(source);
    ASSERT_TRUE(window.showDocument(documents[0]));
    ASSERT_TRUE(editMass(window, 11));
    ASSERT_TRUE(window.hideDocument(documents[0]));
    {
        CloseChoices choices({QMessageBox::Cancel});
        EXPECT_FALSE(window.closeSource(source));
        EXPECT_EQ(choices.count(), 1u);
    }
    EXPECT_EQ(window.context()->session().sourceDocuments(source), documents);
    {
        CloseChoices choices({QMessageBox::Save});
        EXPECT_TRUE(window.closeSource(source));
        EXPECT_EQ(choices.count(), 1u);
    }
    EXPECT_TRUE(window.context()->session().sources().empty());
    EXPECT_EQ(tabs(window)->count(), 0);
    const auto saved = motrack::readArchiveGroup(temporary.filePath("group.motrack"));
    EXPECT_DOUBLE_EQ(saved.members[0].document.project.pointMasses.front().mass, 11);
    ASSERT_TRUE(openProject(window, temporary.filePath("group.motrack")));
    const auto again = window.context()->session().sources().front();
    ASSERT_TRUE(editMass(window, 19));
    {
        CloseChoices choices({QMessageBox::Discard});
        EXPECT_TRUE(window.closeSource(again));
    }
    EXPECT_EQ(motrack::readArchiveGroup(temporary.filePath("group.motrack")).members[0].document.project,
              saved.members[0].document.project);
}

TEST(WorkspaceShell, CancelingWholeWindowCloseKeepsPreviouslyApprovedSourcesAndHiddenRecords)
{
    QTemporaryDir temporary;
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openProject(window, nativeCopy(temporary, "one")));
    const auto first = currentDocument(window);
    ASSERT_TRUE(editMass(window, 7));
    ASSERT_TRUE(window.hideDocument(first));
    ASSERT_TRUE(openProject(window, nativeCopy(temporary, "two")));
    ASSERT_TRUE(editMass(window, 9));
    const auto sources = window.context()->session().sources();
    {
        CloseChoices choices({QMessageBox::Discard, QMessageBox::Cancel});
        EXPECT_FALSE(window.close());
        EXPECT_EQ(choices.count(), 2u);
    }
    EXPECT_EQ(window.context()->session().sources(), sources);
    EXPECT_TRUE(window.context()->session().isModified(sources[0]));
    EXPECT_DOUBLE_EQ(window.context()->session().project(first).pointMasses.front().mass, 7);
    {
        CloseChoices choices({QMessageBox::Discard, QMessageBox::Save});
        EXPECT_TRUE(window.close());
        EXPECT_EQ(choices.count(), 2u);
    }
    EXPECT_TRUE(window.context()->session().sources().empty());
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(temporary.filePath("two.motrack")).project.pointMasses.front().mass, 9);
}

TEST(WorkspaceShell, PendingSaveRefusesCloseAndSaveFailureRetainsEverySource)
{
    QTemporaryDir temporary;
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openProject(window, nativeCopy(temporary, "one")));
    const auto document = currentDocument(window);
    auto& model = window.context()->session();
    const auto source = model.sourceId(document);
    auto pending = model.beginSave(document);
    if (!pending)
        FAIL() << "Expected a pending save slot.";
    EXPECT_FALSE(window.closeSource(source));
    EXPECT_FALSE(window.close());
    EXPECT_EQ(tabs(window)->count(), 1);
    (void)model.completeSave(*pending, {}, {});
    ASSERT_TRUE(editMass(window, 8));
    // A directory at the destination is a real failed save, without modifying permissions.
    ASSERT_TRUE(QFile::remove(temporary.filePath("one.motrack")));
    ASSERT_TRUE(QDir().mkdir(temporary.filePath("one.motrack")));
    EXPECT_FALSE(window.saveAll());
    EXPECT_TRUE(model.isModified(source));
    EXPECT_EQ(tabs(window)->count(), 1);
    {
        CloseChoices choices({QMessageBox::Save});
        EXPECT_FALSE(window.closeSource(source));
    }
    EXPECT_EQ(model.sources().size(), 1u);
}

TEST(WorkspaceShell, RawImagesAndMoviesOpenWithoutBecomingSaveDestinations)
{
    QTemporaryDir temporary;
    const auto image = temporary.filePath(QString::fromUtf8("frame-測定.png"));
    QImage pixels(16, 12, QImage::Format_RGB32);
    pixels.fill(Qt::green);
    ASSERT_TRUE(pixels.save(image));
    motrack::WorkspaceWindow window;
    window.show();
    QSignalSpy opened(window.context().get(), &motrack::WorkspaceContext::inputOpened);
    ASSERT_TRUE(window.openImages({image, image}, 25));
    ASSERT_TRUE(QTest::qWaitFor([&] { return opened.count() == 1; }));
    const auto images = currentDocument(window);
    auto& model = window.context()->session();
    EXPECT_EQ(model.project(images).clip.frameDurationMs, 40);
    EXPECT_EQ(model.project(images).clip.stepCount, 2);
    EXPECT_TRUE(model.nativeSavePath(model.sourceId(images)).isEmpty());
    EXPECT_TRUE(model.standardSavePath(model.sourceId(images)).isEmpty());
    ASSERT_TRUE(window.openMovie(fixture("linked-mov/constant-rate.mov")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return opened.count() == 2; }));
    const auto movie = currentDocument(window);
    EXPECT_TRUE(model.document(movie).movie);
    EXPECT_TRUE(model.document(movie).useDisplayOrientation);
    EXPECT_FALSE(model.document(movie).alignMovieToClip);
    EXPECT_EQ(model.document(movie).movieFrameTimes, (std::vector<double>{0, .125, .25, .375}));
    EXPECT_TRUE(model.nativeSavePath(model.sourceId(movie)).isEmpty());
    EXPECT_EQ(model.project(images).clip.frameDurationMs, 40);
    const std::atomic_bool cancelled{true};
    EXPECT_THROW((void)motrack::prepareWorkspaceMovie(fixture(), &cancelled), motrack::ProjectError);
    EXPECT_THROW((void)motrack::prepareWorkspaceMovie("missing.mov"), motrack::ProjectError);
    EXPECT_THROW((void)motrack::prepareWorkspaceMovie(fixture()), motrack::ProjectError);
    EXPECT_THROW((void)motrack::prepareWorkspaceImages({}, 25), motrack::ProjectError);
    EXPECT_THROW((void)motrack::prepareWorkspaceImages({image}, -1), motrack::ProjectError);
    EXPECT_THROW((void)motrack::prepareWorkspaceImages({image}, std::numeric_limits<double>::infinity()), motrack::ProjectError);
    EXPECT_THROW((void)motrack::prepareWorkspaceImages({image}, 25, &cancelled), motrack::ProjectError);
    EXPECT_THROW((void)motrack::prepareWorkspaceImages({image, "missing.png"}, 25), motrack::ProjectError);
    auto unknown = motrack::prepareWorkspaceImages({image}, 0);
    EXPECT_FALSE(unknown.document->document.project.clip.frameDurationMs);
}

TEST(WorkspaceShell, OpenControlsBoundAndCancelOnlyTheirOwnRequestWhileFailuresKeepEdits)
{
    QTemporaryDir temporary;
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    ASSERT_TRUE(openProject(window, nativeCopy(temporary, "one")));
    const auto original = currentDocument(window);
    ASSERT_TRUE(editMass(window, 8));
    QSemaphore entered;
    QSemaphore release;
    auto& pool = context->services()->analysisPool();
    const auto cleanup = qScopeGuard(
        [&]
        {
            release.release(2);
            (void)pool.waitForDone(5000);
        });
    for (int i = 0; i < 2; ++i)
        pool.start(
            [&]
            {
                entered.release();
                (void)release.tryAcquire(1, 5000);
            });
    ASSERT_TRUE(entered.tryAcquire(2, 2000));
    QSignalSpy opened(context.get(), &motrack::WorkspaceContext::inputOpened);
    QSignalSpy cancelled(context.get(), &motrack::WorkspaceContext::inputCancelled);
    QSignalSpy failed(context.get(), &motrack::WorkspaceContext::inputFailed);
    ASSERT_TRUE(window.openProject(nativeCopy(temporary, "two")));
    ASSERT_TRUE(window.openProject(nativeCopy(temporary, "three")));
    EXPECT_FALSE(window.openProject(fixture()));
    auto* pending = window.findChild<QListWidget*>("pendingWorkspaceInputs");
    ASSERT_EQ(pending->count(), 2);
    pending->setCurrentRow(0);
    window.findChild<QPushButton*>("cancelWorkspaceOpen")->click();
    release.release(2);
    ASSERT_TRUE(QTest::qWaitFor([&] { return opened.count() == 1 && cancelled.count() == 1; }));
    EXPECT_EQ(tabs(window)->count(), 2);
    EXPECT_EQ(pending->count(), 0);
    ASSERT_TRUE(window.openProject("missing.trk"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return failed.count() == 1; }));
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Could not open"));
    EXPECT_DOUBLE_EQ(context->session().project(original).pointMasses.front().mass, 8);
    EXPECT_TRUE(context->session().history(original).canUndo());
}

TEST(WorkspaceShell, DestructionDuringCloseQuestionAndSaveKeepsOwnedRecordsUsable)
{
    QTemporaryDir temporary;
    auto context = std::make_shared<motrack::WorkspaceContext>();
    auto window = std::make_unique<motrack::WorkspaceWindow>(context);
    window->show();
    ASSERT_TRUE(openProject(*window, nativeCopy(temporary, "one")));
    const auto document = currentDocument(*window);
    const auto source = context->session().sourceId(document);
    ASSERT_TRUE(editMass(*window, 8));
    QTimer::singleShot(0, window.get(), [&] { window.reset(); });
    EXPECT_FALSE(window->closeSource(source));
    EXPECT_FALSE(window);
    EXPECT_DOUBLE_EQ(context->session().project(document).pointMasses.front().mass, 8);
    context->session().showDocument(document);
    window = std::make_unique<motrack::WorkspaceWindow>(context);
    QSemaphore entered;
    QSemaphore release;
    auto& pool = context->services()->savePool();
    const auto cleanup = qScopeGuard(
        [&]
        {
            release.release();
            (void)pool.waitForDone(5000);
        });
    pool.start(
        [&]
        {
            entered.release();
            (void)release.tryAcquire(1, 5000);
        });
    ASSERT_TRUE(entered.tryAcquire(1, 2000));
    QObject::connect(window.get(), &QObject::destroyed, [&] { release.release(); });
    QTimer::singleShot(0, window.get(), [&] { window.reset(); });
    EXPECT_FALSE(window->saveAll());
    EXPECT_FALSE(window);
    EXPECT_TRUE(context->session().isModified(source));
    EXPECT_FALSE(context->session().hasPendingOperations(source));
    EXPECT_TRUE(context->session().tabs().empty());
}

TEST(WorkspaceShell, SwitchingReorderingAndClosingAnotherTabDoNotStopAutoTracker)
{
    QTemporaryDir temporary;
    motrack::WorkspaceDocument document;
    document.project.pointMasses.push_back(
        {.name = "Ball", .marks = {{.frame = 0, .x = 22, .y = 22}}, .id = motrack::allocateTrackId(document.project)});
    document.project.clip.frameDurationMs = 50;
    document.project.lengthUnit = "cm";
    for (int frame = 0; frame < 3; ++frame)
    {
        const auto name = QString("frame%1.png").arg(frame);
        document.project.clip.mediaReferences.push_back(name.toStdString());
        QImage image(64, 48, QImage::Format_Grayscale8);
        image.fill(30);
        for (int y = -5; y <= 5; ++y)
            for (int x = -5; x <= 5; ++x)
                image.scanLine(22 + y)[22 + frame * 6 + x] =
                    static_cast<uchar>(((x + 5) * 71 + (y + 5) * 103 + (x * y + 25) * 19) % 190 + 30);
        ASSERT_TRUE(image.save(temporary.filePath(name)));
    }
    const auto path = temporary.filePath("tracking.motrack");
    motrack::writeWorkspaceDocument(path, document);
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openProject(window, path));
    const auto tracking = currentDocument(window);
    auto* first = editor(window);
    ASSERT_TRUE(openProject(window, fixture("linked-mov/linked-movie.trk")));
    const auto other = currentDocument(window);
    auto& model = window.context()->session();
    const auto otherSource = model.sourceId(other);
    ASSERT_EQ(model.document(other).movieFrameTimes, (std::vector<double>{0, .125, .25, .375}));
    tabs(window)->setCurrentIndex(0);
    EXPECT_EQ(currentDocument(window), tracking);
    auto* action = first->findChild<QAction*>("autoTracker");
    auto* slider = first->findChild<QSlider*>("stepSlider");
    auto* marks = first->findChild<QTableWidget*>("marksTable");
    ASSERT_TRUE(action && slider && marks);
    QSemaphore entered;
    QSemaphore release;
    std::atomic_bool timedOut{false};
    auto& pool = window.context()->services()->analysisPool();
    const auto cleanup = qScopeGuard(
        [&]
        {
            release.release(2);
            (void)pool.waitForDone(5000);
        });
    for (int i = 0; i < 2; ++i)
        pool.start(
            [&]
            {
                entered.release();
                if (!release.tryAcquire(1, 5000))
                    timedOut = true;
            });
    ASSERT_TRUE(entered.tryAcquire(2, 2000));
    action->trigger();
    slider->setValue(1);
    ASSERT_TRUE(action->isChecked());
    tabs(window)->setCurrentIndex(1);
    EXPECT_EQ(currentDocument(window), other);
    tabs(window)->tabBar()->moveTab(1, 0);
    EXPECT_TRUE(action->isChecked());
    ASSERT_TRUE(window.closeSource(otherSource));
    EXPECT_FALSE(timedOut.load());
    EXPECT_TRUE(action->isChecked());
    release.release(2);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !action->isChecked() && marks->rowCount() == 3; }));
    EXPECT_EQ(marks->item(1, 2)->text(), "28");
    EXPECT_EQ(marks->item(2, 2)->text(), "34");
    EXPECT_EQ(currentDocument(window), tracking);
    EXPECT_EQ(model.project(tracking).lengthUnit, "cm");
    EXPECT_EQ(model.project(tracking).clip.frameDurationMs, 50);
    ASSERT_TRUE(window.saveAll());
    EXPECT_EQ(motrack::readWorkspaceDocument(path).project.pointMasses.front().marks.size(), 3u);
    first->findChild<QAction*>("undoEdit")->trigger();
    EXPECT_EQ(marks->rowCount(), 2);
    first->findChild<QAction*>("undoEdit")->trigger();
    ASSERT_EQ(marks->rowCount(), 1);
    slider->setValue(0);
    for (int i = 0; i < 2; ++i)
        pool.start(
            [&]
            {
                entered.release();
                (void)release.tryAcquire(1, 5000);
            });
    ASSERT_TRUE(entered.tryAcquire(2, 2000));
    action->trigger();
    slider->setValue(1);
    ASSERT_TRUE(action->isChecked());
    QSignalSpy opened(window.context().get(), &motrack::WorkspaceContext::inputOpened);
    ASSERT_TRUE(window.openProject(nativeCopy(temporary, "replacement")));
    {
        CloseChoices choices({QMessageBox::Discard});
        ASSERT_TRUE(window.closeSource(model.sourceId(tracking)));
    }
    release.release(2);
    ASSERT_TRUE(QTest::qWaitFor([&] { return opened.count() == 1; }));
    ASSERT_TRUE(pool.waitForDone(5000));
    QApplication::processEvents();
    EXPECT_EQ(model.sources().size(), 1u);
    const auto replacement = currentDocument(window);
    EXPECT_NE(replacement, tracking);
    EXPECT_FALSE(model.history(replacement).canUndo());
    EXPECT_FALSE(model.isModified(model.sourceId(replacement)));
    EXPECT_FALSE(editor(window)->findChild<QAction*>("autoTracker")->isChecked());
}

TEST(WorkspaceShell, OpenDialogsMayBeCancelledOrTheirOwnerDestroyed)
{
    for (const auto* name : {"workspaceOpenProject", "workspaceOpenMovie", "workspaceOpenImages"})
    {
        SCOPED_TRACE(name);
        auto window = std::make_unique<motrack::WorkspaceWindow>();
        window->show();
        QTimer::singleShot(0,
                           window.get(),
                           []
                           {
                               if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                                   dialog->reject();
                           });
        window->findChild<QAction*>(name)->trigger();
        EXPECT_EQ(tabs(*window)->count(), 0);
        QTimer::singleShot(0, window.get(), [&] { window.reset(); });
        window->findChild<QAction*>(name)->trigger();
        EXPECT_FALSE(window);
    }
}

TEST(WorkspaceShell, ClosingWorkspaceCancelsPendingInputsAndCompletionCanDestroyTheWindow)
{
    auto context = std::make_shared<motrack::WorkspaceContext>();
    auto window = std::make_unique<motrack::WorkspaceWindow>(context);
    window->show();
    QSemaphore entered;
    QSemaphore release;
    auto& pool = context->services()->analysisPool();
    const auto cleanup = qScopeGuard(
        [&]
        {
            release.release(2);
            (void)pool.waitForDone(5000);
        });
    for (int i = 0; i < 2; ++i)
        pool.start(
            [&]
            {
                entered.release();
                (void)release.tryAcquire(1, 5000);
            });
    ASSERT_TRUE(entered.tryAcquire(2, 2000));
    QSignalSpy cancelled(context.get(), &motrack::WorkspaceContext::inputCancelled);
    ASSERT_TRUE(window->openProject(fixture()));
    EXPECT_TRUE(window->close());
    release.release(2);
    ASSERT_TRUE(QTest::qWaitFor([&] { return cancelled.count() == 1; }));
    EXPECT_TRUE(context->session().sources().empty());
    QObject::connect(context.get(), &motrack::WorkspaceContext::inputOpened, context.get(), [&] { window.reset(); });
    ASSERT_TRUE(window->openProject(fixture()));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !window; }));
    ASSERT_EQ(context->session().sources().size(), 1u);
    EXPECT_TRUE(context->session().tabs().empty());
}

TEST(WorkspaceShell, DestroyedArchiveWindowHidesUnopenedTabsAndPreservesEveryMember)
{
    auto context = std::make_shared<motrack::WorkspaceContext>();
    auto window = std::make_unique<motrack::WorkspaceWindow>(context);
    ASSERT_TRUE(openProject(*window, fixture("linked-images/multitab-images.trz")));
    const auto source = context->session().sources().front();
    const auto documents = context->session().sourceDocuments(source);
    ASSERT_EQ(tabs(*window)->count(), 2);
    EXPECT_EQ(window->findChildren<QSlider*>("stepSlider").size(), 1);
    window.reset();
    EXPECT_TRUE(context->session().tabs().empty());
    EXPECT_EQ(context->session().sourceDocuments(source), documents);
}

TEST(WorkspaceShell, LaterClosePromptCannotDiscardAnEarlierSourcesNewerEdits)
{
    QTemporaryDir temporary;
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openProject(window, nativeCopy(temporary, "one")));
    const auto first = currentDocument(window);
    ASSERT_TRUE(editMass(window, 7));
    ASSERT_TRUE(openProject(window, nativeCopy(temporary, "two")));
    ASSERT_TRUE(editMass(window, 9));
    const auto sources = window.context()->session().sources();
    QTimer responses;
    QObject::connect(&responses,
                     &QTimer::timeout,
                     &window,
                     [&]
                     {
                         if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                         {
                             if (dialog->text().contains("two.motrack"))
                                 EXPECT_TRUE(window.context()->session().history(first).setPointMassValue(
                                     window.context()->session().project(first), 0, 12));
                             dialog->button(QMessageBox::Discard)->click();
                         }
                     });
    responses.start(1);
    EXPECT_FALSE(window.close());
    EXPECT_EQ(window.context()->session().sources(), sources);
    EXPECT_DOUBLE_EQ(window.context()->session().project(first).pointMasses.front().mass, 12);
}

TEST(WorkspaceShell, SaveAllRefusesTwoModifiedSourcesThatShareAFile)
{
    QTemporaryDir temporary;
    const auto path = nativeCopy(temporary, "same");
    const auto original = motrack::readWorkspaceDocument(path).project;
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openProject(window, path));
    const auto first = currentDocument(window);
    ASSERT_TRUE(editMass(window, 7));
    ASSERT_TRUE(openProject(window, path));
    const auto second = currentDocument(window);
    ASSERT_TRUE(editMass(window, 9));
    EXPECT_FALSE(window.saveAll());
    EXPECT_EQ(motrack::readWorkspaceDocument(path).project, original);
    auto& model = window.context()->session();
    EXPECT_TRUE(model.isModified(model.sourceId(first)));
    EXPECT_TRUE(model.isModified(model.sourceId(second)));
    {
        CloseChoices choices({QMessageBox::Save});
        EXPECT_FALSE(window.closeSource(model.sourceId(first)));
    }
    EXPECT_EQ(model.sources().size(), 2u);
    ASSERT_TRUE(editor(window)->saveWorkspace(temporary.filePath("separate.motrack")));
    ASSERT_TRUE(window.saveAll());
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(path).project.pointMasses.front().mass, 7);
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(temporary.filePath("separate.motrack")).project.pointMasses.front().mass, 9);
}

TEST(WorkspaceShell, SaveAllProtectsAStandardSourcesCompanionFromAnotherEditedNativeSource)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("group.trz");
    const auto native = temporary.filePath("group.motrack");
    ASSERT_TRUE(QFile::copy(fixture("linked-images/multitab-images.trz"), path));
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openProject(window, path));
    const auto first = currentDocument(window);
    ASSERT_TRUE(editMass(window, 7));
    ASSERT_TRUE(window.saveAll());
    ASSERT_TRUE(openProject(window, native));
    const auto second = currentDocument(window);
    ASSERT_TRUE(editMass(window, 9));
    ASSERT_TRUE(window.showDocument(first));
    ASSERT_TRUE(editMass(window, 11));
    EXPECT_FALSE(window.saveAll());
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Two modified projects share"));
    EXPECT_DOUBLE_EQ(motrack::readArchiveGroup(native).members.front().document.project.pointMasses.front().mass, 7);
    ASSERT_TRUE(window.showDocument(second));
    ASSERT_TRUE(editor(window)->saveTrackerProject(temporary.filePath("separate.trz")));
    ASSERT_TRUE(window.saveAll());
    EXPECT_DOUBLE_EQ(motrack::readArchiveGroup(native).members.front().document.project.pointMasses.front().mass, 11);
    EXPECT_DOUBLE_EQ(
        motrack::readArchiveGroup(temporary.filePath("separate.motrack")).members.front().document.project.pointMasses.front().mass, 9);
}

TEST(WorkspaceShell, FirstOrdinarySaveProtectsExistingCompanionsAcrossAllSaveEntryPoints)
{
    for (const bool archive : {false, true})
    {
        SCOPED_TRACE(archive);
        QTemporaryDir temporary;
        const auto standard = temporary.filePath(archive ? "experiment.trz" : "experiment.trk");
        const auto native = temporary.filePath("experiment.motrack");
        const auto input = archive ? fixture("linked-images/multitab-images.trz") : fixture();
        ASSERT_TRUE(QFile::copy(input, standard));
        if (!archive)
        {
            ASSERT_TRUE(QDir().mkdir(temporary.filePath("images")));
            for (int i = 0; i < 4; ++i)
            {
                const auto name = QString("images/frame%1.png").arg(i);
                ASSERT_TRUE(QFile::copy(QFileInfo(input).dir().filePath(name), temporary.filePath(name)));
            }
        }
        auto independent = motrack::prepareWorkspaceInput(standard);
        if (independent.group)
        {
            const auto id = independent.group->memberIds().front();
            ASSERT_TRUE(independent.group->history(id).setPointMassValue(independent.group->project(id), 0, 17));
            motrack::writeArchiveGroup(native, independent.group->prepareSave().snapshot);
        }
        else
        {
            ASSERT_TRUE(independent.document->history.setPointMassValue(independent.document->document.project, 0, 17));
            motrack::writeWorkspaceDocument(native, independent.document->document);
        }
        const auto bytes = [](const QString& path)
        {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        };
        const auto originalStandard = bytes(standard);
        const auto originalNative = bytes(native);
        motrack::WorkspaceWindow window;
        window.show();
        ASSERT_TRUE(openProject(window, standard));
        ASSERT_TRUE(editMass(window, 7));
        int confirmations = 0;
        auto answer = QMessageBox::Cancel;
        QTimer responses;
        QObject::connect(&responses,
                         &QTimer::timeout,
                         &window,
                         [&]
                         {
                             if (auto* question = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                             {
                                 if (question->objectName() == "workspaceCloseQuestion")
                                     question->button(QMessageBox::Save)->click();
                                 else
                                 {
                                     // macOS ignores QMessageBox window titles; verify the actual confirmation and target instead.
                                     EXPECT_EQ(question->text(), QString("This also replaces the existing MoTrack file:\n%1").arg(native));
                                     ++confirmations;
                                     question->button(answer)->click();
                                 }
                             }
                         });
        responses.start(1);
        editor(window)->findChild<QAction*>("saveProject")->trigger();
        EXPECT_FALSE(window.saveAll());
        const auto source = window.context()->session().sourceId(currentDocument(window));
        EXPECT_FALSE(window.closeSource(source));
        EXPECT_EQ(confirmations, 3);
        EXPECT_EQ(bytes(standard), originalStandard);
        EXPECT_EQ(bytes(native), originalNative);
        EXPECT_TRUE(window.context()->session().isModified(source));
        // Continue only if Cancel really retained the editor, keeping a failed regression bounded.
        ASSERT_EQ(tabs(window)->count(), archive ? 2 : 1);
        answer = QMessageBox::Yes;
        ASSERT_TRUE(window.saveAll());
        EXPECT_EQ(confirmations, 4);
        ASSERT_TRUE(editMass(window, 9));
        ASSERT_TRUE(window.saveAll());
        EXPECT_EQ(confirmations, 4); // Established pairs keep automatic Save.
        const auto saved = archive ? motrack::readArchiveGroup(native).members.front().document : motrack::readWorkspaceDocument(native);
        EXPECT_DOUBLE_EQ(saved.project.pointMasses.front().mass, 9);
    }
}

TEST(WorkspaceShell, AbortedProjectAndWindowCloseAllowPendingMovieFramesToComplete)
{
    for (const bool entireWindow : {false, true})
    {
        SCOPED_TRACE(entireWindow);
        motrack::WorkspaceWindow window;
        window.show();
        ASSERT_TRUE(openProject(window, fixture("linked-mov/linked-movie.trk")));
        auto* canvas = editor(window)->findChild<QWidget*>("videoCanvas");
        ASSERT_TRUE(QTest::qWaitFor([&] { return canvas->accessibleName() == "Frame 0"; }));
        ASSERT_TRUE(editMass(window, 9));
        auto* slider = editor(window)->findChild<QSlider*>("stepSlider");
        slider->setValue(1); // No event-loop turn before the close request.
        {
            CloseChoices choices({QMessageBox::Cancel});
            EXPECT_FALSE(entireWindow ? window.close() : window.closeSource(window.context()->session().sourceId(currentDocument(window))));
        }
        EXPECT_TRUE(QTest::qWaitFor([&] { return canvas->accessibleName() == "Frame 1"; }));
        EXPECT_TRUE(editor(window)->findChild<QAction*>("calibrationStick")->isEnabled());
        EXPECT_TRUE(window.context()->session().isModified(window.context()->session().sourceId(currentDocument(window))));
    }
}

bool answerRevert(motrack::WorkspaceWindow& window,
                  QUuid document,
                  QMessageBox::StandardButton answer,
                  const std::function<void()>& duringQuestion = {})
{
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog,
                     &QTimer::timeout,
                     &window,
                     []
                     {
                         if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                             dialog->reject();
                     });
    watchdog.start(2000);
    QTimer response;
    QObject::connect(&response,
                     &QTimer::timeout,
                     &window,
                     [&]
                     {
                         if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                             dialog && dialog->objectName() == "workspaceRevertQuestion")
                         {
                             response.stop();
                             if (duringQuestion)
                                 duringQuestion();
                             dialog->button(answer)->click();
                         }
                     });
    response.start(1);
    return window.revertDocument(document);
}

TEST(WorkspaceShell, RevertRestoresOneSavedExperimentAndKeepsOtherEditsTabOrderAndHiddenRecords)
{
    for (const bool archive : {false, true})
    {
        SCOPED_TRACE(archive);
        QTemporaryDir temporary;
        const auto path = archive ? temporary.filePath("group.trz") : nativeCopy(temporary, "first");
        if (archive)
            ASSERT_TRUE(QFile::copy(fixture("linked-images/multitab-images.trz"), path));
        motrack::WorkspaceWindow window;
        window.show();
        ASSERT_TRUE(openProject(window, path));
        auto& session = window.context()->session();
        const auto first = currentDocument(window);
        const auto source = session.sourceId(first);
        if (!archive)
            ASSERT_TRUE(openProject(window, nativeCopy(temporary, "other")));
        const auto other = archive ? session.sourceDocuments(source).back() : currentDocument(window);
        ASSERT_TRUE(window.showDocument(first));
        ASSERT_TRUE(editMass(window, 7));
        ASSERT_TRUE(window.showDocument(other));
        ASSERT_TRUE(editMass(window, 13));
        ASSERT_TRUE(window.saveAll());
        ASSERT_TRUE(editMass(window, 17));
        ASSERT_TRUE(window.showDocument(first));
        ASSERT_TRUE(editMass(window, 9));
        const auto order = session.tabs();
        const auto identity = session.documentCheckpoint(first).identity;
        auto* original = editor(window);
        auto* slider = original->findChild<QSlider*>("stepSlider");
        slider->setValue(1);
        EXPECT_FALSE(answerRevert(window, first, QMessageBox::Cancel));
        EXPECT_EQ(editor(window), original);
        EXPECT_EQ(slider->value(), 1);
        EXPECT_DOUBLE_EQ(session.project(first).pointMasses.front().mass, 9);
        auto* revert = window.findChild<QAction*>("revertExperiment");
        ASSERT_TRUE(revert && revert->isEnabled());
        EXPECT_TRUE(window.menuBar()->actions().front()->menu()->actions().contains(revert));
        ASSERT_TRUE(answerRevert(window, first, QMessageBox::Discard));
        EXPECT_EQ(session.tabs(), order);
        EXPECT_EQ(currentDocument(window), first);
        EXPECT_EQ(session.documentCheckpoint(first).identity, identity);
        EXPECT_DOUBLE_EQ(session.project(first).pointMasses.front().mass, 7);
        EXPECT_DOUBLE_EQ(session.project(other).pointMasses.front().mass, 17);
        EXPECT_FALSE(session.history(first).canUndo());
        EXPECT_TRUE(session.history(other).canUndo());
        EXPECT_EQ(session.isModified(source), archive);
        ASSERT_TRUE(window.hideDocument(first));
        ASSERT_TRUE(session.history(first).setPointMassValue(session.project(first), 0, 19));
        ASSERT_TRUE(answerRevert(window, first, QMessageBox::Discard));
        EXPECT_EQ(tabs(window)->count(), 1);
        EXPECT_EQ(currentDocument(window), other);
        EXPECT_DOUBLE_EQ(session.project(first).pointMasses.front().mass, 7);
        ASSERT_TRUE(window.saveAll());
        if (archive)
        {
            const auto saved = motrack::readArchiveGroup(temporary.filePath("group.motrack"));
            EXPECT_DOUBLE_EQ(saved.members.front().document.project.pointMasses.front().mass, 7);
            EXPECT_DOUBLE_EQ(saved.members.back().document.project.pointMasses.front().mass, 17);
        }
        else
        {
            EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(path).project.pointMasses.front().mass, 7);
            EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(temporary.filePath("other.motrack")).project.pointMasses.front().mass, 17);
        }
    }
}

TEST(WorkspaceShell, RevertRefusesPendingWritesAndChangesArrivingDuringConfirmation)
{
    QTemporaryDir temporary;
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openProject(window, nativeCopy(temporary, "original")));
    const auto id = currentDocument(window);
    auto& session = window.context()->session();
    ASSERT_TRUE(editMass(window, 7));
    auto* original = editor(window);
    auto pending = session.beginSave(id);
    if (!pending)
        FAIL() << "Expected a pending save.";
    EXPECT_FALSE(window.revertDocument(id));
    EXPECT_EQ(editor(window), original);
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Cannot revert"));
    EXPECT_EQ(session.completeSave(*pending, temporary.filePath("unused.motrack"), {}), motrack::SaveOutcome::Failed);
    EXPECT_FALSE(answerRevert(
        window, id, QMessageBox::Discard, [&] { EXPECT_TRUE(session.history(id).setPointMassValue(session.project(id), 0, 9)); }));
    EXPECT_EQ(editor(window), original);
    EXPECT_DOUBLE_EQ(session.project(id).pointMasses.front().mass, 9);
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("changed during confirmation"));
    EXPECT_FALSE(answerRevert(window, id, QMessageBox::Discard, [&] { pending = session.beginSave(id); }));
    if (!pending)
        FAIL() << "Expected a save started during confirmation.";
    EXPECT_EQ(editor(window), original);
    EXPECT_EQ(session.completeSave(*pending, temporary.filePath("unused.motrack"), {}), motrack::SaveOutcome::Failed);
    EXPECT_TRUE(session.history(id).canUndo());
}

TEST(WorkspaceShell, DestructionDuringRevertQuestionPreservesRetainedEdits)
{
    QTemporaryDir temporary;
    auto window = std::make_unique<motrack::WorkspaceWindow>();
    window->show();
    ASSERT_TRUE(openProject(*window, nativeCopy(temporary, "original")));
    const auto id = currentDocument(*window);
    const auto context = window->context();
    ASSERT_TRUE(editMass(*window, 7));
    QTimer::singleShot(0, context.get(), [&] { window.reset(); });
    EXPECT_FALSE(window->revertDocument(id));
    EXPECT_FALSE(window);
    EXPECT_DOUBLE_EQ(context->session().project(id).pointMasses.front().mass, 7);
    EXPECT_TRUE(context->session().isModified(context->session().sourceId(id)));
    EXPECT_TRUE(context->session().tabs().empty());
}

TEST(WorkspaceShell, RevertKeepsMovieExtentDiscoveredAfterOpeningAnOlderNativeFile)
{
    QTemporaryDir temporary;
    auto input = motrack::prepareWorkspaceMovie(QString::fromUtf8(MOTRACK_FIXTURES) + "/vfr.mp4");
    auto snapshot = input.document->document;
    snapshot.project.clip.sourceFrameCount = 1;
    snapshot.project.clip.stepCount = 1;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createPointMass(snapshot.project, "mass"));
    const auto path = temporary.filePath("older.motrack");
    motrack::rebaseWorkspaceMedia(snapshot, input.mediaProjectPath, path);
    motrack::writeWorkspaceDocument(path, snapshot);
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openProject(window, path));
    const auto id = currentDocument(window);
    auto& session = window.context()->session();
    ASSERT_TRUE(QTest::qWaitFor([&] { return session.project(id).clip.sourceFrameCount == 4; }));
    const auto timeline = session.document(id).movieTimeline;
    ASSERT_TRUE(timeline);
    ASSERT_TRUE(editMass(window, 7));
    ASSERT_TRUE(answerRevert(window, id, QMessageBox::Discard));
    EXPECT_EQ(session.project(id).clip.stepCount, 1);
    EXPECT_EQ(session.project(id).clip.sourceFrameCount, 4);
    EXPECT_EQ(session.document(id).movieTimeline, timeline);
    EXPECT_FALSE(session.isModified(session.sourceId(id)));
    EXPECT_TRUE(session.history(id).setClipRange(session.project(id), 0, 3, 1));
}

} // namespace
