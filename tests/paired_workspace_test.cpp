#include "image_sequence.hpp"
#include "main_window.hpp"
#include "paired_save.hpp"
#include "tracker_archive.hpp"
#include "tracker_writer.hpp"
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

TEST(PairedWorkspace, ExternalRecoveryChangesRequireReconfirmationBeforeWritingTheCurrentSnapshot)
{
    for (const bool replaced : {false, true})
    {
        SCOPED_TRACE(replaced);
        QTemporaryDir temporary;
        const auto target = temporary.filePath("saved.trk");
        std::atomic_bool cancelled{false};
        const auto interrupt = [&cancelled](auto state)
        {
            cancelled = state == motrack::PairedSaveProgress::StandardSaved;
        };
        auto external = motrack::importTrackerDocument(fixture());
        const auto first = motrack::writePairedDocuments(target, external, fixture(), &cancelled, interrupt);
        ASSERT_EQ(first.status, motrack::PairedSaveStatus::RecoveryRequired);
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(fixture()));
        ASSERT_TRUE(editMass(window, 5));
        EXPECT_FALSE(window.saveTrackerProject(target));
        ASSERT_EQ(motrack::recoverPairedDocuments(first.recoveryPath).status, motrack::PairedSaveStatus::Saved);
        if (replaced)
        {
            motrack::TrackEdits edits;
            ASSERT_TRUE(edits.setPointMassValue(external.project, 0, 7));
            cancelled = false;
            const auto second = motrack::writePairedDocuments(target, external, fixture(), &cancelled, interrupt);
            ASSERT_EQ(second.status, motrack::PairedSaveStatus::RecoveryRequired);
            const auto standard = bytes(target);
            const auto native = bytes(second.nativePath);
            const auto journal = bytes(second.recoveryPath);
            EXPECT_FALSE(window.saveTrackerProject(target, true));
            EXPECT_TRUE(window.isWindowModified());
            EXPECT_EQ(bytes(target), standard);
            EXPECT_EQ(bytes(second.nativePath), native);
            EXPECT_EQ(bytes(second.recoveryPath), journal);
        }
        ASSERT_TRUE(window.saveTrackerProject(target, true));
        EXPECT_FALSE(window.isWindowModified());
        EXPECT_FALSE(QFile::exists(first.recoveryPath));
        EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 5);
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(first.nativePath).project.pointMasses.at(0).mass, 5);
    }
}

TEST(PairedWorkspace, UnidentifiedJournalsRequireConfirmationAfterRepairOrReplacement)
{
    for (int change = 0; change < 3; ++change)
    {
        SCOPED_TRACE(change); // Repaired, replaced, or completed externally.
        QTemporaryDir temporary;
        const auto target = temporary.filePath("saved.trk");
        std::atomic_bool cancelled{false};
        const auto interrupt = [&cancelled](auto state)
        {
            cancelled = state == motrack::PairedSaveProgress::StandardSaved;
        };
        auto external = motrack::importTrackerDocument(fixture());
        const auto first = motrack::writePairedDocuments(target, external, fixture(), &cancelled, interrupt);
        ASSERT_EQ(first.status, motrack::PairedSaveStatus::RecoveryRequired);
        const auto journal = bytes(first.recoveryPath);
        put(first.recoveryPath, "unreadable journal");
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(fixture()));
        ASSERT_TRUE(editMass(window, 5));
        EXPECT_FALSE(window.saveTrackerProject(target));
        put(first.recoveryPath, journal);
        if (change != 0)
            ASSERT_EQ(motrack::recoverPairedDocuments(first.recoveryPath).status, motrack::PairedSaveStatus::Saved);
        if (change == 1)
        {
            motrack::TrackEdits edits;
            ASSERT_TRUE(edits.setPointMassValue(external.project, 0, 7));
            cancelled = false;
            ASSERT_EQ(motrack::writePairedDocuments(target, external, fixture(), &cancelled, interrupt).status,
                      motrack::PairedSaveStatus::RecoveryRequired);
        }
        if (change != 2)
        {
            const auto standard = bytes(target);
            const auto pending = bytes(first.recoveryPath);
            const bool hasNative = QFile::exists(first.nativePath);
            const auto native = hasNative ? bytes(first.nativePath) : QByteArray{};
            EXPECT_FALSE(window.saveTrackerProject(target, true));
            EXPECT_TRUE(window.isWindowModified());
            EXPECT_EQ(bytes(target), standard);
            EXPECT_EQ(bytes(first.recoveryPath), pending);
            EXPECT_EQ(QFile::exists(first.nativePath), hasNative);
            if (hasNative)
                EXPECT_EQ(bytes(first.nativePath), native);
        }
        ASSERT_TRUE(window.saveTrackerProject(target, true));
        EXPECT_FALSE(window.isWindowModified());
        EXPECT_FALSE(QFile::exists(first.recoveryPath));
        EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 5);
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(first.nativePath).project.pointMasses.at(0).mass, 5);
    }
}

TEST(PairedWorkspace, RecoveryConflictsKeepCurrentEditsAndBothDestinations)
{
    QTemporaryDir temporary;
    const auto target = temporary.filePath("saved.trk");
    std::atomic_bool cancelled{false};
    const auto partial =
        motrack::writePairedDocuments(target,
                                      motrack::importTrackerDocument(fixture()),
                                      fixture(),
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
    const auto preparedStandard = bytes(target);
    put(target, "external change");
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    ASSERT_TRUE(editMass(window, 7));
    EXPECT_FALSE(window.saveTrackerProject(target));
    EXPECT_FALSE(window.saveTrackerProject(target, true));
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Could not finish the interrupted save"));
    EXPECT_EQ(bytes(target), "external change");
    EXPECT_FALSE(QFile::exists(partial.nativePath));
    EXPECT_TRUE(QFile::exists(partial.recoveryPath));
    put(target, preparedStandard);
    ASSERT_TRUE(window.saveTrackerProject(target, true));
    EXPECT_FALSE(window.isWindowModified());
    EXPECT_FALSE(QFile::exists(partial.recoveryPath));
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 7);
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(partial.nativePath).project.pointMasses.at(0).mass, 7);
}

TEST(PairedWorkspace, AppliesPreparedOpensAfterSavingAndRejectsCanceledResults)
{
    for (const bool movie : {false, true})
    {
        for (const bool cancel : {false, true})
        {
            for (const bool failSave : {false, true})
            {
                SCOPED_TRACE(::testing::Message() << "movie=" << movie << " cancel=" << cancel << " failSave=" << failSave);
                QTemporaryDir temporary;
                QSemaphore entered;
                QSemaphore release;
                motrack::MainWindow window;
                ASSERT_TRUE(window.loadProject(fixture()));
                auto* pool = window.findChild<QThreadPool*>("workspaceSavePool");
                ASSERT_NE(pool, nullptr);
                pool->start(
                    [&]
                    {
                        entered.release();
                        static_cast<void>(release.tryAcquire(1, 5000));
                    });
                ASSERT_TRUE(entered.tryAcquire(1, 5000));
                const auto input = movie ? "vfr.mp4" : "image-sequence.trk";
                bool processed = false;
                QTimer::singleShot(0,
                                   &window,
                                   [&]
                                   {
                                       const auto path = QString::fromUtf8(MOTRACK_FIXTURES) + '/' + input;
                                       if (movie)
                                           window.openMovie(path);
                                       else
                                           window.openProject(path);
                                       EXPECT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
                                       QCoreApplication::sendPostedEvents();
                                       EXPECT_TRUE(window.windowTitle().contains("linked-images.trk"));
                                       if (cancel)
                                       {
                                           window.openProject(temporary.filePath("missing.trk"));
                                           EXPECT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
                                           QCoreApplication::sendPostedEvents();
                                       }
                                       processed = true;
                                       release.release();
                                   });
                const auto target = temporary.filePath(failSave ? "missing/saved.trk" : "saved.trk");
                EXPECT_EQ(window.saveTrackerProject(target), !failSave);
                EXPECT_TRUE(processed);
                if (cancel)
                {
                    ASSERT_TRUE(QTest::qWaitFor([&] { return window.statusBar()->currentMessage() == "Could not open this project."; }));
                    EXPECT_TRUE(window.windowTitle().contains(failSave ? "linked-images.trk" : "saved.trk"));
                }
                else
                {
                    ASSERT_TRUE(QTest::qWaitFor([&] { return window.windowTitle().contains(input); }));
                }
                if (!failSave)
                {
                    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 2);
                    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(temporary.filePath("saved.motrack")).project.pointMasses.at(0).mass, 2);
                }
            }
        }
    }
}

TEST(PairedWorkspace, DoesNotApplySaveCompletionToAReplacementDocument)
{
    QTemporaryDir temporary;
    QSemaphore entered;
    QSemaphore release;
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    auto* pool = window.findChild<QThreadPool*>("workspaceSavePool");
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
                           EXPECT_TRUE(window.loadProject(fixture()));
                           release.release();
                       });
    const auto target = temporary.filePath("old.trk");
    EXPECT_FALSE(window.saveTrackerProject(target));
    EXPECT_TRUE(window.windowTitle().contains("linked-images.trk"));
    EXPECT_FALSE(window.isWindowModified());
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 2);
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
        QObject::connect(
            &poll,
            &QTimer::timeout,
            &window,
            [&]
            {
                if (!choseFile)
                {
                    if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                    {
                        choseFile = true;
                        // Qt leaves a focused filename editor unchanged in selectFile().
                        if (auto* focused = dialog->focusWidget())
                            focused->clearFocus();
                        dialog->selectFile(target);
                        ASSERT_TRUE(QTest::qWaitFor([dialog, &target] { return dialog->selectedFiles() == QStringList{target}; }, 1500))
                            << dialog->selectedFiles().join(", ").toStdString();
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

TEST(PairedWorkspace, PendingOpensInvalidateTheTrackerChooserAndCompanionConfirmation)
{
    for (const bool duringCompanion : {false, true})
    {
        SCOPED_TRACE(duringCompanion);
        QTemporaryDir temporary;
        const auto replacement = temporary.filePath("replacement.trk");
        auto other = motrack::importTrackerDocument(fixture());
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setPointMassValue(other.project, 0, 7));
        motrack::writeTrackerDocument(replacement, other, fixture());
        const auto originalReplacement = bytes(replacement);
        const auto target = temporary.filePath("saved.trk");
        const auto native = temporary.filePath("saved.motrack");
        put(native, "unrelated companion");
        motrack::MainWindow window;
        window.show();
        ASSERT_TRUE(window.loadProject(fixture()));
        auto* action = window.findChild<QAction*>("saveTrackerProject");
        ASSERT_NE(action, nullptr);
        QTimer poll;
        QTimer watchdog;
        bool choseFile = false;
        bool replaced = false;
        const auto replace = [&]
        {
            window.openProject(replacement);
            EXPECT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
            QCoreApplication::sendPostedEvents();
            replaced = window.windowTitle().contains("replacement.trk");
            EXPECT_TRUE(replaced);
        };
        QObject::connect(
            &poll,
            &QTimer::timeout,
            &window,
            [&]
            {
                if (!choseFile)
                {
                    if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                    {
                        choseFile = true;
                        if (!duringCompanion)
                        {
                            poll.stop();
                            replace();
                        }
                        // Qt leaves a focused filename editor unchanged in selectFile().
                        if (auto* focused = dialog->focusWidget())
                            focused->clearFocus();
                        dialog->selectFile(target);
                        ASSERT_TRUE(QTest::qWaitFor([dialog, &target] { return dialog->selectedFiles() == QStringList{target}; }, 1500))
                            << dialog->selectedFiles().join(", ").toStdString();
                        QMetaObject::invokeMethod(dialog, "accept");
                    }
                }
                else if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                {
                    poll.stop();
                    replace();
                    message->button(QMessageBox::Yes)->click();
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
        watchdog.start(7000);
        action->trigger();
        EXPECT_TRUE(choseFile && replaced);
        EXPECT_FALSE(QFile::exists(target));
        EXPECT_EQ(bytes(native), "unrelated companion");
        EXPECT_EQ(bytes(replacement), originalReplacement);
        EXPECT_FALSE(QFile::exists(temporary.filePath("replacement.motrack")));
    }
}

TEST(PairedWorkspace, PendingOpensInvalidateTheNativeSaveChooser)
{
    QTemporaryDir temporary;
    const auto replacement = temporary.filePath("replacement.trk");
    ASSERT_TRUE(QFile::copy(fixture(), replacement));
    const auto target = temporary.filePath("saved.motrack");
    put(target, "keep existing destination");
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixture()));
    auto* action = window.findChild<QAction*>("saveWorkspace");
    ASSERT_NE(action, nullptr);
    QTimer poll;
    QTimer watchdog;
    bool replaced = false;
    QObject::connect(&poll,
                     &QTimer::timeout,
                     &window,
                     [&]
                     {
                         auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                         if (!dialog)
                             return;
                         poll.stop();
                         window.openProject(replacement);
                         ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
                         QCoreApplication::sendPostedEvents();
                         replaced = window.windowTitle().contains("replacement.trk");
                         ASSERT_TRUE(replaced);
                         dialog->setOption(QFileDialog::DontConfirmOverwrite);
                         if (auto* focused = dialog->focusWidget())
                             focused->clearFocus();
                         dialog->selectFile(target);
                         ASSERT_TRUE(QTest::qWaitFor([dialog, &target] { return dialog->selectedFiles() == QStringList{target}; }, 1500));
                         QMetaObject::invokeMethod(dialog, "accept");
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
    watchdog.start(7000);
    action->trigger();
    EXPECT_TRUE(replaced);
    EXPECT_EQ(bytes(target), "keep existing destination");
}

TEST(PairedWorkspace, PendingOpenInvalidatesRecoveryConfirmationAndItsSaveDestination)
{
    QTemporaryDir temporary;
    const auto target = temporary.filePath("saved.trk");
    std::atomic_bool cancelled{false};
    const auto partial =
        motrack::writePairedDocuments(target,
                                      motrack::importTrackerDocument(fixture()),
                                      fixture(),
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
    const auto originalStandard = bytes(target);
    const auto originalRecord = bytes(partial.recoveryPath);
    const auto replacement = temporary.filePath("replacement.trk");
    auto other = motrack::importTrackerDocument(fixture());
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(other.project, 0, 7));
    motrack::writeTrackerDocument(replacement, other, fixture());
    const auto originalReplacement = bytes(replacement);
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixture()));
    EXPECT_FALSE(window.saveTrackerProject(target));
    auto* action = window.findChild<QAction*>("saveProject");
    ASSERT_NE(action, nullptr);
    QTimer poll;
    QTimer discard;
    QTimer watchdog;
    bool confirmed = false;
    bool replaced = false;
    QObject::connect(&discard,
                     &QTimer::timeout,
                     &window,
                     []
                     {
                         if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                         {
                             if (auto* button = message->button(QMessageBox::Discard))
                                 button->click();
                         }
                     });
    QObject::connect(&poll,
                     &QTimer::timeout,
                     &window,
                     [&]
                     {
                         // The outer confirmation remains on the stack throughout this callback.
                         auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                         if (!message || !message->button(QMessageBox::Yes))
                             return;
                         poll.stop();
                         discard.start(10);
                         window.openProject(replacement);
                         EXPECT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
                         QCoreApplication::sendPostedEvents();
                         replaced = window.windowTitle().contains("replacement.trk");
                         EXPECT_TRUE(replaced);
                         discard.stop();
                         confirmed = true;
                         message->button(QMessageBox::Yes)->click();
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
    watchdog.start(7000);
    action->trigger();
    EXPECT_TRUE(confirmed && replaced);
    EXPECT_EQ(bytes(target), originalStandard);
    EXPECT_EQ(bytes(partial.recoveryPath), originalRecord);
    EXPECT_FALSE(QFile::exists(partial.nativePath));
    EXPECT_EQ(bytes(replacement), originalReplacement);
    EXPECT_FALSE(QFile::exists(temporary.filePath("replacement.motrack")));
}

TEST(PairedWorkspace, DestroyingTheWindowDuringSaveCancelsAndReleasesTheWorker)
{
    for (const auto [native, throughMenu] :
         {std::pair{false, false}, std::pair{false, true}, std::pair{true, false}, std::pair{true, true}})
    {
        SCOPED_TRACE(native);
        SCOPED_TRACE(throughMenu);
        QTemporaryDir temporary;
        const auto target = temporary.filePath(native ? "saved.motrack" : "saved.trk");
        put(target, "previous destination");
        QSemaphore entered;
        QSemaphore release;
        auto window = std::make_unique<motrack::MainWindow>();
        ASSERT_TRUE(window->loadProject(fixture()));
        // MainWindow sets cancellation before QObject emits destroyed, then its child pool joins.
        QObject::connect(window.get(), &QObject::destroyed, [&release] { release.release(); });
        auto* pool = window->findChild<QThreadPool*>("workspaceSavePool");
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
        QObject::connect(
            &poll,
            &QTimer::timeout,
            [&]
            {
                if (throughMenu && !choseFile)
                {
                    if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                    {
                        choseFile = true;
                        dialog->setOption(QFileDialog::DontConfirmOverwrite, true);
                        // Qt leaves a focused filename editor unchanged in selectFile().
                        if (auto* focused = dialog->focusWidget())
                            focused->clearFocus();
                        dialog->selectFile(target);
                        ASSERT_TRUE(QTest::qWaitFor([dialog, &target] { return dialog->selectedFiles() == QStringList{target}; }, 1500))
                            << dialog->selectedFiles().join(", ").toStdString();
                        QMetaObject::invokeMethod(dialog, "accept");
                    }
                    return;
                }
                if (window && (native ? window->statusBar()->currentMessage() == "Saving project…"
                                      : window->findChild<QDialog*>("pairedSaveProgress") != nullptr))
                {
                    poll.stop();
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
            auto* action = window->findChild<QAction*>(native ? "saveWorkspace" : "saveTrackerProject");
            ASSERT_NE(action, nullptr);
            action->trigger();
            EXPECT_TRUE(choseFile);
        }
        else
            EXPECT_FALSE(native ? window->saveWorkspace(target) : window->saveTrackerProject(target));
        EXPECT_EQ(window, nullptr);
        EXPECT_EQ(bytes(target), "previous destination");
    }
}

TEST(PairedWorkspace, WindowDestructionDuringSaveDialogsLeavesNoDanglingWidgets)
{
    // Native/Tracker chooser, companion/recovery confirmation, and discard prompts from close/open.
    for (int phase = 0; phase < 8; ++phase)
    {
        SCOPED_TRACE(phase);
        QTemporaryDir temporary;
        const auto standard = temporary.filePath("saved.trk");
        const auto native = temporary.filePath("saved.motrack");
        const auto replacement = temporary.filePath("replacement.trk");
        ASSERT_TRUE(QFile::copy(fixture(), replacement));
        QString journal;
        QByteArray standardBefore;
        if (phase == 2)
            put(native, "unrelated companion");
        if (phase == 3)
        {
            std::atomic_bool cancelled{false};
            const auto interrupted = motrack::writePairedDocuments(standard,
                                                                   motrack::importTrackerDocument(fixture()),
                                                                   fixture(),
                                                                   &cancelled,
                                                                   [&cancelled](auto state)
                                                                   { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
            ASSERT_EQ(interrupted.status, motrack::PairedSaveStatus::RecoveryRequired);
            journal = interrupted.recoveryPath;
            standardBefore = bytes(standard);
        }
        auto window = std::make_unique<motrack::MainWindow>();
        ASSERT_TRUE(window->loadProject(fixture()));
        if (phase == 3)
            EXPECT_FALSE(window->saveTrackerProject(standard));
        if (phase >= 4)
            ASSERT_TRUE(editMass(*window, 5));
        QTimer poll;
        QTimer watchdog;
        bool destroyed = false;
        bool choseFile = false;
        QObject::connect(&poll,
                         &QTimer::timeout,
                         [&]
                         {
                             if (auto* chooser = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                             {
                                 if (phase == 2 && !choseFile)
                                 {
                                     choseFile = true;
                                     if (auto* focused = chooser->focusWidget())
                                         focused->clearFocus();
                                     chooser->selectFile(standard);
                                     ASSERT_TRUE(QTest::qWaitFor([&] { return chooser->selectedFiles() == QStringList{standard}; }, 1500));
                                     QMetaObject::invokeMethod(chooser, "accept");
                                     return;
                                 }
                                 if (phase > 1)
                                     return;
                             }
                             else if (!qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                                 return;
                             poll.stop();
                             destroyed = true;
                             window.reset();
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
        watchdog.start(4000);
        if (phase < 4)
        {
            const auto* name = phase == 0 ? "saveWorkspace" : phase == 3 ? "saveProject" : "saveTrackerProject";
            window->findChild<QAction*>(name)->trigger();
        }
        else if (phase == 4)
            static_cast<void>(window->close());
        else if (phase == 5)
            window->openProject(replacement);
        else if (phase == 6)
            window->openMovie(QString::fromUtf8(MOTRACK_FIXTURES) + "/vfr.mp4");
        else
            EXPECT_FALSE(window->openImageSequence({fixture("images/frame0.png")}, 25));
        ASSERT_TRUE(QTest::qWaitFor([&] { return !window; }, 4500));
        EXPECT_TRUE(destroyed);
        if (phase == 2)
            EXPECT_EQ(bytes(native), "unrelated companion");
        if (phase == 3)
        {
            EXPECT_EQ(bytes(standard), standardBefore);
            EXPECT_TRUE(QFileInfo::exists(journal));
        }
        else
            EXPECT_FALSE(QFileInfo::exists(standard));
    }
}

} // namespace
