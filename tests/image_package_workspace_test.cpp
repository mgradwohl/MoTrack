#include "image_sequence.hpp"
#include "main_window.hpp"
#include "paired_save.hpp"
#include "tracker_archive.hpp"
#include "tracker_writer.hpp"
#include "workspace_document.hpp"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLocale>
#include <QLockFile>
#include <QPushButton>
#include <QSemaphore>
#include <QSlider>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <utility>

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

TEST(ImagePackageWorkspace, ExportsIndexedMovWithoutChangingSaveStateAndReopensWithoutOriginalMedia)
{
    QTemporaryDir temporary;
    const auto movieRoot = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-mov/";
    const auto source = temporary.filePath("input.trk");
    const auto movie = temporary.filePath("constant-rate.mov");
    const auto native = temporary.filePath("saved.motrack");
    const auto target = temporary.filePath(QString::fromUtf8("portable é & movie.trz"));
    ASSERT_TRUE(QFile::copy(movieRoot + "linked-movie.trk", source));
    ASSERT_TRUE(QFile::copy(movieRoot + "constant-rate.mov", movie));
    {
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(source));
        auto* action = window.findChild<QAction*>("exportTrackerArchive");
        ASSERT_NE(action, nullptr);
        ASSERT_TRUE(QTest::qWaitFor([action] { return action->isEnabled(); }));
        ASSERT_TRUE(window.saveWorkspace(native));
        const auto saved = bytes(native);
        const auto title = window.windowTitle();
        ASSERT_TRUE(QFile::remove(source));
        ASSERT_TRUE(editMass(window, 5));
        ASSERT_TRUE(window.exportTrackerArchive(target));
        EXPECT_TRUE(window.isWindowModified());
        EXPECT_EQ(window.windowTitle(), title);
        EXPECT_EQ(bytes(native), saved);
        ASSERT_NE(window.findChild<QAction*>("undoEdit"), nullptr);
        ASSERT_NE(window.findChild<QAction*>("redoEdit"), nullptr);
        ASSERT_NE(window.findChild<QAction*>("saveProject"), nullptr);
        window.findChild<QAction*>("undoEdit")->trigger();
        window.findChild<QAction*>("saveProject")->trigger();
        EXPECT_FALSE(window.isWindowModified());
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(native).project.pointMasses.at(0).mass, 2);
        window.findChild<QAction*>("redoEdit")->trigger();
        ASSERT_TRUE(window.exportTrackerArchive(target));
        EXPECT_TRUE(window.isWindowModified());
    }
    ASSERT_TRUE(QFile::remove(movie));
    ASSERT_TRUE(QFile::remove(native));
    ASSERT_TRUE(QDir(temporary.path()).mkdir("moved"));
    const auto moved = temporary.filePath("moved/portable.trz");
    ASSERT_TRUE(QFile::rename(target, moved));
    const auto archive = motrack::TrackerArchive::open(moved);
    EXPECT_EQ(bytes(QDir(archive.root()).filePath("media/movie.mov")), bytes(movieRoot + "constant-rate.mov"));
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(archive.projectPath("experiment.trk")).project.pointMasses.at(0).mass, 5);
    motrack::MainWindow reopened;
    ASSERT_TRUE(reopened.loadProject(moved));
    auto* canvas = reopened.findChild<QWidget*>("videoCanvas");
    auto* slider = reopened.findChild<QSlider*>("stepSlider");
    auto* action = reopened.findChild<QAction*>("exportTrackerArchive");
    ASSERT_TRUE(canvas && slider && action);
    EXPECT_FALSE(action->isEnabled()); // Existing archives use paired Save, not new-package export.
    slider->setValue(3);
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 3"; }));
    ASSERT_TRUE(editMass(reopened, 7));
    ASSERT_TRUE(reopened.saveTrackerProject(moved));
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(temporary.filePath("moved/portable.motrack")).project.pointMasses.at(0).mass, 7);
}

TEST(ImagePackageWorkspace, MovieExportCancellationAndUnsupportedContainersPreserveTheDestination)
{
    QTemporaryDir temporary;
    const auto movieRoot = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/";
    const auto target = temporary.filePath("portable.trz");
    put(target, "existing archive");
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(movieRoot + "linked-movie/linked-movie.trk"));
    auto* action = window.findChild<QAction*>("exportTrackerArchive");
    auto* plot = window.findChild<QWidget*>("positionPlot");
    ASSERT_TRUE(action && plot);
    ASSERT_TRUE(QTest::qWaitFor([plot] { return plot->accessibleName().endsWith("Time (s)"); }));
    EXPECT_FALSE(action->isEnabled());
    EXPECT_FALSE(window.exportTrackerArchive(target));
    ASSERT_TRUE(window.loadProject(movieRoot + "linked-mov/linked-movie.trk"));
    ASSERT_TRUE(QTest::qWaitFor([action] { return action->isEnabled(); }));
    ASSERT_TRUE(editMass(window, 5));
    auto* pool = &window.executionServices().savePool();
    ASSERT_NE(pool, nullptr);
    QSemaphore entered;
    QSemaphore release;
    pool->start(
        [&]
        {
            entered.release();
            static_cast<void>(release.tryAcquire(1, 5000));
        });
    ASSERT_TRUE(entered.tryAcquire(1, 5000));
    bool canceled = false;
    QTimer::singleShot(0,
                       &window,
                       [&]
                       {
                           if (auto* cancel = window.findChild<QPushButton*>("cancelImageArchiveExport"))
                           {
                               cancel->click();
                               canceled = true;
                           }
                           release.release();
                       });
    EXPECT_FALSE(window.exportTrackerArchive(target));
    EXPECT_TRUE(canceled);
    EXPECT_EQ(bytes(target), "existing archive");
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_FALSE(QFileInfo::exists(temporary.filePath("portable.motrack")));
    ASSERT_TRUE(pool->waitForDone(5000));
}

TEST(ImagePackageWorkspace, PreservesPendingPairedArchiveAndCompanionUntilExplicitRecovery)
{
    for (const auto& stem : {QString("saved"), QString(240, 'a'), QString::fromUtf8("é").repeated(120)})
    {
        SCOPED_TRACE(stem.toStdString());
        QTemporaryDir temporary;
        const auto target = temporary.filePath(stem + ".trz");
        auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(fixture("archived-images.trz")));
        auto document = motrack::importTrackerDocument(payload->projectPath("experiment.trk"));
        payload->resolveMedia(document.project, "experiment.trk");
        document.archivePayload = payload;
        document.archiveMember = "experiment.trk";
        std::atomic_bool cancelled{false};
        const auto partial =
            motrack::writePairedDocuments(target,
                                          document,
                                          fixture(),
                                          &cancelled,
                                          [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
        ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
        const auto archiveBytes = bytes(target);
        const auto journalBytes = bytes(partial.recoveryPath);
        EXPECT_FALSE(QFileInfo::exists(partial.nativePath));
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(fixture()));
        ASSERT_TRUE(editMass(window, 7));
        EXPECT_FALSE(window.exportTrackerArchive(target));
        EXPECT_TRUE(window.statusBar()->currentMessage().contains("Use Save Tracker project"));
        EXPECT_EQ(bytes(target), archiveBytes);
        EXPECT_EQ(bytes(partial.recoveryPath), journalBytes);
        EXPECT_FALSE(QFileInfo::exists(partial.nativePath));
        EXPECT_TRUE(window.isWindowModified());
        ASSERT_TRUE(window.exportTrackerArchive(temporary.filePath("copy.trz")));
        EXPECT_EQ(bytes(target), archiveBytes);
        ASSERT_EQ(motrack::recoverPairedDocuments(partial.recoveryPath, nullptr, {}, partial.transactionId).status,
                  motrack::PairedSaveStatus::Saved);
        const auto companion = bytes(partial.nativePath);
        ASSERT_TRUE(window.exportTrackerArchive(target));
        EXPECT_EQ(bytes(partial.nativePath), companion);
        EXPECT_TRUE(window.isWindowModified());
        const auto exported = motrack::TrackerArchive::open(target);
        EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(exported.projectPath("experiment.trk")).project.pointMasses.front().mass, 7);
    }
}

TEST(ImagePackageWorkspace, RefusesAnActiveSiblingSaveLockAndAllowsOtherDestinations)
{
    for (const auto& stem : {QString("saved"), QString(240, 'a'), QString::fromUtf8("é").repeated(120)})
    {
        SCOPED_TRACE(stem.toStdString());
        QTemporaryDir temporary;
        const auto target = temporary.filePath(stem + ".trz");
        put(target, "original archive");
        const auto lockName = stem == "saved" ? QString(".saved.motrack-save.lock") : stem + '~';
        QLockFile lock(temporary.filePath(lockName));
        lock.setStaleLockTime(0);
        ASSERT_TRUE(lock.tryLock(0));
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(fixture()));
        EXPECT_FALSE(window.exportTrackerArchive(target));
        EXPECT_TRUE(window.statusBar()->currentMessage().contains("Wait for it to finish and retry"));
        EXPECT_EQ(bytes(target), "original archive");
        ASSERT_TRUE(window.exportTrackerArchive(temporary.filePath("copy.trz")));
        lock.unlock();
        ASSERT_TRUE(window.exportTrackerArchive(target));
        EXPECT_FALSE(bytes(target).isEmpty());
    }
}

TEST(ImagePackageWorkspace, ExportKeepsDirtyStateUndoAndOrdinarySaveDestinations)
{
    for (const bool paired : {false, true})
    {
        SCOPED_TRACE(paired);
        QTemporaryDir temporary;
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(fixture()));
        const auto standard = temporary.filePath("saved.trk");
        const auto native = temporary.filePath("saved.motrack");
        ASSERT_TRUE(paired ? window.saveTrackerProject(standard) : window.saveWorkspace(native));
        const auto title = window.windowTitle();
        const auto originalNative = bytes(native);
        const auto target = temporary.filePath("portable.trz");
        auto* action = window.findChild<QAction*>("exportTrackerArchive");
        ASSERT_NE(action, nullptr);
        ASSERT_TRUE(action->isEnabled());
        ASSERT_TRUE(window.exportTrackerArchive(target));
        EXPECT_FALSE(window.isWindowModified());
        ASSERT_TRUE(editMass(window, 5));
        ASSERT_TRUE(window.exportTrackerArchive(target));
        EXPECT_TRUE(window.isWindowModified());
        EXPECT_EQ(window.windowTitle(), title);
        EXPECT_EQ(bytes(native), originalNative);
        const auto archive = motrack::TrackerArchive::open(target);
        const auto projectPath = archive.projectPath("experiment.trk");
        const auto document = motrack::importTrackerDocument(projectPath);
        EXPECT_DOUBLE_EQ(document.project.pointMasses.at(0).mass, 5);
        const auto sequence = motrack::ImageSequence::open(projectPath, document.project.clip);
        if (!sequence)
            FAIL() << "Exported image sequence could not be reopened.";
        const auto frame = sequence->readStep(0);
        if (!frame)
            FAIL() << "Exported image sequence has no first frame.";
        EXPECT_EQ(frame->image, QImage(fixture("images/frame0.png")));
        window.findChild<QAction*>("undoEdit")->trigger();
        EXPECT_FALSE(window.isWindowModified());
        window.findChild<QAction*>("redoEdit")->trigger();
        EXPECT_TRUE(window.isWindowModified());
        ASSERT_TRUE(editMass(window, 7));
        window.findChild<QAction*>("saveProject")->trigger();
        EXPECT_FALSE(window.isWindowModified());
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(native).project.pointMasses.at(0).mass, 7);
        if (paired)
            EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(standard).project.pointMasses.at(0).mass, 7);
        const auto portable = motrack::TrackerArchive::open(target);
        EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(portable.projectPath("experiment.trk")).project.pointMasses.at(0).mass, 5);
    }
}

TEST(ImagePackageWorkspace, UnsupportedInputsAndMissingImagesKeepExistingOutputs)
{
    QTemporaryDir temporary;
    const auto target = temporary.filePath("portable.trz");
    put(target, "existing archive");
    motrack::MainWindow window;
    auto* action = window.findChild<QAction*>("exportTrackerArchive");
    ASSERT_NE(action, nullptr);
    EXPECT_FALSE(action->isEnabled());
    EXPECT_FALSE(window.exportTrackerArchive(target));
    ASSERT_TRUE(window.loadProject(fixture("archived-images.trz")));
    EXPECT_FALSE(action->isEnabled());
    EXPECT_FALSE(window.exportTrackerArchive(target));
    ASSERT_TRUE(window.openImageSequence({fixture("images/frame0.png")}, 25));
    EXPECT_FALSE(action->isEnabled());
    EXPECT_FALSE(window.exportTrackerArchive(target));
    const auto input = temporary.filePath("missing-images.trk");
    ASSERT_TRUE(QFile::copy(fixture(), input));
    ASSERT_TRUE(window.loadProject(input));
    EXPECT_TRUE(action->isEnabled());
    EXPECT_FALSE(window.exportTrackerArchive(temporary.filePath("wrong.trk")));
    EXPECT_FALSE(window.exportTrackerArchive(target));
    EXPECT_EQ(bytes(target), "existing archive");
    EXPECT_FALSE(window.isWindowModified());
    EXPECT_TRUE(window.statusBar()->currentMessage().startsWith("Could not export the Tracker archive:"));
    EXPECT_TRUE(QDir::fromNativeSeparators(window.statusBar()->currentMessage()).contains(temporary.filePath("images/frame0.png")));
}

TEST(ImagePackageWorkspace, CancellationAndOverlappingSavesKeepTheOriginalFiles)
{
    QTemporaryDir temporary;
    QSemaphore entered;
    QSemaphore release;
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    ASSERT_TRUE(editMass(window, 5));
    const auto target = temporary.filePath("portable.trz");
    put(target, "existing archive");
    auto* pool = &window.executionServices().savePool();
    ASSERT_NE(pool, nullptr);
    pool->start(
        [&]
        {
            entered.release();
            static_cast<void>(release.tryAcquire(1, 5000));
        });
    ASSERT_TRUE(entered.tryAcquire(1, 5000));
    bool canceled = false;
    QTimer::singleShot(0,
                       &window,
                       [&]
                       {
                           if (auto* cancel = window.findChild<QPushButton*>("cancelImageArchiveExport"))
                           {
                               cancel->click();
                               canceled = true;
                           }
                           EXPECT_FALSE(window.exportTrackerArchive(target));
                           EXPECT_FALSE(window.saveTrackerProject(temporary.filePath("other.trk")));
                           EXPECT_FALSE(window.saveWorkspace(temporary.filePath("other.motrack")));
                           EXPECT_FALSE(window.close());
                           release.release();
                       });
    EXPECT_FALSE(window.exportTrackerArchive(target));
    EXPECT_TRUE(canceled);
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_EQ(bytes(target), "existing archive");
    EXPECT_EQ(window.statusBar()->currentMessage(), "Export canceled. Existing files were kept.");
    EXPECT_EQ(window.findChild<QDialog*>("imageArchiveProgress"), nullptr);
    EXPECT_TRUE(window.exportTrackerArchive(target));
    EXPECT_TRUE(window.isWindowModified());
}

TEST(ImagePackageWorkspace, ProgrammaticEditsDuringExportKeepTheirUnsavedState)
{
    QTemporaryDir temporary;
    QSemaphore entered;
    QSemaphore release;
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    ASSERT_TRUE(editMass(window, 5));
    auto* pool = &window.executionServices().savePool();
    ASSERT_NE(pool, nullptr);
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
                           EXPECT_TRUE(editMass(window, 7));
                           release.release();
                       });
    const auto target = temporary.filePath("snapshot.trz");
    ASSERT_TRUE(window.exportTrackerArchive(target));
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_EQ(window.statusBar()->currentMessage(), "Archive exported. The project changed during export.");
    const auto archive = motrack::TrackerArchive::open(target);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(archive.projectPath("experiment.trk")).project.pointMasses.at(0).mass, 5);
    const auto native = temporary.filePath("current.motrack");
    ASSERT_TRUE(window.saveWorkspace(native));
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(native).project.pointMasses.at(0).mass, 7);
}

TEST(ImagePackageWorkspace, AppliesPreparedProjectOpensAfterExportFinishes)
{
    QTemporaryDir temporary;
    QSemaphore entered;
    QSemaphore release;
    auto replacement = motrack::importTrackerDocument(fixture());
    replacement.project.pointMasses.at(0).mass = 7;
    const auto replacementPath = temporary.filePath("replacement.trk");
    motrack::writeTrackerDocument(replacementPath, replacement, fixture());
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    auto* pool = &window.executionServices().savePool();
    ASSERT_NE(pool, nullptr);
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
                           window.openProject(replacementPath);
                           EXPECT_TRUE(window.executionServices().analysisPool().waitForDone(5000));
                           QCoreApplication::sendPostedEvents();
                           EXPECT_FALSE(window.windowTitle().contains("replacement.trk"));
                           release.release();
                       });
    const auto target = temporary.filePath("snapshot.trz");
    ASSERT_TRUE(window.exportTrackerArchive(target));
    ASSERT_TRUE(QTest::qWaitFor([&] { return window.windowTitle().contains("replacement.trk"); }, 3000));
    EXPECT_FALSE(window.isWindowModified());
    const auto native = temporary.filePath("replacement.motrack");
    ASSERT_TRUE(window.saveWorkspace(native));
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(native).project.pointMasses.at(0).mass, 7);
    const auto archive = motrack::TrackerArchive::open(target);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(archive.projectPath("experiment.trk")).project.pointMasses.at(0).mass, 2);
}

TEST(ImagePackageWorkspace, MenuExportsAndRejectsAnObsoleteFilenameDialog)
{
    for (const bool replaceDocument : {false, true})
    {
        SCOPED_TRACE(replaceDocument);
        QTemporaryDir temporary;
        const auto target = temporary.filePath("portable.trz");
        const auto replacement = temporary.filePath("replacement.trk");
        ASSERT_TRUE(QFile::copy(fixture(), replacement));
        motrack::MainWindow window;
        window.show();
        ASSERT_TRUE(window.loadProject(fixture()));
        auto* action = window.findChild<QAction*>("exportTrackerArchive");
        ASSERT_NE(action, nullptr);
        QTimer poll;
        QTimer watchdog;
        bool selected = false;
        QObject::connect(&poll,
                         &QTimer::timeout,
                         &window,
                         [&]
                         {
                             if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                             {
                                 poll.stop();
                                 if (replaceDocument)
                                 {
                                     window.openProject(replacement);
                                     EXPECT_TRUE(window.executionServices().analysisPool().waitForDone(5000));
                                     QCoreApplication::sendPostedEvents();
                                     EXPECT_TRUE(window.windowTitle().contains("replacement.trk"));
                                 }
                                 if (auto* focused = dialog->focusWidget())
                                     focused->clearFocus();
                                 dialog->selectFile(target);
                                 ASSERT_TRUE(QTest::qWaitFor([&] { return dialog->selectedFiles() == QStringList{target}; }, 1500));
                                 selected = true;
                                 QMetaObject::invokeMethod(dialog, "accept");
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
        watchdog.start(3000);
        action->trigger();
        EXPECT_TRUE(selected);
        EXPECT_EQ(QFileInfo::exists(target), !replaceDocument);
        EXPECT_FALSE(window.isWindowModified());
    }
}

TEST(ImagePackageWorkspace, WindowDestructionDuringTheFilenameChooserCancelsExport)
{
    auto window = std::make_unique<motrack::MainWindow>();
    ASSERT_TRUE(window->loadProject(fixture()));
    QTimer poll;
    QTimer watchdog;
    QObject::connect(&poll,
                     &QTimer::timeout,
                     [&]
                     {
                         if (qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                         {
                             poll.stop();
                             window.reset();
                         }
                     });
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog,
                     &QTimer::timeout,
                     []
                     {
                         if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                             dialog->reject();
                     });
    poll.start(10);
    watchdog.start(3000);
    window->findChild<QAction*>("exportTrackerArchive")->trigger();
    EXPECT_EQ(window, nullptr);
}

TEST(ImagePackageWorkspace, WindowDestructionDuringDirectOrMenuExportReleasesTheWorker)
{
    for (const bool throughMenu : {false, true})
    {
        SCOPED_TRACE(throughMenu);
        QTemporaryDir temporary;
        const auto target = temporary.filePath("portable.trz");
        QSemaphore entered;
        QSemaphore release;
        auto window = std::make_unique<motrack::MainWindow>();
        ASSERT_TRUE(window->loadProject(fixture()));
        auto* pool = &window->executionServices().savePool();
        ASSERT_NE(pool, nullptr);
        pool->start(
            [&]
            {
                entered.release();
                static_cast<void>(release.tryAcquire(1, 5000));
            });
        ASSERT_TRUE(entered.tryAcquire(1, 5000));
        QTimer poll;
        QTimer watchdog;
        bool choseFile = false;
        QObject::connect(&poll,
                         &QTimer::timeout,
                         [&]
                         {
                             if (throughMenu && !choseFile)
                             {
                                 if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                                 {
                                     choseFile = true;
                                     if (auto* focused = dialog->focusWidget())
                                         focused->clearFocus();
                                     dialog->selectFile(target);
                                     ASSERT_TRUE(QTest::qWaitFor([&] { return dialog->selectedFiles() == QStringList{target}; }, 1500));
                                     QMetaObject::invokeMethod(dialog, "accept");
                                 }
                                 return;
                             }
                             if (window && window->findChild<QDialog*>("imageArchiveProgress"))
                             {
                                 poll.stop();
                                 release.release();
                                 window.reset();
                             }
                         });
        QObject::connect(&watchdog,
                         &QTimer::timeout,
                         [&]
                         {
                             release.release();
                             if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                                 dialog->reject();
                         });
        poll.start(10);
        watchdog.start(3000);
        if (throughMenu)
        {
            window->findChild<QAction*>("exportTrackerArchive")->trigger();
            EXPECT_TRUE(choseFile);
        }
        else
            EXPECT_FALSE(window->exportTrackerArchive(target));
        EXPECT_EQ(window, nullptr);
    }
}

} // namespace
