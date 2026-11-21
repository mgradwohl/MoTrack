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
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <gtest/gtest.h>

#include <functional>
#include <ranges>
#include <string>
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
                                 ++m_Next;
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
        m_Timeout.start(5000);
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
                    absoluteEntry(experiment),
                    {.absolute = std::nullopt, .relative = "set.trk"},
                    {.absolute = std::nullopt, .relative = "gone.trk"},
                });
    motrack::WorkspaceWindow window;
    window.show();
    DialogScript script(
        {answer("workspaceTabsetMissing", QMessageBox::Yes), chooseFile(experiment), answer("workspaceTabsetMissing", QMessageBox::No)});
    ASSERT_TRUE(openTabset(window, set, 1));
    EXPECT_EQ(script.handled(), 3U);
    // The located file is also the next entry; each file opens once, unlike Tracker.
    EXPECT_EQ(window.context()->session().sources().size(), 1U);
    EXPECT_EQ(tabs(window)->count(), 1);
    auto* box = problems(window);
    ASSERT_NE(box, nullptr);
    const auto text = box->informativeText();
    EXPECT_TRUE(text.contains("set.trk")) << text.toStdString();
    EXPECT_TRUE(text.contains("gone.trk")) << text.toStdString();
    box->accept();
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
    const auto entries = motrack::parseTrackerTabset(file.readAll().toStdString());
    file.close(); // Windows refuses to move an open file below.
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 2U);
    EXPECT_EQ((*entries)[0].relative, "../group.trz");
    EXPECT_EQ((*entries)[0].absolute, QDir::fromNativeSeparators(canonical(archive)).toStdString());
    EXPECT_EQ((*entries)[1].relative, "../clip.mov");
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
        // Skipping the only project leaves nothing to reference.
        DialogScript script({answer("workspaceTabsetSaveQuestion", QMessageBox::No)});
        EXPECT_FALSE(window.saveTabset(set));
        EXPECT_EQ(script.handled(), 1U);
    }
    EXPECT_FALSE(QFile::exists(set));
    auto* box = problems(window);
    ASSERT_NE(box, nullptr);
    box->accept();
    {
        DialogScript script({answer("workspaceTabsetSaveQuestion", QMessageBox::Yes)});
        // The project is saved, but a tabset must never replace an open experiment or its companion.
        EXPECT_FALSE(window.saveTabset(temporary.filePath("group.motrack")));
        EXPECT_EQ(script.handled(), 1U);
    }
    EXPECT_FALSE(session.isModified(group));
    EXPECT_FALSE(window.saveTabset(archive));
    const auto saved = motrack::TrackerArchive::open(archive);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(saved.projectPath(saved.projects().front())).project.pointMasses.front().mass, 21);
    EXPECT_TRUE(motrack::readArchiveGroup(temporary.filePath("group.motrack")).members.size() == 2U);
    EXPECT_TRUE(window.saveTabset(set));
    EXPECT_TRUE(QFile::exists(set));
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
    const auto entries = motrack::parseTrackerTabset(file.readAll().toStdString());
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 1U);
    EXPECT_EQ(entries->front().relative, "clip.mov");
}

} // namespace
