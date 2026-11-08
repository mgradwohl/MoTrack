#include "image_sequence.hpp"
#include "main_window.hpp"
#include "paired_save.hpp"
#include "tracker_archive.hpp"
#include "workspace_document.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSemaphore>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <gtest/gtest.h>

#include <stdexcept>

namespace
{
QString fixture(const QString& name = "linked-images.trk")
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/" + name;
}

QByteArray bytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Could not read paired workspace output.");
    return file.readAll();
}

void put(const QString& path, const QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        throw std::runtime_error("Could not write paired workspace fixture.");
}

bool editMass(motrack::MainWindow& window, double value)
{
    auto* action = window.findChild<QAction*>("setPointMassValue");
    if (!action || !action->isEnabled())
        return false;
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
    bool accepted = false;
    QTimer::singleShot(0,
                       &window,
                       [&]
                       {
                           if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                           {
                               dialog->setTextValue(QLocale().toString(value));
                               accepted = true;
                               dialog->accept();
                           }
                       });
    action->trigger();
    return accepted;
}

TEST(PairedWorkspace, SavesEditedLinkedAndArchivedProjectsAndUsesThePairForOrdinarySave)
{
    for (const bool archive : {false, true})
    {
        SCOPED_TRACE(archive);
        QTemporaryDir temporary;
        motrack::MainWindow window;
        auto* paired = window.findChild<QAction*>("saveTrackerProject");
        ASSERT_NE(paired, nullptr);
        EXPECT_FALSE(paired->isEnabled());
        EXPECT_FALSE(window.saveTrackerProject(temporary.filePath("empty.trk")));
        ASSERT_TRUE(window.loadProject(fixture(archive ? "archived-images.trz" : "linked-images.trk")));
        EXPECT_TRUE(paired->isEnabled());
        ASSERT_TRUE(editMass(window, 5));
        const auto target = temporary.filePath(archive ? "saved.trz" : "saved.trk");
        ASSERT_TRUE(window.saveTrackerProject(target));
        EXPECT_FALSE(window.isWindowModified());
        const auto nativePath = temporary.filePath("saved.motrack");
        const auto native = motrack::readWorkspaceDocument(nativePath);
        EXPECT_DOUBLE_EQ(native.project.pointMasses.at(0).mass, 5);
        if (archive)
        {
            const auto standard = motrack::TrackerArchive::open(target);
            EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(standard.projectPath("experiment.trk")).project.pointMasses.at(0).mass, 5);
            ASSERT_TRUE(native.archivePayload);
            EXPECT_EQ(bytes(native.archivePayload->sourcePath()), bytes(fixture("archived-images.trz")));
        }
        else
        {
            EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 5);
            const auto sequence = motrack::ImageSequence::open(nativePath, native.project.clip);
            if (!sequence)
                FAIL() << "The native companion lost its image sequence.";
            const auto frame = sequence->readStep(0);
            if (!frame)
                FAIL() << "The native companion lost its first frame.";
            EXPECT_EQ(frame->image, QImage(fixture("images/frame0.png")));
        }
        ASSERT_TRUE(editMass(window, 7));
        auto* save = window.findChild<QAction*>("saveProject");
        ASSERT_NE(save, nullptr);
        save->trigger();
        EXPECT_FALSE(window.isWindowModified());
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(nativePath).project.pointMasses.at(0).mass, 7);
        ASSERT_TRUE(window.loadProject(nativePath));
        auto* canvas = window.findChild<QWidget*>("videoCanvas");
        ASSERT_NE(canvas, nullptr);
        EXPECT_EQ(canvas->accessibleName(), "Frame 0");
    }
}

TEST(PairedWorkspace, RecoversAnOlderPairThenSavesCurrentEdits)
{
    QTemporaryDir temporary;
    const auto target = temporary.filePath("saved.trk");
    auto earlier = motrack::importTrackerDocument(fixture());
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(earlier.project, 0, 5));
    std::atomic_bool cancelled{false};
    const auto partial =
        motrack::writePairedDocuments(target,
                                      earlier,
                                      fixture(),
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    ASSERT_TRUE(editMass(window, 7));
    EXPECT_FALSE(window.saveTrackerProject(target));
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_FALSE(window.saveTrackerProject(target)); // Recovery requires an explicit request.
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 5);
    ASSERT_TRUE(window.saveTrackerProject(target, true));
    EXPECT_FALSE(window.isWindowModified());
    EXPECT_FALSE(QFile::exists(partial.recoveryPath));
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 7);
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(partial.nativePath).project.pointMasses.at(0).mass, 7);
}

TEST(PairedWorkspace, CancellationPreservesBothExistingFilesAndNewerEdits)
{
    QTemporaryDir temporary;
    QSemaphore entered;
    QSemaphore release;
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    ASSERT_TRUE(editMass(window, 5));
    const auto target = temporary.filePath("saved.trk");
    const auto native = temporary.filePath("saved.motrack");
    put(target, "previous standard");
    put(native, "previous native");
    auto* pool = window.findChild<QThreadPool*>("workspaceSavePool");
    ASSERT_NE(pool, nullptr);
    pool->start(
        [&]
        {
            entered.release();
            static_cast<void>(release.tryAcquire(1, 5000));
        });
    ASSERT_TRUE(entered.tryAcquire(1, 5000));
    bool clicked = false;
    QTimer::singleShot(0,
                       &window,
                       [&]
                       {
                           if (auto* cancel = window.findChild<QPushButton*>("cancelPairedSave"))
                           {
                               cancel->click();
                               clicked = true;
                           }
                           EXPECT_FALSE(window.saveTrackerProject(target));
                           EXPECT_FALSE(window.close());
                           release.release();
                       });
    EXPECT_FALSE(window.saveTrackerProject(target));
    EXPECT_TRUE(clicked);
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_EQ(bytes(target), "previous standard");
    EXPECT_EQ(bytes(native), "previous native");
    EXPECT_TRUE(window.statusBar()->currentMessage().startsWith("Save canceled."));
    EXPECT_EQ(window.findChild<QDialog*>("pairedSaveProgress"), nullptr);
}

TEST(PairedWorkspace, StaleCompletionAndNativeOnlySaveLeaveStandardEditsDirty)
{
    QTemporaryDir temporary;
    QSemaphore entered;
    QSemaphore release;
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    auto* pool = window.findChild<QThreadPool*>("workspaceSavePool");
    auto* create = window.findChild<QAction*>("newPointMass");
    auto* undo = window.findChild<QAction*>("undoEdit");
    ASSERT_TRUE(pool && create && undo);
    pool->start(
        [&]
        {
            entered.release();
            static_cast<void>(release.tryAcquire(1, 5000));
        });
    ASSERT_TRUE(entered.tryAcquire(1, 5000));
    QTimer::singleShot(0,
                       &window,
                       [&]
                       {
                           create->trigger();
                           release.release();
                       });
    const auto target = temporary.filePath("saved.trk");
    EXPECT_FALSE(window.saveTrackerProject(target));
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_EQ(motrack::readWorkspaceDocument(temporary.filePath("saved.motrack")).project.pointMasses.size(), 1u);
    ASSERT_TRUE(window.saveWorkspace(temporary.filePath("newer-copy.motrack")));
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_EQ(motrack::readWorkspaceDocument(temporary.filePath("newer-copy.motrack")).project.pointMasses.size(), 2u);
    undo->trigger();
    EXPECT_TRUE(window.isWindowModified());
    ASSERT_TRUE(window.saveTrackerProject(target));
    EXPECT_FALSE(window.isWindowModified());
}

TEST(PairedWorkspace, RejectsUnsupportedEditsAndWrongDestinationsWithoutReplacingFiles)
{
    QTemporaryDir temporary;
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    const auto target = temporary.filePath("saved.trk");
    const auto native = temporary.filePath("saved.motrack");
    put(target, "previous standard");
    put(native, "previous native");
    EXPECT_FALSE(window.saveTrackerProject(temporary.filePath("wrong.trz")));
    EXPECT_FALSE(window.saveTrackerProject(temporary.filePath("missing/saved.trk")));
    ASSERT_TRUE(window.loadProject(QString::fromUtf8(MOTRACK_FIXTURES) + "/movie.trk"));
    ASSERT_TRUE(editMass(window, 5));
    EXPECT_FALSE(window.saveTrackerProject(target));
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_EQ(bytes(target), "previous standard");
    EXPECT_EQ(bytes(native), "previous native");
}

TEST(PairedWorkspace, FileMenuAsksBeforeReplacingAnUnrelatedCompanion)
{
    for (const bool replace : {false, true})
    {
        SCOPED_TRACE(replace);
        QTemporaryDir temporary;
        motrack::MainWindow window;
        window.show();
        ASSERT_TRUE(window.loadProject(fixture()));
        const auto target = temporary.filePath("saved.trk");
        const auto native = temporary.filePath("saved.motrack");
        put(native, "unrelated companion");
        auto* action = window.findChild<QAction*>("saveTrackerProject");
        ASSERT_NE(action, nullptr);
        QTimer poll;
        QTimer watchdog;
        bool choseFile = false;
        bool choseCompanion = false;
        QObject::connect(&poll,
                         &QTimer::timeout,
                         &window,
                         [&]
                         {
                             if (!choseFile)
                             {
                                 if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                                 {
                                     choseFile = true;
                                     dialog->selectFile(target);
                                     QMetaObject::invokeMethod(dialog, "accept");
                                 }
                             }
                             else if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                             {
                                 choseCompanion = true;
                                 message->button(replace ? QMessageBox::Yes : QMessageBox::Cancel)->click();
                                 poll.stop();
                             }
                         });
        QObject::connect(&watchdog,
                         &QTimer::timeout,
                         &window,
                         []
                         {
                             if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                                 dialog->reject();
                         });
        poll.start(10);
        watchdog.setSingleShot(true);
        watchdog.start(3000);
        action->trigger();
        EXPECT_TRUE(choseFile && choseCompanion);
        if (replace)
        {
            EXPECT_TRUE(QFile::exists(target));
            EXPECT_EQ(motrack::readWorkspaceDocument(native).project.pointMasses.size(), 1u);
        }
        else
        {
            EXPECT_FALSE(QFile::exists(target));
            EXPECT_EQ(bytes(native), "unrelated companion");
        }
    }
}

TEST(PairedWorkspace, DestroyingTheWindowDuringSaveCancelsAndReleasesTheWorker)
{
    QTemporaryDir temporary;
    QSemaphore entered;
    QSemaphore release;
    auto* window = new motrack::MainWindow;
    ASSERT_TRUE(window->loadProject(fixture()));
    auto* pool = window->findChild<QThreadPool*>("workspaceSavePool");
    ASSERT_NE(pool, nullptr);
    pool->start(
        [&]
        {
            entered.release();
            static_cast<void>(release.tryAcquire(1, 5000));
        });
    ASSERT_TRUE(entered.tryAcquire(1, 5000));
    QTimer::singleShot(0,
                       [window, &release]
                       {
                           release.release();
                           delete window;
                       });
    EXPECT_FALSE(window->saveTrackerProject(temporary.filePath("saved.trk")));
}
} // namespace
