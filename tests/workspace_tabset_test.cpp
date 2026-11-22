#include "archive_group.hpp"
#include "main_window.hpp"
#include "tracker_archive.hpp"
#include "tracker_tabset.hpp"
#include "workspace_document.hpp"
#include "workspace_input.hpp"
#include "workspace_window.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <gtest/gtest.h>

#include <functional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace
{

using motrack::TabsetEntry;

QString fixture(const char* name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/" + name;
}

QString canonical(const QString& path)
{
    return QFileInfo(path).canonicalFilePath();
}

QTabWidget* tabs(motrack::WorkspaceWindow& window)
{
    return window.findChild<QTabWidget*>("experimentTabs");
}

std::vector<QString> tabLabels(motrack::WorkspaceWindow& window)
{
    std::vector<QString> labels;
    labels.reserve(static_cast<std::size_t>(tabs(window)->count()));
    for (int index = 0; index < tabs(window)->count(); ++index)
        labels.push_back(tabs(window)->tabText(index));
    return labels;
}

void writeTabset(const QString& path, const std::vector<TabsetEntry>& entries)
{
    ASSERT_TRUE(motrack::saveTrackerTabset(path, entries).has_value());
}

TabsetEntry absoluteEntry(const QString& path)
{
    const auto file = canonical(path).toStdString();
    return {.absolute = file, .relative = file};
}

/// The entry MoTrack itself writes for `path` in the tabset `set`, so that saving the set back keeps it wherever the
/// checkout and the temporary folder are.
TabsetEntry listedEntry(const QString& path, const QString& set)
{
    return motrack::tabsetEntry(canonical(path), canonical(QFileInfo(set).absolutePath()));
}

/// Answer the expected modal dialogs in order; any other or late dialog is rejected after a bound.
class DialogScript
{
  public:
    using Step = std::function<bool(QWidget*)>;
    explicit DialogScript(std::vector<Step> steps) : m_Steps(std::move(steps))
    {
        QObject::connect(&m_Timer,
                         &QTimer::timeout,
                         [this]
                         {
                             auto* dialog = QApplication::activeModalWidget();
                             if (dialog && m_Next < m_Steps.size() && m_Steps[m_Next](dialog))
                             {
                                 ++m_Next;
                                 m_Timeout.start(); // Bound each wait, not the whole script, on slow runners.
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
        m_Timeout.start(15000);
    }
    [[nodiscard]] std::size_t handled() const
    {
        return m_Next;
    }

  private:
    std::vector<Step> m_Steps;
    std::size_t m_Next = 0;
    QTimer m_Timer;
    QTimer m_Timeout;
};

DialogScript::Step answer(const char* name, QMessageBox::StandardButton button)
{
    return [name, button](QWidget* widget)
    {
        auto* box = qobject_cast<QMessageBox*>(widget);
        if (!box || box->objectName() != name)
            return false;
        box->button(button)->click();
        return true;
    };
}

DialogScript::Step chooseFile(QString path)
{
    return [path = std::move(path)](QWidget* widget)
    {
        auto* dialog = qobject_cast<QFileDialog*>(widget);
        if (!dialog)
            return false;
        dialog->selectFile(path);
        // QFileDialog::accept is protected; dispatch through the public QDialog slot.
        static_cast<QDialog*>(dialog)->accept();
        return true;
    };
}

bool editMass(motrack::WorkspaceWindow& window, double mass)
{
    auto* view = static_cast<motrack::MainWindow*>(tabs(window)->currentWidget());
    auto* action = view->findChild<QAction*>("setPointMassValue");
    if (!action || !action->isEnabled())
        return false;
    bool accepted = false;
    DialogScript script({[&](QWidget* widget)
                         {
                             auto* dialog = qobject_cast<QInputDialog*>(widget);
                             if (!dialog)
                                 return false;
                             dialog->setTextValue(QLocale().toString(mass));
                             accepted = true;
                             dialog->accept();
                             return true;
                         }});
    action->trigger();
    return accepted;
}

/// Opening completes when every listed tab exists and no input remains pending.
bool openTabset(motrack::WorkspaceWindow& window, const QString& path, int expectedTabs)
{
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::tabsetFinished);
    return window.openProject(path).has_value() && QTest::qWaitFor([&] { return finished.count() == 1; }, 20000) &&
           tabs(window)->count() == expectedTabs;
}

QMessageBox* problems(motrack::WorkspaceWindow& window)
{
    QMessageBox* box = nullptr;
    (void)QTest::qWaitFor([&] { return (box = window.findChild<QMessageBox*>("workspaceTabsetProblems")) != nullptr; });
    return box;
}

/// Dismiss a summary and wait until it is gone, so that the next one can be told apart from it.
void dismiss(motrack::WorkspaceWindow& window, QMessageBox* box)
{
    box->accept();
    ASSERT_TRUE(QTest::qWaitFor([&] { return window.findChild<QMessageBox*>("workspaceTabsetProblems") == nullptr; }));
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

TEST(WorkspaceTabset, PreparedTabsetsOwnNoSourceAndCannotBeAdopted)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("set.trk");
    writeTabset(path, {absoluteEntry(fixture("linked-images/linked-images.trk"))});
    auto input = motrack::prepareWorkspaceInput(path);
    ASSERT_TRUE(input.tabset.has_value());
    EXPECT_FALSE(input.document);
    EXPECT_FALSE(input.group);
    ASSERT_EQ(input.tabset->size(), 1U);
    EXPECT_EQ(input.tabset->front().file, canonical(fixture("linked-images/linked-images.trk")));
    motrack::WorkspaceSession session;
    EXPECT_THROW((void)motrack::adoptWorkspaceInput(session, input), motrack::ProjectError);
    EXPECT_TRUE(session.sources().empty());
    // An ordinary experiment is not mistaken for a tabset.
    EXPECT_FALSE(motrack::prepareWorkspaceInput(fixture("linked-images/linked-images.trk")).tabset);
}

TEST(WorkspaceTabset, OpensListedFilesInOrderAsIndependentSourcesAndSelectsTheFirstExperiment)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    ASSERT_TRUE(QFile::copy(fixture("linked-images/multitab-images.trz"), temporary.filePath("group.trz")));
    const auto set = temporary.filePath("set.trk");
    writeTabset(set,
                {
                    absoluteEntry(fixture("linked-mov/constant-rate.mov")),
                    absoluteEntry(fixture("linked-images/linked-images.trk")),
                    {.absolute = std::nullopt, .relative = "group.trz"},
                });
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openTabset(window, set, 4));
    const auto labels = tabLabels(window);
    EXPECT_EQ(labels[0], "constant-rate.mov");
    EXPECT_EQ(labels[1], "linked-images.trk");
    EXPECT_TRUE(labels[2].startsWith("group.trz"));
    EXPECT_TRUE(labels[3].startsWith("group.trz"));
    auto& session = window.context()->session();
    ASSERT_EQ(session.sources().size(), 3U);
    // Like Tracker, the first experiment file is selected rather than the leading video.
    const auto selected = window.currentDocument();
    ASSERT_TRUE(selected);
    EXPECT_EQ(canonical(session.sourcePath(session.sourceId(*selected))).toStdString(),
              canonical(fixture("linked-images/linked-images.trk")).toStdString());
    EXPECT_EQ(window.findChild<QMessageBox*>("workspaceTabsetProblems"), nullptr);
    // Each source keeps its own history: editing one leaves the others clean.
    ASSERT_TRUE(editMass(window, 4));
    EXPECT_TRUE(session.isModified(session.sourceId(*selected)));
    for (const auto source : session.sources())
        if (source != session.sourceId(*selected))
            EXPECT_FALSE(session.isModified(source));
}

TEST(WorkspaceTabset, MissingFilesCanBeLocatedOrSkippedAndOtherProblemsAreReported)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto set = temporary.filePath("set.trk");
    const auto experiment = fixture("linked-images/linked-images.trk");
    writeTabset(set,
                {
                    {.absolute = "/nowhere/moved.trk", .relative = "moved.trk"},
                    listedEntry(experiment, set),
                    {.absolute = std::nullopt, .relative = "set.trk"},
                    {.absolute = std::nullopt, .relative = "gone.trk"},
                });
    motrack::WorkspaceWindow window;
    window.show();
    auto* saveAction = window.findChild<QAction*>("saveTrackerTabset");
    ASSERT_NE(saveAction, nullptr);
    bool savedWhileOpening = true;
    DialogScript script({[&](QWidget* widget)
                         {
                             if (widget->objectName() != "workspaceTabsetMissing")
                                 return false;
                             // A tabset that is still opening cannot be saved over itself with only some of its files.
                             savedWhileOpening = saveAction->isEnabled() || window.saveTabset(set);
                             return answer("workspaceTabsetMissing", QMessageBox::Yes)(widget);
                         },
                         chooseFile(experiment),
                         answer("workspaceTabsetMissing", QMessageBox::No)});
    ASSERT_TRUE(openTabset(window, set, 1));
    EXPECT_EQ(script.handled(), 3U);
    EXPECT_FALSE(savedWhileOpening);
    EXPECT_TRUE(saveAction->isEnabled());
    // The located file is also the next entry; each file opens once, unlike Tracker.
    EXPECT_EQ(window.context()->session().sources().size(), 1U);
    EXPECT_EQ(tabs(window)->count(), 1);
    auto* box = problems(window);
    ASSERT_NE(box, nullptr);
    const auto text = box->informativeText();
    EXPECT_TRUE(text.contains("set.trk")) << text.toStdString();
    EXPECT_TRUE(text.contains("gone.trk")) << text.toStdString();
    dismiss(window, box);

    // Saving the set back keeps every entry, including those whose files did not open, and says so.
    const auto bytes = readBytes(set);
    ASSERT_TRUE(window.saveTabset(set));
    box = problems(window);
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->informativeText().contains("moved.trk: not open, so the tabset still lists it as before."))
        << box->informativeText().toStdString();
    EXPECT_TRUE(box->informativeText().contains("gone.trk: not open")) << box->informativeText().toStdString();
    dismiss(window, box);
    EXPECT_EQ(readBytes(set), bytes);
}

TEST(WorkspaceTabset, SavingAsksToSaveChangedProjectsAndReopensTheSameSet)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto archive = temporary.filePath("group.trz");
    const auto movie = temporary.filePath("clip.mov");
    ASSERT_TRUE(QFile::copy(fixture("linked-images/multitab-images.trz"), archive));
    ASSERT_TRUE(QFile::copy(fixture("linked-mov/constant-rate.mov"), movie));
    const auto set = temporary.filePath("sets/lesson.trk");
    ASSERT_TRUE(QDir().mkpath(temporary.filePath("sets")));
    {
        motrack::WorkspaceWindow window;
        window.show();
        QSignalSpy opened(window.context().get(), &motrack::WorkspaceContext::inputOpened);
        ASSERT_TRUE(window.openProject(archive));
        ASSERT_TRUE(QTest::qWaitFor([&] { return opened.count() == 1; }));
        ASSERT_TRUE(window.openMovie(movie));
        ASSERT_TRUE(QTest::qWaitFor([&] { return opened.count() == 2; }));
        const auto& session = window.context()->session();
        const auto group = session.sources().front();
        ASSERT_TRUE(window.showDocument(session.sourceDocuments(group).back()));
        ASSERT_TRUE(editMass(window, 13));
        ASSERT_TRUE(session.isModified(group));
        DialogScript script({answer("workspaceTabsetSaveQuestion", QMessageBox::Yes)});
        ASSERT_TRUE(window.saveTabset(set));
        EXPECT_EQ(script.handled(), 1U);
        EXPECT_FALSE(session.isModified(group));
    }
    QFile file(set);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto tabset = motrack::parseTrackerTabset(file.readAll().toStdString());
    file.close(); // Windows refuses to move an open file below.
    ASSERT_TRUE(tabset.has_value());
    const auto& entries = tabset->entries;
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].relative, "../group.trz");
    EXPECT_EQ(entries[0].absolute, QDir::fromNativeSeparators(canonical(archive)).toStdString());
    EXPECT_EQ(entries[1].relative, "../clip.mov");
    const auto saved = motrack::TrackerArchive::open(archive);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(saved.projectPath(saved.projects().back())).project.pointMasses.front().mass, 13);

    // Relocating the whole folder keeps the set usable: relative paths win over the stale absolute ones.
    const auto relocated = temporary.filePath("relocated");
    ASSERT_TRUE(QDir().mkpath(relocated + "/sets"));
    ASSERT_TRUE(QFile::rename(archive, relocated + "/group.trz"));
    ASSERT_TRUE(QFile::rename(movie, relocated + "/clip.mov"));
    ASSERT_TRUE(QFile::rename(set, relocated + "/sets/lesson.trk"));
    ASSERT_TRUE(QFile::remove(temporary.filePath("group.motrack")));
    motrack::WorkspaceWindow reopened;
    reopened.show();
    ASSERT_TRUE(openTabset(reopened, relocated + "/sets/lesson.trk", 3));
    auto& session = reopened.context()->session();
    ASSERT_EQ(session.sources().size(), 2U);
    EXPECT_EQ(canonical(session.sourcePath(session.sources().front())).toStdString(), canonical(relocated + "/group.trz").toStdString());
    EXPECT_DOUBLE_EQ(session.project(session.sourceDocuments(session.sources().front()).back()).pointMasses.front().mass, 13);
}

TEST(WorkspaceTabset, SavingRefusesCancelledSavesOpenProjectFilesAndSetsWithNothingToReference)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto archive = temporary.filePath("group.trz");
    ASSERT_TRUE(QFile::copy(fixture("linked-images/multitab-images.trz"), archive));
    const auto set = temporary.filePath("set.trk");
    motrack::WorkspaceWindow window;
    window.show();
    QSignalSpy opened(window.context().get(), &motrack::WorkspaceContext::inputOpened);
    ASSERT_TRUE(window.openProject(archive));
    ASSERT_TRUE(QTest::qWaitFor([&] { return opened.count() == 1; }));
    const auto& session = window.context()->session();
    const auto group = session.sources().front();
    ASSERT_TRUE(editMass(window, 21));
    {
        DialogScript script({answer("workspaceTabsetSaveQuestion", QMessageBox::Cancel)});
        EXPECT_FALSE(window.saveTabset(set));
        EXPECT_EQ(script.handled(), 1U);
    }
    EXPECT_FALSE(QFile::exists(set));
    EXPECT_TRUE(session.isModified(group));
    {
        // Not saving the changes lists the file as it was last saved, as Tracker lists every tab that has a file.
        DialogScript script({answer("workspaceTabsetSaveQuestion", QMessageBox::No)});
        EXPECT_TRUE(window.saveTabset(set));
        EXPECT_EQ(script.handled(), 1U);
    }
    auto* box = problems(window);
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->informativeText().contains("group.trz: listed as last saved")) << box->informativeText().toStdString();
    dismiss(window, box);
    EXPECT_TRUE(session.isModified(group));
    const auto listed = motrack::readTrackerTabset(set);
    ASSERT_TRUE(listed.has_value()) << listed.error();
    EXPECT_EQ(listed->entries.size(), 1U);
    ASSERT_TRUE(QFile::remove(set));
    {
        DialogScript script({answer("workspaceTabsetSaveQuestion", QMessageBox::Yes)});
        // The project is saved, and as in Tracker a tabset ends in .trk, so its companion is never replaced.
        EXPECT_TRUE(window.saveTabset(temporary.filePath("group.motrack")));
        EXPECT_EQ(script.handled(), 1U);
    }
    EXPECT_FALSE(session.isModified(group));
    EXPECT_TRUE(motrack::isTrackerTabsetFile(temporary.filePath("group.trk")));
    // An existing file that the .trk name points to is replaced only when chosen.
    EXPECT_FALSE(window.saveTabset(archive));
    // An existing experiment that is not open is never replaced by a tabset either.
    const auto experiment = temporary.filePath("experiment.trk");
    ASSERT_TRUE(QFile::copy(fixture("linked-images/linked-images.trk"), experiment));
    EXPECT_FALSE(window.saveTabset(experiment));
    EXPECT_FALSE(motrack::isTrackerTabsetFile(experiment));
    const auto saved = motrack::TrackerArchive::open(archive);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(saved.projectPath(saved.projects().front())).project.pointMasses.front().mass, 21);
    EXPECT_TRUE(motrack::readArchiveGroup(temporary.filePath("group.motrack")).members.size() == 2U);
    EXPECT_TRUE(window.saveTabset(set));
    EXPECT_TRUE(QFile::exists(set));
}

TEST(WorkspaceTabset, LocatedTabsetsOpenInPlaceAndLaterTabsetsStillOpen)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto nested = temporary.filePath("moved/inner.trk");
    ASSERT_TRUE(QDir().mkpath(temporary.filePath("moved")));
    writeTabset(nested,
                {absoluteEntry(fixture("linked-images/linked-images.trk")),
                 {.absolute = std::nullopt, .relative = "gone.trk"},
                 absoluteEntry(fixture("linked-mov/constant-rate.mov"))});
    const auto set = temporary.filePath("set.trk");
    writeTabset(set, {{.absolute = std::nullopt, .relative = "inner.trk"}, absoluteEntry(fixture("linked-mov/constant-rate.mov"))});
    motrack::WorkspaceWindow window;
    window.show();
    {
        DialogScript script({answer("workspaceTabsetMissing", QMessageBox::Yes), chooseFile(nested)});
        ASSERT_TRUE(openTabset(window, set, 2));
        EXPECT_EQ(script.handled(), 2U);
    }
    // The located tabset's experiment opens where that tabset was listed, before the following movie.
    const auto labels = tabLabels(window);
    EXPECT_EQ(labels[0], "linked-images.trk");
    EXPECT_EQ(labels[1], "constant-rate.mov");
    auto* box = problems(window);
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->informativeText().contains("gone.trk"));
    box->accept();
    // A file the parent already opened is not opened again by a located tabset.
    EXPECT_EQ(window.context()->session().sources().size(), 2U);
    // A later tabset is not blocked by the located one.
    const auto later = temporary.filePath("later.trk");
    writeTabset(later, {absoluteEntry(fixture("linked-images/secondary-images.trk"))});
    ASSERT_TRUE(openTabset(window, later, 3));
}

TEST(WorkspaceTabset, TabsetsArrivingTogetherOpenOneAfterAnother)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto first = temporary.filePath("first.trk");
    const auto second = temporary.filePath("second.trk");
    writeTabset(first, {{.absolute = std::nullopt, .relative = "missing.trk"}, absoluteEntry(fixture("linked-images/linked-images.trk"))});
    writeTabset(second, {absoluteEntry(fixture("linked-images/secondary-images.trk"))});
    motrack::WorkspaceWindow window;
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::tabsetFinished);
    QSignalSpy expanded(window.context().get(), &motrack::WorkspaceContext::tabsetOpened);
    // Open the second tabset from inside the first one's prompt and keep the prompt up until the context has
    // delivered both expansions, so the second always arrives during the first one's nested event loop.
    bool secondRequested = false;
    DialogScript script({[&](QWidget* widget)
                         {
                             auto* box = qobject_cast<QMessageBox*>(widget);
                             if (!box || box->objectName() != "workspaceTabsetMissing")
                                 return false;
                             if (!std::exchange(secondRequested, true))
                                 EXPECT_TRUE(window.openProject(second));
                             if (expanded.count() < 2)
                                 return false;
                             box->button(QMessageBox::No)->click();
                             return true;
                         }});
    ASSERT_TRUE(window.openProject(first));
    ASSERT_TRUE(QTest::qWaitFor([&] { return finished.count() == 2; }, 20000));
    EXPECT_EQ(finished.at(0).at(0).toString(), canonical(first));
    EXPECT_EQ(finished.at(1).at(0).toString(), canonical(second));
    EXPECT_EQ(tabLabels(window), (std::vector<QString>{"linked-images.trk", "secondary-images.trk"}));
}

TEST(WorkspaceTabset, FailedFilesAreReportedWhileTheOthersOpen)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto broken = temporary.filePath("broken.trk");
    QFile file(broken);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_GT(file.write("not Tracker XML"), 0);
    file.close();
    const auto set = temporary.filePath("set.trk");
    writeTabset(set, {listedEntry(broken, set), listedEntry(fixture("linked-images/linked-images.trk"), set)});
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openTabset(window, set, 1));
    auto* box = problems(window);
    ASSERT_NE(box, nullptr);
    // The listed file's line is B1's import diagnostic, as a single-project window reports it.
    EXPECT_TRUE(box->informativeText().contains("broken.trk: Invalid Tracker XML: No OSP root object"))
        << box->informativeText().toStdString();
    dismiss(window, box);
    // Saving the set back does not drop the file that failed to open.
    const auto bytes = readBytes(set);
    ASSERT_TRUE(window.saveTabset(set));
    box = problems(window);
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->informativeText().contains("broken.trk: not open")) << box->informativeText().toStdString();
    dismiss(window, box);
    EXPECT_EQ(readBytes(set), bytes);
}

TEST(WorkspaceTabset, CancelingAnOpenStopsTheRemainingFiles)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto set = temporary.filePath("set.trk");
    writeTabset(set,
                {absoluteEntry(fixture("linked-images/linked-images.trk")), absoluteEntry(fixture("linked-images/secondary-images.trk"))});
    auto context = std::make_shared<motrack::WorkspaceContext>();
    motrack::WorkspaceWindow window(context);
    window.show();
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::tabsetFinished);
    QSignalSpy expanded(context.get(), &motrack::WorkspaceContext::tabsetOpened);
    // Hold both analysis workers after the expansion so the first listed open stays pending, then cancel it.
    QSemaphore entered;
    QSemaphore release;
    auto& pool = context->services()->analysisPool();
    const auto cleanup = qScopeGuard(
        [&]
        {
            release.release(2);
            (void)pool.waitForDone(5000);
        });
    ASSERT_TRUE(window.openProject(set));
    ASSERT_TRUE(QTest::qWaitFor([&] { return expanded.count() == 1; }));
    for (int i = 0; i < 2; ++i)
        pool.start(
            [&]
            {
                entered.release();
                (void)release.tryAcquire(1, 5000);
            });
    ASSERT_TRUE(entered.tryAcquire(2, 2000));
    auto* pending = window.findChild<QListWidget*>("pendingWorkspaceInputs");
    ASSERT_TRUE(QTest::qWaitFor([&] { return pending->count() == 1; }));
    ASSERT_TRUE(context->cancelOpen(pending->item(0)->data(Qt::UserRole).toUuid()));
    release.release(2);
    ASSERT_TRUE(QTest::qWaitFor([&] { return finished.count() == 1; }));
    EXPECT_TRUE(context->session().sources().empty());
    auto* box = problems(window);
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->informativeText().contains("remaining"));
    box->accept();
}

TEST(WorkspaceTabset, DestroyingTheWindowDuringALocatePromptIsSafe)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto set = temporary.filePath("set.trk");
    writeTabset(set, {{.absolute = std::nullopt, .relative = "missing.trk"}, absoluteEntry(fixture("linked-images/linked-images.trk"))});
    auto context = std::make_shared<motrack::WorkspaceContext>();
    auto window = std::make_unique<motrack::WorkspaceWindow>(context);
    window->show();
    DialogScript script({[&](QWidget* widget)
                         {
                             if (!qobject_cast<QMessageBox*>(widget) || widget->objectName() != "workspaceTabsetMissing")
                                 return false;
                             window.reset();
                             return true;
                         }});
    ASSERT_TRUE(window->openProject(set));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !window; }));
    // Every open that could have started has finished and delivered its result.
    ASSERT_TRUE(context->services()->analysisPool().waitForDone(10000));
    QCoreApplication::processEvents();
    // The remaining file never opens once its owner is gone.
    EXPECT_TRUE(context->session().sources().empty());
}

TEST(WorkspaceTabset, NativeOnlyAndChangedMediaSourcesAreReportedInsteadOfReferenced)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    auto input = motrack::prepareWorkspaceInput(fixture("linked-images/linked-images.trk"));
    auto document = input.document->document;
    const auto native = temporary.filePath("native.motrack");
    motrack::rebaseWorkspaceMedia(document, input.mediaProjectPath, native);
    motrack::writeWorkspaceDocument(native, document);
    const auto movie = temporary.filePath("clip.mov");
    ASSERT_TRUE(QFile::copy(fixture("linked-mov/constant-rate.mov"), movie));
    motrack::WorkspaceWindow window;
    window.show();
    QSignalSpy opened(window.context().get(), &motrack::WorkspaceContext::inputOpened);
    ASSERT_TRUE(window.openProject(native));
    ASSERT_TRUE(QTest::qWaitFor([&] { return opened.count() == 1; }));
    ASSERT_TRUE(window.openMovie(movie));
    ASSERT_TRUE(QTest::qWaitFor([&] { return opened.count() == 2; }));
    const auto set = temporary.filePath("set.trk");
    ASSERT_TRUE(window.saveTabset(set));
    auto* box = problems(window);
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->informativeText().contains("native.motrack"));
    box->accept();
    QFile file(set);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto tabset = motrack::parseTrackerTabset(file.readAll().toStdString());
    ASSERT_TRUE(tabset.has_value());
    ASSERT_EQ(tabset->entries.size(), 1U);
    EXPECT_EQ(tabset->entries.front().relative, "clip.mov");
}

TEST(WorkspaceTabset, SavingOverTheOpenedTabsetKeepsItsBytesAndAddsOnlyNewTabs)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto set = temporary.filePath("lesson.trk");
    // As Tracker on Windows writes a tabset (CRLF, no final line break), with a property Tracker ignores.
    auto xml = motrack::writeTrackerTabset(std::vector{listedEntry(fixture("linked-images/linked-images.trk"), set),
                                                       listedEntry(fixture("linked-mov/constant-rate.mov"), set)});
    ASSERT_TRUE(xml.has_value());
    auto original = QByteArray::fromStdString(*xml).replace("\n", "\r\n");
    original.replace("    <property name=\"tabs\"",
                     "    <property name=\"note\" type=\"string\">kept</property>\r\n    <property name=\"tabs\"");
    {
        QFile file(set);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(original), original.size());
    }
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openTabset(window, set, 2));
    // Saving the unchanged set back changes nothing.
    ASSERT_TRUE(window.saveTabset(set));
    EXPECT_EQ(readBytes(set), original);

    // Save tabset as… offers the tabset opened last.
    QString offered;
    {
        DialogScript script({[&](QWidget* widget)
                             {
                                 auto* dialog = qobject_cast<QFileDialog*>(widget);
                                 if (!dialog || dialog->objectName() != "workspaceTabsetSaveDialog")
                                     return false;
                                 offered = dialog->selectedFiles().value(0);
                                 dialog->reject();
                                 return true;
                             }});
        EXPECT_FALSE(window.saveTabset());
        EXPECT_EQ(script.handled(), 1U);
    }
    EXPECT_EQ(canonical(offered).toStdString(), canonical(set).toStdString());

    // Another experiment adds one entry; the listed ones, the other property and the line breaks keep their bytes.
    QSignalSpy opened(window.context().get(), &motrack::WorkspaceContext::inputOpened);
    ASSERT_TRUE(window.openProject(fixture("linked-images/secondary-images.trk")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return opened.count() == 1; }));
    ASSERT_TRUE(window.saveTabset(set));
    const auto edited = readBytes(set);
    const auto kept = original.left(original.lastIndexOf("    </property>\r\n</object>"));
    EXPECT_TRUE(edited.startsWith(kept)) << edited.toStdString();
    EXPECT_TRUE(edited.endsWith("    </property>\r\n</object>")) << edited.toStdString();
    EXPECT_FALSE(edited.contains("\n\n"));
    EXPECT_EQ(edited.count('\n'), edited.count("\r\n"));
    const auto reread = motrack::parseTrackerTabset(edited.toStdString());
    ASSERT_TRUE(reread.has_value()) << reread.error();
    ASSERT_EQ(reread->entries.size(), 3U);
    EXPECT_EQ(reread->entries[2].absolute,
              QDir::fromNativeSeparators(canonical(fixture("linked-images/secondary-images.trk"))).toStdString());

    // A file the user closes leaves the saved set; the others keep their entries.
    auto& session = window.context()->session();
    const auto open = session.sources();
    const auto movie = std::ranges::find_if(open, [&](QUuid source) { return session.sourcePath(source).endsWith("constant-rate.mov"); });
    ASSERT_NE(movie, open.end());
    ASSERT_TRUE(window.closeSource(*movie));
    ASSERT_TRUE(window.saveTabset(set));
    auto* box = problems(window);
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->informativeText().contains("constant-rate.mov: closed, so the tabset no longer lists it."))
        << box->informativeText().toStdString();
    dismiss(window, box);
    const auto closed = motrack::readTrackerTabset(set);
    ASSERT_TRUE(closed.has_value()) << closed.error();
    ASSERT_EQ(closed->entries.size(), 2U);
    EXPECT_EQ(closed->entries[0], reread->entries[0]);
    EXPECT_EQ(closed->entries[1], reread->entries[2]);

    // A tabset MoTrack cannot read is not replaced.
    const auto unreadable = temporary.filePath("unreadable.trk");
    const QByteArray refused = "<object class=\"org.opensourcephysics.cabrillo.tracker.TFrame\">\n"
                               "    <property name=\"tabs\" type=\"string\">a.trk</property>\n</object>\n";
    {
        QFile file(unreadable);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(refused), refused.size());
    }
    EXPECT_FALSE(window.saveTabset(unreadable));
    EXPECT_EQ(readBytes(unreadable), refused);
}

TEST(WorkspaceTabset, NativeMembersWithResetTrackerLinksOpenWithTheirNoticeAndStayOutOfSavedSets)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    auto input = motrack::prepareWorkspaceInput(fixture("linked-images/linked-images.trk"));
    auto document = input.document->document;
    const auto native = temporary.filePath("stale.motrack");
    motrack::rebaseWorkspaceMedia(document, input.mediaProjectPath, native);
    motrack::writeWorkspaceDocument(native, document);
    // Associations that no longer name the source's track objects, as a native file saved before B1 can hold. B1 drops
    // them on open, keeps the measurements and refuses Tracker export.
    {
        auto root = QJsonDocument::fromJson(readBytes(native)).object();
        auto source = root.value("trackerSource").toObject();
        source.insert("bindings", QJsonArray{});
        root.insert("trackerSource", source);
        QFile file(native);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_GT(file.write(QJsonDocument(root).toJson()), 0);
    }
    ASSERT_TRUE(motrack::readWorkspaceDocument(native).trackerBindingsReset);
    const auto set = temporary.filePath("set.trk");
    writeTabset(set, {absoluteEntry(native), absoluteEntry(fixture("linked-images/secondary-images.trk"))});
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openTabset(window, set, 2));
    auto* box = problems(window);
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->text().contains("cannot be saved as Tracker files")) << box->text().toStdString();
    EXPECT_TRUE(box->informativeText().contains("stale.motrack: Tracker export unavailable")) << box->informativeText().toStdString();
    dismiss(window, box);
    // The selected first experiment's view shows B1's persistent note too.
    const auto* view = tabs(window)->currentWidget();
    ASSERT_NE(view, nullptr);
    const auto* notice = view->findChild<QLabel*>("sourceNotice");
    ASSERT_NE(notice, nullptr);
    EXPECT_FALSE(notice->isHidden());

    // Saving the set leaves it out with the reason, instead of pointing to a Tracker export that is refused.
    const auto saved = temporary.filePath("saved.trk");
    ASSERT_TRUE(window.saveTabset(saved));
    box = problems(window);
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->informativeText().contains("stale.motrack: its links to the original Tracker tracks could not be restored"))
        << box->informativeText().toStdString();
    dismiss(window, box);
    const auto written = motrack::readTrackerTabset(saved);
    ASSERT_TRUE(written.has_value()) << written.error();
    ASSERT_EQ(written->entries.size(), 1U);
    EXPECT_TRUE(written->entries.front().relative.ends_with("secondary-images.trk"));
}

TEST(WorkspaceTabset, SavingWaitsForEveryOpenIncludingOnesStartedDuringItsPrompts)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto archive = temporary.filePath("group.trz");
    ASSERT_TRUE(QFile::copy(fixture("linked-images/multitab-images.trz"), archive));
    const auto set = temporary.filePath("set.trk");
    writeTabset(set, {{.absolute = std::nullopt, .relative = "group.trz"}, absoluteEntry(fixture("linked-images/linked-images.trk"))});
    const auto other = temporary.filePath("other.trk");
    writeTabset(other, {absoluteEntry(fixture("linked-images/secondary-images.trk"))});
    const auto bytes = readBytes(set);
    motrack::WorkspaceWindow window;
    window.show();
    ASSERT_TRUE(openTabset(window, set, 3));
    auto* saveAction = window.findChild<QAction*>("saveTrackerTabset");
    ASSERT_NE(saveAction, nullptr);
    EXPECT_TRUE(saveAction->isEnabled());

    // Any open in progress may turn out to be a tabset, so saving waits for it.
    QSignalSpy finished(&window, &motrack::WorkspaceWindow::tabsetFinished);
    ASSERT_TRUE(window.openProject(other));
    EXPECT_FALSE(saveAction->isEnabled());
    EXPECT_FALSE(window.saveTabset(set));
    ASSERT_TRUE(QTest::qWaitFor([&] { return finished.count() == 1; }, 20000));
    EXPECT_TRUE(saveAction->isEnabled());
    EXPECT_EQ(readBytes(set), bytes);

    // An open that starts during one of Save tabset as…'s prompts stops the save after that prompt.
    auto& session = window.context()->session();
    const auto open = session.sources();
    const auto group = std::ranges::find_if(open, [&](QUuid source) { return session.sourcePath(source).endsWith("group.trz"); });
    ASSERT_NE(group, open.end());
    ASSERT_TRUE(window.showDocument(session.sourceDocuments(*group).front()));
    ASSERT_TRUE(editMass(window, 7));
    bool started = false;
    {
        DialogScript script({[&](QWidget* widget)
                             {
                                 if (widget->objectName() != "workspaceTabsetSaveQuestion")
                                     return false;
                                 started = window.openProject(other).has_value();
                                 return answer("workspaceTabsetSaveQuestion", QMessageBox::No)(widget);
                             }});
        EXPECT_FALSE(window.saveTabset(set));
        EXPECT_EQ(script.handled(), 1U);
    }
    EXPECT_TRUE(started);
    EXPECT_EQ(readBytes(set), bytes);
    ASSERT_TRUE(QTest::qWaitFor([&] { return finished.count() == 2; }, 20000));
}

TEST(WorkspaceTabset, AFileClosedEarlierStaysInASetThatListsItEvenIfItThenFailsToOpen)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto clip = temporary.filePath("clip.mov");
    ASSERT_TRUE(QFile::copy(fixture("linked-mov/constant-rate.mov"), clip));
    ASSERT_TRUE(QFile::setPermissions(clip, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    motrack::WorkspaceWindow window;
    window.show();
    QSignalSpy opened(window.context().get(), &motrack::WorkspaceContext::inputOpened);
    ASSERT_TRUE(window.openMovie(clip));
    ASSERT_TRUE(QTest::qWaitFor([&] { return opened.count() == 1; }));
    auto& session = window.context()->session();
    ASSERT_TRUE(window.closeSource(session.sources().front()));
    // The movie is damaged after it was closed; a set that lists it then cannot open it.
    {
        QFile file(clip);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_GT(file.write("not a movie"), 0);
    }
    const auto set = temporary.filePath("set.trk");
    writeTabset(set, {listedEntry(clip, set), listedEntry(fixture("linked-images/linked-images.trk"), set)});
    const auto bytes = readBytes(set);
    ASSERT_TRUE(openTabset(window, set, 1));
    auto* box = problems(window);
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->informativeText().contains("clip.mov")) << box->informativeText().toStdString();
    dismiss(window, box);
    // Saving the set back keeps the movie's entry: listing it again undid the earlier close.
    ASSERT_TRUE(window.saveTabset(set));
    box = problems(window);
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->informativeText().contains("clip.mov: not open, so the tabset still lists it as before."))
        << box->informativeText().toStdString();
    dismiss(window, box);
    EXPECT_EQ(readBytes(set), bytes);
}

} // namespace
