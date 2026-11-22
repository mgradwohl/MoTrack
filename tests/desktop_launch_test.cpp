#include "desktop_launch.hpp"

#include "archive_fixture.hpp"
#include "filesystem_path.hpp"
#include "local_media_path.hpp"
#include "main_window.hpp"
#include "tracker_tabset.hpp"
#include "workspace_document.hpp"
#include "workspace_input.hpp"
#include "workspace_window.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileOpenEvent>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSet>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <stdexcept>

namespace
{

QString fixture(const char* name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + '/' + name;
}

QString corpusFixture(const char* name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/" + name;
}

/// A folder whose name exercises spaces and non-ASCII characters from several scripts.
QDir unicodeFolder(const QTemporaryDir& temporary)
{
    const auto name = QString::fromUtf8("Experiment folder — Ünïcødé 測定");
    EXPECT_TRUE(QDir(temporary.path()).mkpath(name));
    return {QDir(temporary.path()).filePath(name)};
}

QString copied(const QString& source, const QDir& folder, const QString& name)
{
    const auto path = folder.filePath(name);
    EXPECT_TRUE(QFile::copy(source, path)) << path.toStdString();
    return path;
}

QString nativeCopy(const QDir& folder, const QString& name)
{
    auto input = motrack::prepareWorkspaceInput(fixture("point-mass.trk"));
    auto document = input.document->document;
    const auto path = folder.filePath(name);
    motrack::rebaseWorkspaceMedia(document, input.mediaProjectPath, path);
    motrack::writeWorkspaceDocument(path, document);
    return path;
}

QString imageFile(const QDir& folder, const QString& name)
{
    QImage pixels(16, 12, QImage::Format_RGB32);
    pixels.fill(Qt::darkCyan);
    const auto path = folder.filePath(name);
    EXPECT_TRUE(pixels.save(path));
    return path;
}

QString canonical(const QString& path)
{
    return QFileInfo(path).canonicalFilePath();
}

/// Source files of the open tabs, in tab order, as canonical paths.
QStringList tabSources(motrack::WorkspaceWindow& window)
{
    auto& session = window.context()->session();
    QStringList sources;
    for (const auto document : session.tabs())
        sources.push_back(canonical(session.sourcePath(session.sourceId(document))));
    return sources;
}

QStringList canonicalPaths(const QStringList& paths)
{
    QStringList result;
    for (const auto& path : paths)
        result.push_back(canonical(path));
    return result;
}

/// Wait for the given finished batch and return its problems; a batch that never finishes fails the test.
QStringList nextBatch(QSignalSpy& finished, int count = 1)
{
    if (!QTest::qWaitFor([&] { return finished.count() >= count; }, 20000))
        throw std::runtime_error("The requested files did not finish opening.");
    return finished.at(count - 1).front().toStringList();
}

/// The notices of a batch that nextBatch has already waited for.
QStringList batchNotices(const QSignalSpy& finished, int count = 1)
{
    return finished.at(count - 1).at(1).toStringList();
}

QUuid selectedDocument(const motrack::WorkspaceWindow& window)
{
    if (const auto document = window.currentDocument())
        return *document;
    throw std::runtime_error("Expected a selected experiment.");
}

bool contains(const QStringList& problems, const QString& text)
{
    return std::ranges::any_of(problems, [&](const QString& problem) { return problem.contains(text); });
}

/// Keep both analysis workers busy so opens stay pending until released.
class BlockedInputs
{
  public:
    explicit BlockedInputs(motrack::WorkspaceContext& context) : m_Pool(context.services()->analysisPool())
    {
        for (int i = 0; i < 2; ++i)
            m_Pool.start(
                [this]
                {
                    m_Entered.release();
                    (void)m_Release.tryAcquire(1, 10000);
                });
        EXPECT_TRUE(m_Entered.tryAcquire(2, 5000));
    }
    BlockedInputs(const BlockedInputs&) = delete;
    BlockedInputs& operator=(const BlockedInputs&) = delete;
    ~BlockedInputs()
    {
        release();
        (void)m_Pool.waitForDone(10000);
    }
    void release()
    {
        if (!m_Released)
            m_Release.release(2);
        m_Released = true;
    }

  private:
    QThreadPool& m_Pool;
    QSemaphore m_Entered;
    QSemaphore m_Release;
    bool m_Released = false;
};

bool editMass(motrack::WorkspaceWindow& window, double mass)
{
    auto* view = static_cast<motrack::MainWindow*>(window.findChild<QTabWidget*>("experimentTabs")->currentWidget());
    auto* action = view ? view->findChild<QAction*>("setPointMassValue") : nullptr;
    if (!action || !action->isEnabled())
        return false;
    bool accepted = false;
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

/// Answer the next modal question with this object name: run `request` inside its event loop, give held work time to
/// (wrongly) start, record the state with `inspect`, then choose.
class ModalQuestion
{
  public:
    ModalQuestion(QString name, QMessageBox::StandardButton choice, std::function<void()> request, std::function<void()> inspect)
    {
        QObject::connect(&m_Timer,
                         &QTimer::timeout,
                         [this, name = std::move(name), choice, request = std::move(request), inspect = std::move(inspect)]
                         {
                             auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                             if (!dialog || dialog->objectName() != name)
                                 return;
                             if (!m_Asked.isValid())
                             {
                                 m_Asked.start();
                                 request();
                                 return;
                             }
                             if (m_Asked.elapsed() < 200)
                                 return;
                             inspect();
                             m_Timer.stop();
                             m_Answered = true;
                             dialog->button(choice)->click();
                         });
        m_Timer.start(1);
    }
    [[nodiscard]] bool answered() const
    {
        return m_Answered;
    }
    ModalQuestion(const ModalQuestion&) = delete;
    ModalQuestion& operator=(const ModalQuestion&) = delete;

  private:
    QTimer m_Timer;
    QElapsedTimer m_Asked;
    bool m_Answered = false;
};

QStringList folderEntries(const QDir& folder)
{
    return folder.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}

struct SmokeResult
{
    int exitCode = -1;
    QStringList opened;
    QString errors;
};

/// Run the real executable offscreen, so its arguments cross the operating system's process boundary.
SmokeResult runSmokeLaunch(const QStringList& arguments, const QString& workingDirectory)
{
    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("QT_QPA_PLATFORM", "offscreen");
    child.setProcessEnvironment(environment);
    child.setWorkingDirectory(workingDirectory);
    child.start(QString::fromUtf8(MOTRACK_EXECUTABLE), QStringList{"--smoke-test"} + arguments);
    SmokeResult result;
    if (!child.waitForFinished(30000) || child.exitStatus() != QProcess::NormalExit)
    {
        child.kill();
        (void)child.waitForFinished(5000);
        return result;
    }
    result.exitCode = child.exitCode();
    for (const auto& line : QString::fromUtf8(child.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts))
        if (const auto text = line.trimmed(); text.startsWith("opened\t"))
            result.opened.push_back(canonical(text.sliced(7)));
    result.errors = QString::fromUtf8(child.readAllStandardError());
    return result;
}

} // namespace

TEST(DesktopLaunch, ParsesOptionsLocalUrlsAndRelativePathsInArgumentOrder)
{
    const auto base = QDir::cleanPath(QDir::tempPath() + "/launch base");
    const auto absolute = QDir::cleanPath(QDir::tempPath() + QString::fromUtf8("/a b/ü 測定.trk"));
    const auto launch = motrack::parseLaunchArguments({"motrack",
                                                       "--smoke-test",
                                                       "",
                                                       absolute,
                                                       QString::fromUtf8("relative name ő.motrack"),
                                                       QUrl::fromLocalFile(QDir::tempPath() + "/50% done #1.trz").toString(),
                                                       "https://user:secret@example.com/remote.trk",
                                                       "file://",
                                                       "--unknown",
                                                       "-psn_0_12345",
                                                       "--",
                                                       "--smoke-test",
                                                       "-dash.trk"},
                                                      base);
    EXPECT_TRUE(launch.smokeTest);
    EXPECT_EQ(launch.unknownOptions, QStringList{"--unknown"});
    EXPECT_EQ(launch.requested.files,
              (QStringList{absolute,
                           QDir(base).absoluteFilePath(QString::fromUtf8("relative name ő.motrack")),
                           QDir::tempPath() + "/50% done #1.trz",
                           QDir(base).absoluteFilePath("--smoke-test"),
                           QDir(base).absoluteFilePath("-dash.trk")}));
    // Credentials never appear in a report; an empty file URL names no file.
    EXPECT_EQ(launch.requested.remote, (QStringList{"https://example.com/remote.trk", "file://"}));
    EXPECT_FALSE(motrack::parseLaunchArguments({"motrack", "a.trk"}, base).smokeTest);
#ifdef Q_OS_WIN
    // Extended-length spellings from the shell lose their prefix; Qt adds its own for long paths.
    EXPECT_EQ(motrack::launchFilePath(R"(\\?\C:\Long folder\a.trk)", base), "C:/Long folder/a.trk");
    EXPECT_EQ(motrack::launchFilePath(R"(\\?\UNC\server\share\a.trk)", base), "//server/share/a.trk");
    EXPECT_EQ(motrack::launchFilePath(R"(C:\Folder\b.trk)", base), "C:/Folder/b.trk");
    // A drive letter is not a URL scheme.
    EXPECT_EQ(motrack::parseLaunchArguments({"motrack", "C://x.trk"}, base).requested.files, QStringList{"C://x.trk"});
#endif
    const auto defaults =
        motrack::parseLaunchArguments({"motrack", "-AppleLanguages", "(de)", "-NSDocumentRevisionsDebugMode", "YES", "a.trk"}, base);
#ifdef Q_OS_MACOS
    // Cocoa's `-Key value` user defaults, as testers use to switch the interface language.
    EXPECT_TRUE(defaults.unknownOptions.isEmpty());
    EXPECT_EQ(defaults.requested.files, QStringList{QDir(base).absoluteFilePath("a.trk")});
    EXPECT_TRUE(motrack::parseLaunchArguments({"motrack", "-AppleLanguages"}, base).unknownOptions.isEmpty());
#else
    EXPECT_EQ(defaults.unknownOptions, (QStringList{"-AppleLanguages", "-NSDocumentRevisionsDebugMode"}));
#endif
    EXPECT_EQ(motrack::launchFileKind("A.TRK"), motrack::LaunchFileKind::Project);
    EXPECT_EQ(motrack::launchFileKind("a.Trz"), motrack::LaunchFileKind::Project);
    EXPECT_EQ(motrack::launchFileKind("a.motrack"), motrack::LaunchFileKind::Project);
    EXPECT_EQ(motrack::launchFileKind("frame.PNG"), motrack::LaunchFileKind::Image);
    EXPECT_EQ(motrack::launchFileKind("clip.mov"), motrack::LaunchFileKind::Movie);
    EXPECT_EQ(motrack::launchFileKind("no-suffix"), motrack::LaunchFileKind::Movie);
}

TEST(DesktopLaunch, OpensRequestedFilesInOrderOnceWithinTheTwoInputBound)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto menuFirst = nativeCopy(folder, "menu one.motrack");
    const auto menuSecond = nativeCopy(folder, "menu two.motrack");
    const auto project = copied(fixture("point-mass.trk"), folder, QString::fromUtf8("first experiment é.trk"));
    const auto image = imageFile(folder, QString::fromUtf8("frame ü.png"));
    const auto archive = copied(fixture("archive.trz"), folder, QString::fromUtf8("archive 測定.trz"));
    const auto native = nativeCopy(folder, QString::fromUtf8("native ✓ copy.motrack"));
    const auto movie = copied(corpusFixture("linked-mov/constant-rate.mov"), folder, QString::fromUtf8("clip ñ.mov"));
    const auto broken = folder.filePath("broken.trk");
    {
        QFile file(broken);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("not a Tracker project");
    }
    ASSERT_TRUE(folder.mkdir("subfolder"));
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    auto* pending = window.findChild<QListWidget*>("pendingWorkspaceInputs");
    int requestedAtOnce = 0;
    QObject::connect(pending->model(),
                     &QAbstractItemModel::rowsInserted,
                     &window,
                     [&]
                     {
                         int requested = 0;
                         for (int row = 0; row < pending->count(); ++row)
                             requested += pending->item(row)->text().contains("menu") ? 0 : 1;
                         requestedAtOnce = std::max(requestedAtOnce, requested);
                     });
    {
        BlockedInputs blocked(*context);
        // Menu opens hold both input slots; requested files wait instead of reporting a busy workspace.
        ASSERT_TRUE(window.openProject(menuFirst));
        ASSERT_TRUE(window.openProject(menuSecond));
        window.openFiles({.files = {project,
                                    folder.filePath("missing file.trk"),
                                    image,
                                    folder.filePath("subfolder"),
                                    archive,
                                    broken,
                                    folder.filePath(QString::fromUtf8("./first experiment é.trk")), // the same file again
                                    native,
                                    movie},
                          .remote = {"https://example.com/remote.trk"}});
        QTest::qWait(50);
        EXPECT_EQ(pending->count(), 2);
        EXPECT_TRUE(finished.isEmpty());
        EXPECT_FALSE(window.statusBar()->currentMessage().contains("already opening"));
    }
    const auto problems = nextBatch(finished);
    EXPECT_EQ(requestedAtOnce, 1);
    const auto sources = tabSources(window);
    ASSERT_EQ(sources.size(), 7);
    // The two menu opens may finish in either order; requested files follow in their requested order.
    EXPECT_EQ(QSet<QString>(sources.begin(), sources.begin() + 2), (QSet<QString>{canonical(menuFirst), canonical(menuSecond)}));
    EXPECT_EQ(sources.sliced(2), canonicalPaths({project, image, archive, native, movie}));
    EXPECT_EQ(problems.size(), 4) << problems.join('\n').toStdString();
    EXPECT_TRUE(contains(problems, "missing file.trk was not found."));
    EXPECT_TRUE(contains(problems, "subfolder is a folder"));
    EXPECT_TRUE(contains(problems, "broken.trk could not be opened"));
    EXPECT_TRUE(contains(problems, "https://example.com/remote.trk is not a local file"));
    auto& session = context->session();
    const auto& tabs = session.tabs();
    EXPECT_EQ(session.project(tabs[3]).clip.stepCount, 1);
    EXPECT_FALSE(session.project(tabs[3]).clip.frameDurationMs);
    EXPECT_TRUE(session.document(tabs.back()).movie);
    EXPECT_EQ(window.currentDocument(), tabs.back());
    auto* summary = window.findChild<QMessageBox*>("workspaceRequestedFileProblems");
    ASSERT_TRUE(summary);
    EXPECT_TRUE(summary->isVisible());
    EXPECT_TRUE(summary->informativeText().contains("missing file.trk"));
    summary->accept();
}

TEST(DesktopLaunch, LargeBatchesAreBoundedAndTheirSummaryIsAbbreviated)
{
    QTemporaryDir temporary;
    motrack::WorkspaceWindow window;
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    QStringList missing;
    for (int i = 0; i < 300; ++i)
        missing.push_back(QDir(temporary.path()).filePath(QString("missing %1.trk").arg(i)));
    window.openFiles({.files = missing.first(150)});
    window.openFiles({.files = missing.sliced(150)}); // A later request joins the same batch and bound.
    const auto problems = nextBatch(finished);
    EXPECT_EQ(finished.count(), 1);
    ASSERT_EQ(problems.size(), 257);
    EXPECT_TRUE(contains(problems, "Only 256 files can be requested at a time"));
    EXPECT_TRUE(contains(problems, "missing 255.trk was not found."));
    EXPECT_FALSE(contains(problems, "missing 256.trk"));
    auto* summary = window.findChild<QMessageBox*>("workspaceRequestedFileProblems");
    ASSERT_TRUE(summary);
    EXPECT_EQ(summary->informativeText().count('\n'), 20);
    EXPECT_TRUE(summary->informativeText().contains("237 more"));
    summary->accept();
    EXPECT_TRUE(window.context()->session().sources().empty());
}

TEST(DesktopLaunch, CancelingARequestedFileStopsItsBatchAndClosingStopsQueuedFiles)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto one = nativeCopy(folder, "one.motrack");
    const auto two = nativeCopy(folder, "two.motrack");
    const auto three = nativeCopy(folder, "three.motrack");
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    auto* pending = window.findChild<QListWidget*>("pendingWorkspaceInputs");
    {
        BlockedInputs blocked(*context);
        window.openFiles({.files = {one, two, three}});
        ASSERT_TRUE(QTest::qWaitFor([&] { return pending->count() == 1; }));
        pending->setCurrentRow(0);
        window.findChild<QPushButton*>("cancelWorkspaceOpen")->click();
    }
    auto problems = nextBatch(finished);
    EXPECT_EQ(problems, QStringList{"Opening was canceled; 2 remaining file(s) were not opened."});
    EXPECT_TRUE(context->session().sources().empty());
    window.findChild<QMessageBox*>("workspaceRequestedFileProblems")->accept();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !window.findChild<QMessageBox*>("workspaceRequestedFileProblems"); }));
    {
        BlockedInputs blocked(*context);
        window.openFiles({.files = {one, two}});
        ASSERT_TRUE(QTest::qWaitFor([&] { return pending->count() == 1; }));
        EXPECT_TRUE(window.close());
    }
    problems = nextBatch(finished, 2);
    EXPECT_EQ(problems, QStringList{"Closing the window stopped 1 requested file(s) from opening."});
    EXPECT_TRUE(context->session().sources().empty());
    // A hidden window shows no summary after it has been closed.
    EXPECT_FALSE(window.findChild<QMessageBox*>("workspaceRequestedFileProblems"));
}

TEST(DesktopLaunch, RequestsSelectAnAlreadyOpenFileIncludingCaseVariantsAndHiddenViews)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto first = nativeCopy(folder, "first project.motrack");
    const auto second = nativeCopy(folder, "second project.motrack");
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    window.openFiles({.files = {first, second}});
    EXPECT_TRUE(nextBatch(finished).isEmpty());
    auto& session = context->session();
    ASSERT_EQ(session.sources().size(), 2u);
    const auto firstDocument = session.tabs().front();
    const auto secondDocument = session.tabs().back();
    EXPECT_EQ(window.currentDocument(), secondDocument);
    // A repeated Finder open or launch selects the open file; a second copy's later save would replace the first (#255).
    window.openFiles({.files = {first}});
    EXPECT_TRUE(nextBatch(finished, 2).isEmpty());
    EXPECT_EQ(session.sources().size(), 2u);
    EXPECT_EQ(window.currentDocument(), firstDocument);
    ASSERT_TRUE(window.hideDocument(firstDocument));
    EXPECT_EQ(window.currentDocument(), secondDocument);
    window.openFiles({.files = {first}});
    EXPECT_TRUE(nextBatch(finished, 3).isEmpty());
    EXPECT_EQ(window.currentDocument(), firstDocument);
    EXPECT_EQ(session.sources().size(), 2u);
    // Where the file system ignores case (Windows and macOS by default), a case variant names the same file.
    const auto variant = folder.filePath("FIRST PROJECT.MOTRACK");
    if (!QFileInfo::exists(variant))
        return;
    ASSERT_TRUE(window.showDocument(secondDocument));
    window.openFiles({.files = {variant}});
    EXPECT_TRUE(nextBatch(finished, 4).isEmpty());
    EXPECT_EQ(window.currentDocument(), firstDocument);
    EXPECT_EQ(session.sources().size(), 2u);
    const auto third = nativeCopy(folder, "third project.motrack");
    window.openFiles({.files = {third, folder.filePath("THIRD PROJECT.MOTRACK")}});
    EXPECT_TRUE(nextBatch(finished, 5).isEmpty());
    EXPECT_EQ(session.sources().size(), 3u);
}

TEST(DesktopLaunch, RequestsSelectAProjectByTheFileItWasSavedTo)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto original = nativeCopy(folder, "original.motrack");
    const auto savedAs = folder.filePath(QString::fromUtf8("saved as ü.motrack"));
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    window.openFiles({.files = {original}});
    (void)nextBatch(finished);
    auto* view = dynamic_cast<motrack::MainWindow*>(window.findChild<QTabWidget*>("experimentTabs")->currentWidget());
    ASSERT_TRUE(view);
    ASSERT_TRUE(view->saveWorkspace(savedAs));
    auto& session = context->session();
    const auto source = session.sources().front();
    ASSERT_TRUE(motrack::sameLocalFile(session.nativeSavePath(source), savedAs));
    // Finder or a file manager now offers the saved file; a second copy's save would replace this one (#255).
    window.openFiles({.files = {savedAs}});
    EXPECT_TRUE(nextBatch(finished, 2).isEmpty());
    EXPECT_EQ(session.sources(), std::vector<QUuid>{source});
    // The file it was opened from still names the same source.
    window.openFiles({.files = {original}});
    EXPECT_TRUE(nextBatch(finished, 3).isEmpty());
    EXPECT_EQ(session.sources(), std::vector<QUuid>{source});
}

TEST(DesktopLaunch, AStaleCheckResultAfterTheWindowClosedOpensNothing)
{
    QTemporaryDir temporary;
    const auto project = nativeCopy(unicodeFolder(temporary), "checked while closing.motrack");
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    QSemaphore entered;
    QSemaphore release;
    auto& pool = context->services()->requestPool();
    pool.start(
        [&]
        {
            entered.release();
            (void)release.tryAcquire(1, 10000);
        });
    ASSERT_TRUE(entered.tryAcquire(1, 5000));
    const auto cleanup = qScopeGuard(
        [&]
        {
            release.release();
            (void)pool.waitForDone(10000);
        });
    window.openFiles({.files = {project}});
    QTest::qWait(50); // The check is queued behind the blocked worker.
    EXPECT_TRUE(window.close());
    EXPECT_EQ(nextBatch(finished), QStringList{"Closing the window stopped 1 requested file(s) from opening."});
    release.release();
    ASSERT_TRUE(pool.waitForDone(10000));
    QTest::qWait(50); // Deliver the worker's late answer.
    EXPECT_TRUE(context->session().sources().empty());
    EXPECT_EQ(finished.count(), 1);
}

TEST(DesktopLaunch, ARequestedTabsetOpensItsFilesInPlaceBeforeTheNextRequestedFile)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto first = nativeCopy(folder, "first.motrack");
    const auto listedA = nativeCopy(folder, QString::fromUtf8("listed a ü.motrack"));
    const auto listedB = nativeCopy(folder, "listed b.motrack");
    const auto last = nativeCopy(folder, "last.motrack");
    const auto tabset = folder.filePath(QString::fromUtf8("set 測定.trk"));
    const auto directory = canonical(folder.path());
    const std::vector<motrack::TabsetEntry> entries{motrack::tabsetEntry(canonical(listedA), directory),
                                                    motrack::tabsetEntry(canonical(listedB), directory)};
    ASSERT_TRUE(motrack::saveTrackerTabset(tabset, entries).has_value());
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    QSignalSpy tabsetFinished(&window, &motrack::WorkspaceWindow::tabsetFinished);
    // The command line or a drop: the tabset's files take its place, and a file it opened is selected, not reopened.
    window.openFiles({.files = {first, tabset, last, listedA}});
    EXPECT_TRUE(nextBatch(finished).isEmpty());
    EXPECT_EQ(tabsetFinished.count(), 1);
    EXPECT_EQ(tabSources(window), canonicalPaths({first, listedA, listedB, last}));
    EXPECT_EQ(context->session().sources().size(), 4u);
    // The same through the real executable's arguments.
    const auto result = runSmokeLaunch({tabset, last}, folder.path());
    EXPECT_EQ(result.exitCode, 0) << result.errors.toStdString();
    EXPECT_EQ(result.opened, canonicalPaths({listedA, listedB, last}));
}

/// A tabset in the folder listing the given files, relative to the folder as Tracker writes them.
QString writeTabset(const QDir& folder, const QString& name, const QStringList& files)
{
    const auto path = folder.filePath(name);
    std::vector<motrack::TabsetEntry> entries;
    for (const auto& file : files)
        entries.push_back(QFileInfo::exists(file) ? motrack::tabsetEntry(canonical(file), canonical(folder.path()))
                                                  : motrack::TabsetEntry{.absolute = std::nullopt, .relative = file.toStdString()});
    EXPECT_TRUE(motrack::saveTrackerTabset(path, entries).has_value());
    return path;
}

TEST(DesktopLaunch, ATabsetListingAFileRequestedBeforeItReusesTheOpenFile)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto listed = nativeCopy(folder, QString::fromUtf8("listed ü.motrack"));
    const auto other = nativeCopy(folder, "other.motrack");
    const auto tabset = writeTabset(folder, "set.trk", {listed, other});
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    window.openFiles({.files = {listed, tabset}});
    EXPECT_TRUE(nextBatch(finished).isEmpty());
    EXPECT_EQ(tabSources(window), canonicalPaths({listed, other}));
    EXPECT_EQ(context->session().sources().size(), 2u);
}

TEST(DesktopLaunch, ARequestArrivingWhileAMenuTabsetOpensWaitsAndReusesItsFile)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto first = nativeCopy(folder, "first.motrack");
    const auto later = nativeCopy(folder, QString::fromUtf8("listed later ü.motrack"));
    const auto tabset = writeTabset(folder, "menu set.trk", {first, later});
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    QSignalSpy tabsetFinished(&window, &motrack::WorkspaceWindow::tabsetFinished);
    // As the tabset's first file opens, a Finder open or drop requests a file the tabset has not reached.
    bool requested = false;
    QObject::connect(context.get(),
                     &motrack::WorkspaceContext::inputOpened,
                     &window,
                     [&]
                     {
                         if (!std::exchange(requested, true))
                             window.openFiles({.files = {later}});
                     });
    ASSERT_TRUE(window.openProject(tabset));
    EXPECT_TRUE(nextBatch(finished).isEmpty());
    EXPECT_TRUE(requested);
    EXPECT_EQ(tabsetFinished.count(), 1);
    EXPECT_EQ(tabSources(window), canonicalPaths({first, later}));
    EXPECT_EQ(context->session().sources().size(), 2u);
}

TEST(DesktopLaunch, ARequestDuringATabsetsMissingFilePromptWaitsForTheTabset)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto present = nativeCopy(folder, QString::fromUtf8("present ü.motrack"));
    const auto tabset = writeTabset(folder, "partial set.trk", {"missing.motrack", present});
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    QSignalSpy tabsetFinished(&window, &motrack::WorkspaceWindow::tabsetFinished);
    std::size_t sourcesDuringPrompt = 1;
    ModalQuestion missing(
        "workspaceTabsetMissing",
        QMessageBox::No,
        [&] { window.openFiles({.files = {present}}); },
        [&] { sourcesDuringPrompt = context->session().sources().size(); });
    ASSERT_TRUE(window.openProject(tabset));
    EXPECT_TRUE(nextBatch(finished).isEmpty());
    EXPECT_TRUE(missing.answered());
    EXPECT_EQ(sourcesDuringPrompt, 0u);
    EXPECT_EQ(tabsetFinished.count(), 1);
    EXPECT_EQ(tabSources(window), canonicalPaths({present}));
    EXPECT_EQ(context->session().sources().size(), 1u);
}

TEST(DesktopLaunch, AMenuTabsetListingAnOpenFileReusesItAndSavesItsEntry)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    // A saved tabset lists Tracker files, so the projects are Tracker files.
    const auto open = copied(fixture("point-mass.trk"), folder, QString::fromUtf8("already open ü.trk"));
    const auto other = copied(fixture("point-mass.trk"), folder, "other.trk");
    const auto tabset = writeTabset(folder, "listing open.trk", {open, other});
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    QSignalSpy tabsetFinished(&window, &motrack::WorkspaceWindow::tabsetFinished);
    window.openFiles({.files = {open}});
    (void)nextBatch(finished);
    const auto openDocument = selectedDocument(window);
    ASSERT_TRUE(window.hideDocument(openDocument));
    ASSERT_TRUE(window.openProject(tabset));
    ASSERT_TRUE(QTest::qWaitFor([&] { return tabsetFinished.count() == 1; }, 20000));
    // The open file is not opened again; its hidden view returns and, as the first listed experiment, is selected.
    EXPECT_EQ(context->session().sources().size(), 2u);
    EXPECT_EQ(window.currentDocument(), openDocument);
    const auto sources = tabSources(window);
    EXPECT_EQ(QSet<QString>(sources.begin(), sources.end()), (QSet<QString>{canonical(open), canonical(other)}));
    // A tabset saved now still lists the reused file.
    const auto saved = folder.filePath("saved set.trk");
    ASSERT_TRUE(window.saveTabset(saved));
    const auto written = motrack::readTrackerTabset(saved);
    ASSERT_TRUE(written.has_value()) << written.error();
    EXPECT_EQ(written->entries.size(), 2u);
}

TEST(DesktopLaunch, OneFileIdentityMatchesHardLinksForRequestsAndTabsets)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto file = nativeCopy(folder, QString::fromUtf8("linked ü.motrack"));
    const auto link = folder.filePath("hard link.motrack");
    std::error_code error;
    std::filesystem::create_hard_link(motrack::toFilesystemPath(file), motrack::toFilesystemPath(link), error);
    if (error)
        GTEST_SKIP() << "Hard links are unavailable here: " << error.message();
    EXPECT_TRUE(motrack::localFileIdentity(file).has_value());
    EXPECT_EQ(motrack::localFileIdentity(file), motrack::localFileIdentity(link));
    EXPECT_TRUE(motrack::sameLocalFile(file, link));
    EXPECT_EQ(motrack::tabsetIdentityKey(canonical(file)), motrack::tabsetIdentityKey(canonical(link)));
    EXPECT_FALSE(motrack::localFileIdentity(folder.filePath("missing.motrack")).has_value());
    EXPECT_NE(motrack::localFileIdentity(file), motrack::localFileIdentity(nativeCopy(folder, "different.motrack")));
    // A requested hard link of an open file selects it.
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    window.openFiles({.files = {file, link}});
    EXPECT_TRUE(nextBatch(finished).isEmpty());
    EXPECT_EQ(context->session().sources().size(), 1u);
}

TEST(DesktopLaunch, AZeroFileIdOrVolumeIdentifiesNoFile)
{
    // Some network redirectors answer id 0 for every file; such files compare by name, not all as one file.
    EXPECT_FALSE(motrack::fileIdentityKey(7, 0, 0).has_value());
    EXPECT_FALSE(motrack::fileIdentityKey(0, 0, 5).has_value());
    const auto low = motrack::fileIdentityKey(7, 0, 5);
    ASSERT_TRUE(low.has_value());
    EXPECT_EQ(low, motrack::fileIdentityKey(7, 0, 5));
    EXPECT_NE(low, motrack::fileIdentityKey(7, 5, 0));
    EXPECT_NE(low, motrack::fileIdentityKey(8, 0, 5));
    EXPECT_NE(low, motrack::fileIdentityKey(0x70, 0, 5));
}

TEST(DesktopLaunch, FileChecksRunOffTheGuiThread)
{
    QTemporaryDir temporary;
    const auto project = nativeCopy(unicodeFolder(temporary), "on a slow share.motrack");
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    // Occupy the request worker as a share that does not answer would.
    QSemaphore entered;
    QSemaphore release;
    auto& pool = context->services()->requestPool();
    pool.start(
        [&]
        {
            entered.release();
            (void)release.tryAcquire(1, 10000);
        });
    ASSERT_TRUE(entered.tryAcquire(1, 5000));
    const auto cleanup = qScopeGuard(
        [&]
        {
            release.release();
            (void)pool.waitForDone(10000);
        });
    window.openFiles({.files = {project}});
    bool ticked = false;
    QTimer::singleShot(50, &window, [&] { ticked = true; });
    ASSERT_TRUE(QTest::qWaitFor([&] { return ticked; }, 5000)); // The GUI keeps processing events meanwhile.
    EXPECT_TRUE(finished.isEmpty());
    EXPECT_TRUE(context->session().sources().empty());
    release.release();
    EXPECT_TRUE(nextBatch(finished).isEmpty());
    EXPECT_EQ(tabSources(window), canonicalPaths({project}));
}

TEST(DesktopLaunch, RequestsDuringACloseQuestionWaitAndOpenOnlyIfTheCloseIsCanceled)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto edited = nativeCopy(folder, "edited.motrack");
    const auto canceled = nativeCopy(folder, "requested during a canceled close.motrack");
    const auto closed = nativeCopy(folder, "requested during a completed close.motrack");
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    window.openFiles({.files = {edited}});
    (void)nextBatch(finished);
    ASSERT_TRUE(editMass(window, 5));
    auto& session = context->session();
    // A macOS open request can arrive while the close question runs its nested event loop.
    std::size_t sourcesDuringQuestion = 0;
    ModalQuestion cancel(
        "workspaceCloseQuestion",
        QMessageBox::Cancel,
        [&] { window.openFiles({.files = {canceled}}); },
        [&] { sourcesDuringQuestion = session.sources().size(); });
    EXPECT_FALSE(window.close());
    EXPECT_TRUE(cancel.answered());
    EXPECT_EQ(sourcesDuringQuestion, 1u);
    EXPECT_TRUE(nextBatch(finished, 2).isEmpty());
    EXPECT_EQ(tabSources(window), canonicalPaths({edited, canceled}));
    ModalQuestion discard(
        "workspaceCloseQuestion",
        QMessageBox::Discard,
        [&] { window.openFiles({.files = {closed}}); },
        [&] { sourcesDuringQuestion = session.sources().size(); });
    EXPECT_TRUE(window.close());
    EXPECT_TRUE(discard.answered());
    EXPECT_EQ(sourcesDuringQuestion, 2u);
    EXPECT_EQ(nextBatch(finished, 3), QStringList{"Closing the window stopped 1 requested file(s) from opening."});
    EXPECT_TRUE(session.sources().empty());
}

TEST(DesktopLaunch, ArchiveNoticesOutliveTheNextRequestedOpenInTheBatchSummary)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto images = corpusFixture("linked-images");
    const auto read = [](const QString& path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    std::vector<motrack::test::ArchiveEntry> members{{.name = "experiment.trk", .bytes = read(images + "/linked-images.trk")},
                                                     {.name = "broken.trk", .bytes = "invalid xml"}};
    for (const auto* frame : {"images/frame0.png", "images/frame1.png", "images/frame2.png", "images/frame3.png"})
        members.push_back({.name = frame, .bytes = read(images + '/' + frame)});
    for (auto& unsafe : motrack::test::unsafeTrackerExportMembers())
        members.push_back(std::move(unsafe));
    const auto archive = folder.filePath(QString::fromUtf8("library ü.trz"));
    ASSERT_TRUE(motrack::test::writeArchive(archive, members));
    members.erase(members.begin() + 1); // The same archive without its unreadable project.
    const auto limited = folder.filePath(QString::fromUtf8("limited ü.trz"));
    ASSERT_TRUE(motrack::test::writeArchive(limited, members));
    const auto next = nativeCopy(folder, "opened next.motrack");
    motrack::WorkspaceWindow window;
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    window.openFiles({.files = {archive, next}});
    const auto problems = nextBatch(finished);
    const auto notices = batchNotices(finished);
    EXPECT_EQ(tabSources(window), canonicalPaths({archive, next}));
    // The next open replaces the status message; the summary keeps the archive's notices under its name.
    // Skipped members only inform, while an unreadable member project is an incomplete import.
    EXPECT_TRUE(contains(notices, QString::fromUtf8("library ü.trz: 4 archive member(s) with unsafe names were not extracted.")))
        << notices.join('\n').toStdString();
    EXPECT_EQ(problems,
              QStringList{QString::fromUtf8("library ü.trz: Could not read the Tracker project broken.trk; see the Experiments menu.")});
    auto* summary = window.findChild<QMessageBox*>("workspaceRequestedFileProblems");
    ASSERT_TRUE(summary);
    EXPECT_EQ(summary->text(), "Some requested files could not be opened, or opened with limits.");
    EXPECT_TRUE(summary->informativeText().contains("unsafe names"));
    summary->accept();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !window.findChild<QMessageBox*>("workspaceRequestedFileProblems"); }));
    window.openFiles({.files = {limited}});
    EXPECT_TRUE(nextBatch(finished, 2).isEmpty());
    EXPECT_FALSE(batchNotices(finished, 2).isEmpty());
    summary = window.findChild<QMessageBox*>("workspaceRequestedFileProblems");
    ASSERT_TRUE(summary);
    EXPECT_EQ(summary->text(), "Some requested files opened with limits.");
    summary->accept();
    // A launch fails only when a file did not open completely; limits are printed as notices.
    auto result = runSmokeLaunch({limited}, folder.path());
    EXPECT_EQ(result.exitCode, 0) << result.errors.toStdString();
    EXPECT_EQ(result.opened, canonicalPaths({limited}));
    EXPECT_TRUE(result.errors.contains("MoTrack notice: ")) << result.errors.toStdString();
    result = runSmokeLaunch({archive}, folder.path());
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_TRUE(result.errors.contains("Could not read the Tracker project broken.trk")) << result.errors.toStdString();
}

TEST(DesktopLaunch, DropsLocalFilesAsCopiesOntoTheWorkspaceAndExperimentViews)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto first = nativeCopy(folder, QString::fromUtf8("dropped one ü.motrack"));
    const auto second = copied(fixture("point-mass.trk"), folder, QString::fromUtf8("dropped two 測定.trk"));
    motrack::WorkspaceWindow window;
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    auto* overlay = window.findChild<QLabel*>("workspaceDropOverlay");
    ASSERT_TRUE(overlay);
    const auto actions = Qt::CopyAction | Qt::MoveAction;
    QMimeData files;
    files.setUrls({QUrl::fromLocalFile(first), QUrl("https://example.com/linked.trk")});
    QDragEnterEvent enter(QPoint(40, 40), actions, &files, Qt::LeftButton, Qt::NoModifier);
    enter.setDropAction(Qt::MoveAction);
    QApplication::sendEvent(&window, &enter);
    EXPECT_TRUE(enter.isAccepted());
    // A file manager deletes the original after a reported move.
    EXPECT_EQ(enter.dropAction(), Qt::CopyAction);
    EXPECT_TRUE(overlay->isVisible());
    EXPECT_TRUE(overlay->text().contains("Drop to open 1 file(s)"));
    EXPECT_TRUE(overlay->text().contains("1 link(s)"));
    QDragMoveEvent move(QPoint(60, 60), actions, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &move);
    EXPECT_TRUE(move.isAccepted());
    EXPECT_EQ(move.dropAction(), Qt::CopyAction);
    QDragLeaveEvent leave;
    QApplication::sendEvent(&window, &leave);
    EXPECT_FALSE(overlay->isVisible());
    QDragMoveEvent outside(QPoint(60, 60), actions, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &outside);
    EXPECT_FALSE(outside.isAccepted());
    QApplication::sendEvent(&window, &enter);
    QDropEvent drop(QPointF(60, 60), actions, &files, Qt::LeftButton, Qt::NoModifier);
    drop.setDropAction(Qt::MoveAction);
    QApplication::sendEvent(&window, &drop);
    EXPECT_TRUE(drop.isAccepted());
    EXPECT_EQ(drop.dropAction(), Qt::CopyAction);
    EXPECT_FALSE(overlay->isVisible());
    auto problems = nextBatch(finished);
    EXPECT_EQ(problems, QStringList{"https://example.com/linked.trk is not a local file. Download it, then open the downloaded copy."});
    EXPECT_EQ(tabSources(window), canonicalPaths({first}));
    window.findChild<QMessageBox*>("workspaceRequestedFileProblems")->accept();

    // Qt offers a drag to each ancestor that accepts drops until one accepts it. The view's QMainWindow base
    // accepts only its own dock-widget drags, so files dropped on an experiment view reach this window.
    auto* view = window.findChild<QTabWidget*>("experimentTabs")->currentWidget();
    ASSERT_TRUE(dynamic_cast<motrack::MainWindow*>(view));
    const auto center = view->mapTo(&window, view->rect().center());
    auto* under = window.childAt(center);
    ASSERT_TRUE(under == view || view->isAncestorOf(under));
    QMimeData secondDrop;
    secondDrop.setUrls({QUrl::fromLocalFile(second)});
    QDragEnterEvent enterView(under->mapFrom(&window, center), Qt::CopyAction, &secondDrop, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(under, &enterView);
    ASSERT_TRUE(enterView.isAccepted());
    EXPECT_TRUE(overlay->isVisible());
    QDropEvent dropView(QPointF(under->mapFrom(&window, center)), Qt::CopyAction, &secondDrop, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(under, &dropView);
    EXPECT_TRUE(dropView.isAccepted());
    EXPECT_FALSE(overlay->isVisible());
    problems = nextBatch(finished, 2);
    EXPECT_TRUE(problems.isEmpty());
    EXPECT_EQ(tabSources(window), canonicalPaths({first, second}));

    // Remote-only links, move-only drags and data without files are refused without opening anything.
    QMimeData remote;
    remote.setUrls({QUrl("https://example.com/remote.trk")});
    QDragEnterEvent remoteEnter(QPoint(40, 40), actions, &remote, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &remoteEnter);
    EXPECT_FALSE(remoteEnter.isAccepted());
    EXPECT_FALSE(overlay->isVisible());
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Only local files"));
    QDragEnterEvent moveOnly(QPoint(40, 40), Qt::MoveAction, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &moveOnly);
    EXPECT_FALSE(moveOnly.isAccepted());
    QMimeData text;
    text.setText(first);
    QDragEnterEvent textEnter(QPoint(40, 40), actions, &text, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &textEnter);
    EXPECT_FALSE(textEnter.isAccepted());
    QDropEvent textDrop(QPointF(40, 40), actions, &text, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &textDrop);
    EXPECT_FALSE(textDrop.isAccepted());
    QMimeData linked;
    linked.setUrls({QUrl::fromLocalFile(first)});
    QDragEnterEvent linkOnly(QPoint(40, 40), Qt::LinkAction, &linked, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &linkOnly);
    EXPECT_TRUE(linkOnly.isAccepted());
    EXPECT_EQ(linkOnly.dropAction(), Qt::LinkAction);
    QApplication::sendEvent(&window, &leave);
    QTest::qWait(20);
    EXPECT_EQ(finished.count(), 2);
    EXPECT_EQ(window.context()->session().sources().size(), 2u);
}

TEST(DesktopLaunch, OperatingSystemOpenRequestsBeforeAndAfterTheWindowExistOpenInOrder)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto early = nativeCopy(folder, QString::fromUtf8("before window ü.motrack"));
    const auto later = copied(fixture("point-mass.trk"), folder, QString::fromUtf8("after window 測定.trk"));
    const auto reopened = nativeCopy(folder, "after the window closed.motrack");
    motrack::FileOpenRequests requests;
    QObject target;
    target.installEventFilter(&requests);
    QFileOpenEvent beforeWindow(early);
    EXPECT_TRUE(QApplication::sendEvent(&target, &beforeWindow));
    QEvent unrelated(QEvent::Show); // A plain QObject reports this event unhandled unless a filter consumes it.
    EXPECT_FALSE(QApplication::sendEvent(&target, &unrelated));
    {
        motrack::WorkspaceWindow window;
        window.show();
        QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
        requests.attach(&window);
        requests.attach(&window); // Re-attaching replaces the destruction watch rather than adding another.
        QFileOpenEvent afterWindow(QUrl::fromLocalFile(later));
        QFileOpenEvent link(QUrl("https://example.com/linked.trz"));
        EXPECT_TRUE(QApplication::sendEvent(&target, &afterWindow));
        EXPECT_TRUE(QApplication::sendEvent(&target, &link));
        const auto problems = nextBatch(finished);
        EXPECT_EQ(problems, QStringList{"https://example.com/linked.trz is not a local file. Download it, then open the downloaded copy."});
        EXPECT_EQ(tabSources(window), canonicalPaths({early, later}));
        window.findChild<QMessageBox*>("workspaceRequestedFileProblems")->accept();
    }
    // The borrowed window is gone; requests wait for the next one.
    QFileOpenEvent afterClose(reopened);
    EXPECT_TRUE(QApplication::sendEvent(&target, &afterClose));
    motrack::WorkspaceWindow next;
    next.show();
    QSignalSpy finished(&next, &motrack::WorkspaceWindow::filesOpened);
    requests.attach(&next);
    const auto problems = nextBatch(finished);
    EXPECT_TRUE(problems.isEmpty());
    EXPECT_EQ(tabSources(next), canonicalPaths({reopened}));
}

TEST(DesktopLaunch, ProcessArgumentsKeepSpacesUnicodeRelativePathsAndOrder)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto first = copied(fixture("point-mass.trk"), folder, QString::fromUtf8("first experiment é.trk"));
    const auto second = nativeCopy(folder, QString::fromUtf8("second ✓ copy.motrack"));
    const auto relative = QString::fromUtf8("third ő 測定.trk");
    const auto third = copied(fixture("point-mass.trk"), folder, relative);
    auto result = runSmokeLaunch({"--no-such-option", first, second, relative}, folder.path());
    EXPECT_EQ(result.exitCode, 0) << result.errors.toStdString();
    EXPECT_EQ(result.opened, canonicalPaths({first, second, third}));
    EXPECT_TRUE(result.errors.contains("ignoring unknown option --no-such-option")) << result.errors.toStdString();
    // A missing file fails the launch without preventing the others from opening.
    result = runSmokeLaunch({folder.filePath("missing ü.trk"), second}, folder.path());
    EXPECT_EQ(result.exitCode, 1);
    EXPECT_EQ(result.opened, canonicalPaths({second}));
    // macOS passes file-name arguments decomposed (NFD); compare the text in composed form.
    EXPECT_TRUE(result.errors.normalized(QString::NormalizationForm_C).contains(QString::fromUtf8("missing ü.trk was not found.")))
        << result.errors.toStdString();
}

TEST(DesktopLaunch, ASecondInstanceOpeningTheSameProjectLeavesTheFirstInstanceAndFilesIntact)
{
    QTemporaryDir temporary;
    const auto folder = unicodeFolder(temporary);
    const auto project = nativeCopy(folder, QString::fromUtf8("shared project ü.motrack"));
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    window.openFiles({.files = {project}});
    (void)nextBatch(finished);
    const auto document = selectedDocument(window);
    const auto source = context->session().sourceId(document);
    ASSERT_TRUE(editMass(window, 42));
    QFile original(project);
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    const auto bytes = original.readAll();
    original.close();
    const auto entries = folderEntries(folder);
    const auto result = runSmokeLaunch({project}, folder.path());
    EXPECT_EQ(result.exitCode, 0) << result.errors.toStdString();
    EXPECT_EQ(result.opened, canonicalPaths({project}));
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), bytes);
    original.close();
    EXPECT_EQ(folderEntries(folder), entries);
    EXPECT_TRUE(context->session().isModified(source));
    EXPECT_DOUBLE_EQ(context->session().project(document).pointMasses.front().mass, 42);
    // The first instance still owns and saves its edits; the other process left no lock or journal behind.
    ASSERT_TRUE(window.saveAll());
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(project).project.pointMasses.front().mass, 42);
    EXPECT_EQ(folderEntries(folder), entries);
}

TEST(DesktopLaunch, OpensLongPathsIncludingWindowsExtendedLengthSpellings)
{
    QTemporaryDir temporary;
    auto folder = QDir(temporary.path());
    QString nested;
    for (int level = 0; level < 6; ++level)
        nested += QString::fromUtf8("/level %1 with a long folder name — ü 測定 padding text").arg(level);
    ASSERT_TRUE(folder.mkpath(temporary.path() + nested));
    folder.setPath(temporary.path() + nested);
    const auto project = copied(fixture("point-mass.trk"), folder, QString::fromUtf8("long path experiment é.trk"));
    ASSERT_GT(project.size(), 300);
    auto argument = QDir::toNativeSeparators(project);
#ifdef Q_OS_WIN
    const auto extended = R"(\\?\)" + argument;
    EXPECT_EQ(motrack::parseLaunchArguments({"motrack", extended}, {}).requested.files, QStringList{project});
    argument = extended;
#endif
    motrack::WorkspaceWindow window;
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::filesOpened);
    window.openFiles(motrack::parseLaunchArguments({"motrack", argument}, {}).requested);
    const auto problems = nextBatch(finished);
    EXPECT_TRUE(problems.isEmpty()) << problems.join('\n').toStdString();
    EXPECT_EQ(tabSources(window), canonicalPaths({project}));
    const auto result = runSmokeLaunch({argument}, temporary.path());
    EXPECT_EQ(result.exitCode, 0) << result.errors.toStdString();
    EXPECT_EQ(result.opened, canonicalPaths({project}));
}
