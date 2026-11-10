#include "main_window.hpp"
#include "template_tracker.hpp"
#include "track_identity.hpp"
#include "tracker_archive.hpp"
#include "workspace_context.hpp"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLocale>
#include <QPushButton>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSlider>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

namespace
{

QString fixture(const char* name = "linked-images/linked-images.trk")
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/" + name;
}

motrack::WorkspaceSession::OpenedSource addStandalone(motrack::WorkspaceContext& workspace, const QString& path = fixture())
{
    auto incoming = std::make_unique<motrack::DocumentSession>();
    auto document = motrack::importTrackerDocument(path);
    document.alignMovieToClip = true;
    motrack::retainWorkspaceMediaLocations(document, path);
    incoming->replace(std::move(document), path);
    return workspace.session().addDocument(incoming, path);
}

motrack::WorkspaceSession::OpenedSource addGroup(motrack::WorkspaceContext& workspace, bool movie = false)
{
    const auto path = fixture(movie ? "linked-mov/archived-movie.trz" : "linked-images/multitab-images.trz");
    const auto archive = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(path));
    std::optional<motrack::ArchiveGroup> group;
    for (const auto& member : archive->projects())
    {
        auto document = motrack::importTrackerDocument(archive->projectPath(member));
        archive->resolveMedia(document.project, member);
        document.archivePayload = archive;
        document.archiveMember = member.toStdString();
        document.alignMovieToClip = true;
        if (group)
            (void)motrack::appendArchiveGroupDocument(*group, std::move(document));
        else
            group = motrack::makeArchiveGroup(std::move(document));
    }
    if (!group)
        throw std::runtime_error("Missing archive fixture experiments.");
    auto incoming = std::make_unique<motrack::ArchiveGroupSession>();
    incoming->replace(std::move(*group), path, motrack::ArchiveGroupSource::Tracker);
    return workspace.session().addGroup(incoming);
}

motrack::WorkspaceSession::OpenedSource addTrackingExperiment(motrack::WorkspaceContext& workspace, const QTemporaryDir& temporary)
{
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
        if (!image.save(temporary.filePath(name)))
            throw std::runtime_error("Could not create the tracking fixture image.");
    }
    const auto path = temporary.filePath("tracking.motrack");
    motrack::writeWorkspaceDocument(path, document);
    auto incoming = std::make_unique<motrack::DocumentSession>();
    incoming->replace(std::move(document), path);
    return workspace.session().addDocument(incoming, path);
}

bool editMass(motrack::MainWindow& view, double mass)
{
    auto* action = view.findChild<QAction*>("setPointMassValue");
    if (!action || !action->isEnabled())
        return false;
    bool accepted = false;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog,
                     &QTimer::timeout,
                     &view,
                     []
                     {
                         if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                             dialog->reject();
                     });
    watchdog.start(2000);
    QTimer::singleShot(0,
                       &view,
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

TEST(BoundWorkspace, IndependentViewsKeepPlayheadsHistoriesAndSaveDestinations)
{
    QTemporaryDir temporary;
    auto workspace = std::make_shared<motrack::WorkspaceContext>();
    const auto a = addStandalone(*workspace);
    const auto b = addStandalone(*workspace);
    motrack::MainWindow first(workspace, a.documents[0]);
    motrack::MainWindow second(workspace, b.documents[0]);
    ASSERT_TRUE(editMass(first, 6));
    ASSERT_TRUE(editMass(second, 7));
    auto* aSlider = first.findChild<QSlider*>("stepSlider");
    auto* bSlider = second.findChild<QSlider*>("stepSlider");
    ASSERT_TRUE(aSlider && bSlider);
    aSlider->setValue(2);
    bSlider->setValue(1);
    workspace->session().showDocument(a.documents[0]);
    EXPECT_EQ(aSlider->value(), 2);
    EXPECT_EQ(bSlider->value(), 1);
    first.findChild<QAction*>("undoEdit")->trigger();
    EXPECT_DOUBLE_EQ(workspace->session().project(b.documents[0]).pointMasses.front().mass, 7);
    first.findChild<QAction*>("redoEdit")->trigger();
    const auto native = temporary.filePath("first.motrack");
    const auto standard = temporary.filePath("second.trk");
    ASSERT_TRUE(first.saveWorkspace(native));
    EXPECT_FALSE(first.isWindowModified());
    EXPECT_TRUE(second.isWindowModified());
    ASSERT_TRUE(second.saveTrackerProject(standard));
    EXPECT_FALSE(second.isWindowModified());
    EXPECT_EQ(workspace->session().nativeSavePath(a.source), native);
    EXPECT_EQ(workspace->session().standardSavePath(b.source), standard);
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(native).project.pointMasses.front().mass, 6);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(standard).project.pointMasses.front().mass, 7);
    ASSERT_TRUE(editMass(second, 9));
    ASSERT_TRUE(second.saveWorkspace(temporary.filePath("second-copy.motrack")));
    EXPECT_TRUE(second.isWindowModified()); // Native-only copy cannot clear the established Tracker obligation.
    EXPECT_FALSE(first.loadProject(fixture()));
    EXPECT_FALSE(first.openImageSequence({}, 24));
    first.openMovie("missing.mov");
    first.openProject("missing.trk");
    EXPECT_FALSE(first.findChild<QAction*>("openProject")->isEnabled());
    EXPECT_DOUBLE_EQ(workspace->session().project(a.documents[0]).pointMasses.front().mass, 6);
    EXPECT_FALSE(first.saveWorkspace(temporary.filePath("wrong.trk")));
    EXPECT_FALSE(first.saveTrackerProject(temporary.filePath("wrong.trz")));
    EXPECT_FALSE(first.saveWorkspace(temporary.filePath("missing/child.motrack")));
}

TEST(BoundWorkspace, HiddenArchiveMemberEditsAndUndoSurviveViewDestructionAndWholeGroupSaving)
{
    QTemporaryDir temporary;
    auto workspace = std::make_shared<motrack::WorkspaceContext>();
    const auto source = addGroup(*workspace);
    ASSERT_EQ(source.documents.size(), 2u);
    auto first = std::make_unique<motrack::MainWindow>(workspace, source.documents[0]);
    motrack::MainWindow second(workspace, source.documents[1]);
    ASSERT_TRUE(editMass(*first, 6));
    ASSERT_TRUE(QTest::qWaitFor([&] { return second.isWindowModified(); }));
    ASSERT_TRUE(editMass(second, 7));
    const auto firstCheckpoint = workspace->session().documentCheckpoint(source.documents[0]);
    first.reset();
    EXPECT_EQ(workspace->session().tabs(), std::vector<QUuid>{source.documents[1]});
    const auto native = temporary.filePath("group.motrack");
    ASSERT_TRUE(second.saveWorkspace(native));
    const auto reopened = motrack::readArchiveGroup(native);
    ASSERT_EQ(reopened.members.size(), 2u);
    EXPECT_DOUBLE_EQ(reopened.members[0].document.project.pointMasses.front().mass, 6);
    EXPECT_DOUBLE_EQ(reopened.members[1].document.project.pointMasses.front().mass, 7);
    first = std::make_unique<motrack::MainWindow>(workspace, source.documents[0]);
    EXPECT_EQ(workspace->session().documentCheckpoint(source.documents[0]).identity, firstCheckpoint.identity);
    EXPECT_TRUE(first->findChild<QAction*>("undoEdit")->isEnabled());
    second.findChild<QAction*>("undoEdit")->trigger();
    ASSERT_TRUE(QTest::qWaitFor([&] { return first->isWindowModified(); }));
    second.findChild<QAction*>("redoEdit")->trigger();
    const auto standard = temporary.filePath("group.trz");
    ASSERT_TRUE(first->saveTrackerProject(standard));
    EXPECT_FALSE(first->isWindowModified());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !second.isWindowModified(); }));
    const auto archive = motrack::TrackerArchive::open(standard);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(archive.projectPath("experiment.trk")).project.pointMasses.front().mass, 6);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(archive.projectPath("second.trk")).project.pointMasses.front().mass, 7);
    ASSERT_TRUE(editMass(*first, 8));
    ASSERT_TRUE(first->close()); // View close retains unsaved edits; the future shell owns explicit source discard.
    EXPECT_DOUBLE_EQ(workspace->session().project(source.documents[0]).pointMasses.front().mass, 8);
    ASSERT_TRUE(second.saveTrackerProject(standard));
    EXPECT_DOUBLE_EQ(motrack::readArchiveGroup(native).members[0].document.project.pointMasses.front().mass, 8);
}

TEST(BoundWorkspace, RejectsDuplicateEditorsAndKeepsRecordsAfterLastViewReleasesTheWorkspace)
{
    auto workspace = std::make_shared<motrack::WorkspaceContext>();
    const auto source = addStandalone(*workspace);
    auto view = std::make_unique<motrack::MainWindow>(workspace, source.documents[0]);
    EXPECT_THROW(motrack::MainWindow(workspace, source.documents[0]), motrack::ProjectError);
    EXPECT_THROW(motrack::MainWindow(workspace, QUuid::createUuid()), motrack::ProjectError);
    EXPECT_THROW(motrack::MainWindow(std::shared_ptr<motrack::WorkspaceContext>{}, QUuid{}), motrack::ProjectError);
    std::weak_ptr<motrack::WorkspaceContext> weak = workspace;
    workspace.reset();
    EXPECT_FALSE(weak.expired());
    ASSERT_TRUE(editMass(*view, 8));
    view.reset();
    EXPECT_TRUE(weak.expired());
}

TEST(BoundWorkspace, MovieIndexesAreOwnedByTheRightRecordAndRejectObsoleteOrInvalidMetadata)
{
    QTemporaryDir temporary;
    auto workspace = std::make_shared<motrack::WorkspaceContext>();
    const auto source = addGroup(*workspace, true);
    ASSERT_GE(source.documents.size(), 2u);
    motrack::MainWindow first(workspace, source.documents[0]);
    motrack::MainWindow second(workspace, source.documents[1]);
    auto& model = workspace->session();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !model.document(source.documents[0]).movieFrameTimes.empty(); }));
    EXPECT_TRUE(model.document(source.documents[0]).movie);
    ASSERT_TRUE(editMass(first, 6));
    const auto old = model.beginMovieIndex(source.documents[0]);
    const auto current = model.beginMovieIndex(source.documents[0]);
    EXPECT_FALSE(model.applyMovieIndex(old, {0, .125, .25, .375}, 4));
    EXPECT_FALSE(model.applyMovieIndex(current, {0, .125}, 4));
    EXPECT_FALSE(model.applyMovieIndex(current, {0, std::numeric_limits<double>::infinity()}, 2));
    EXPECT_FALSE(model.applyMovieIndex(current, {.125, 0}, 2));
    const auto before = model.document(source.documents[0]).movieFrameTimes;
    EXPECT_FALSE(model.applyMovieIndex(current, {0, 0}, 2));
    EXPECT_FALSE(model.applyMovieIndex(current, {.125, .25}, 2));
    EXPECT_FALSE(model.applyMovieIndex(current, {-.125, 0}, 2));
    EXPECT_EQ(model.document(source.documents[0]).movieFrameTimes, before);
    const auto wrongKind = model.beginJob(source.documents[0], motrack::WorkspaceJob::Frame);
    EXPECT_FALSE(model.applyMovieIndex(wrongKind, {0}, 1));
    EXPECT_TRUE(model.applyMovieIndex(current, {0, .125, .25, .375}, 4));
    EXPECT_TRUE(model.history(source.documents[0]).canUndo());
    EXPECT_TRUE(model.isModified(source.source));
    const auto image = addStandalone(*workspace);
    EXPECT_THROW((void)model.beginMovieIndex(image.documents[0]), motrack::ProjectError);
    const auto saved = temporary.filePath("indexed.motrack");
    ASSERT_TRUE(first.saveWorkspace(saved));
    EXPECT_EQ(motrack::readArchiveGroup(saved).members[0].document.movieFrameTimes, (std::vector<double>{0, .125, .25, .375}));
}

TEST(BoundWorkspace, RefusesExtraSnapshotsAndKeepsExportCopiesSeparateFromSaveCheckpoints)
{
    QTemporaryDir temporary;
    auto workspace = std::make_shared<motrack::WorkspaceContext>();
    const auto source = addStandalone(*workspace);
    motrack::MainWindow view(workspace, source.documents[0]);
    ASSERT_TRUE(editMass(view, 6));
    auto& model = workspace->session();
    const auto a = model.beginSave(source.documents[0]);
    const auto b = model.beginSave(source.documents[0]);
    if (!a.has_value() || !b.has_value())
        FAIL() << "Expected two available snapshots.";
    EXPECT_FALSE(view.saveWorkspace(temporary.filePath("busy.motrack")));
    EXPECT_FALSE(view.exportTrackerArchive(temporary.filePath("busy.trz")));
    EXPECT_FALSE(QFileInfo::exists(temporary.filePath("busy.motrack")));
    (void)model.completeSave(*a, {}, {});
    (void)model.completeSave(*b, {}, {});
    const auto before = model.nativeSavePath(source.source);
    ASSERT_TRUE(view.exportTrackerArchive(temporary.filePath("copy.trz")));
    EXPECT_TRUE(view.isWindowModified());
    EXPECT_EQ(model.nativeSavePath(source.source), before);
    ASSERT_TRUE(view.saveWorkspace(temporary.filePath("saved.motrack")));
    EXPECT_FALSE(view.isWindowModified());
}

TEST(BoundWorkspace, CompletesCancelledWritesAfterTheViewIsDestroyedWithoutLeakingOperationSlots)
{
    for (const bool paired : {false, true})
    {

        SCOPED_TRACE(paired);
        QTemporaryDir temporary;
        auto workspace = std::make_shared<motrack::WorkspaceContext>();
        const auto source = addStandalone(*workspace);
        auto view = std::make_unique<motrack::MainWindow>(workspace, source.documents[0]);
        ASSERT_TRUE(editMass(*view, 8));
        QSemaphore entered;
        QSemaphore release;
        auto& pool = workspace->services()->savePool();
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
        ASSERT_TRUE(entered.tryAcquire(1, 5000));
        QObject::connect(view.get(), &QObject::destroyed, [&] { release.release(); });
        QTimer::singleShot(0, view.get(), [&] { view.reset(); });
        EXPECT_FALSE(paired ? view->saveTrackerProject(temporary.filePath("canceled.trk"))
                            : view->saveWorkspace(temporary.filePath("canceled.motrack")));
        EXPECT_FALSE(view);
        EXPECT_FALSE(QFileInfo::exists(temporary.filePath("canceled.trk")));
        EXPECT_FALSE(QFileInfo::exists(temporary.filePath("canceled.motrack")));
        EXPECT_TRUE(workspace->session().isModified(source.source));
        const auto a = workspace->session().beginSave(source.documents[0]);
        const auto b = workspace->session().beginSave(source.documents[0]);
        if (!a.has_value() || !b.has_value())
            FAIL() << "Canceled view write leaked its operation slot.";
        (void)workspace->session().completeSave(*a, {}, {});
        (void)workspace->session().completeSave(*b, {}, {});
    }
}

TEST(BoundWorkspace, NewerLiveEditsStayDirtyWhenAnOlderViewSaveFinishes)
{
    QTemporaryDir temporary;
    auto workspace = std::make_shared<motrack::WorkspaceContext>();
    const auto source = addStandalone(*workspace);
    motrack::MainWindow view(workspace, source.documents[0]);
    ASSERT_TRUE(editMass(view, 8));
    QSemaphore entered;
    QSemaphore release;
    auto& pool = workspace->services()->savePool();
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
    ASSERT_TRUE(entered.tryAcquire(1, 5000));
    QTimer::singleShot(0,
                       &view,
                       [&]
                       {
                           EXPECT_TRUE(editMass(view, 9));
                           release.release();
                       });
    const auto path = temporary.filePath("older.motrack");
    EXPECT_FALSE(view.saveWorkspace(path));
    EXPECT_TRUE(view.isWindowModified());
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(path).project.pointMasses.front().mass, 8);
    EXPECT_DOUBLE_EQ(workspace->session().project(source.documents[0]).pointMasses.front().mass, 9);
    ASSERT_TRUE(view.saveWorkspace(path));
    EXPECT_FALSE(view.isWindowModified());
}

TEST(BoundWorkspace, RecoversWholeGroupBeforeCapturingFreshEditsAndRetainsCancelledRecovery)
{
    QTemporaryDir temporary;
    auto workspace = std::make_shared<motrack::WorkspaceContext>();
    const auto source = addGroup(*workspace);
    motrack::MainWindow view(workspace, source.documents[0]);
    ASSERT_TRUE(editMass(view, 8));
    const auto path = temporary.filePath("recover.trz");
    const auto request = workspace->session().beginSave(source.documents[0]);
    if (!request.has_value())
        FAIL() << "Expected an available snapshot.";
    const auto result = motrack::writeWorkspacePair(*request,
                                                    path,
                                                    [](motrack::PairedSaveProgress progress)
                                                    {
                                                        if (progress == motrack::PairedSaveProgress::StandardSaved)
                                                            throw std::runtime_error("Interrupt after standard commit.");
                                                    });
    ASSERT_EQ(workspace->session().completePairedSave(*request, result), motrack::SaveOutcome::Partial);
    EXPECT_FALSE(workspace->session().documentCheckpoint(source.documents[0]).recoveryTransactions.isEmpty());
    EXPECT_FALSE(view.saveTrackerProject(path));
    ASSERT_TRUE(editMass(view, 9));
    QSemaphore entered;
    QSemaphore release;
    auto& pool = workspace->services()->savePool();
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
    ASSERT_TRUE(entered.tryAcquire(1, 5000));
    QTimer::singleShot(0,
                       &view,
                       [&]
                       {
                           auto* cancel = view.findChild<QPushButton*>("cancelPairedSave");
                           EXPECT_NE(cancel, nullptr);
                           if (cancel)
                               cancel->click();
                           release.release();
                       });
    EXPECT_FALSE(view.saveTrackerProject(path, true));
    EXPECT_FALSE(workspace->session().recoveryPath(source.source).isEmpty());
    ASSERT_TRUE(view.saveTrackerProject(path, true));
    EXPECT_TRUE(workspace->session().recoveryPath(source.source).isEmpty());
    EXPECT_FALSE(view.isWindowModified());
    EXPECT_DOUBLE_EQ(motrack::readArchiveGroup(temporary.filePath("recover.motrack")).members[0].document.project.pointMasses.front().mass,
                     9);
}

TEST(BoundWorkspace, TrackingContinuesThroughSavingAndClosingAnotherRetainedView)
{
    QTemporaryDir temporary;
    auto workspace = std::make_shared<motrack::WorkspaceContext>();
    const auto a = addTrackingExperiment(*workspace, temporary);
    const auto b = addStandalone(*workspace, fixture("linked-mov/linked-movie.trk"));
    motrack::MainWindow first(workspace, a.documents[0]);
    auto second = std::make_unique<motrack::MainWindow>(workspace, b.documents[0]);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !workspace->session().document(b.documents[0]).movieFrameTimes.empty(); }));
    auto* action = first.findChild<QAction*>("autoTracker");
    auto* slider = first.findChild<QSlider*>("stepSlider");
    auto* marks = first.findChild<QTableWidget*>("marksTable");
    ASSERT_TRUE(action && slider && marks);
    QSemaphore entered;
    QSemaphore release;
    std::atomic_bool timedOut{false};
    auto& pool = workspace->services()->analysisPool();
    const auto cleanup = qScopeGuard(
        [&]
        {
            release.release(2);
            (void)pool.waitForDone(5000);
        });
    for (int index = 0; index < 2; ++index)
        pool.start(
            [&]
            {
                entered.release();
                if (!release.tryAcquire(1, 5000))
                    timedOut = true;
            });
    ASSERT_TRUE(entered.tryAcquire(2, 5000));
    action->trigger();
    slider->setValue(1);
    ASSERT_TRUE(action->isChecked());
    ASSERT_TRUE(first.saveWorkspace(temporary.filePath("during-tracking.motrack")));
    workspace->session().showDocument(b.documents[0]);
    second.reset();
    EXPECT_FALSE(timedOut.load());
    EXPECT_TRUE(action->isChecked());
    release.release(2);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !action->isChecked() && marks->rowCount() == 3; }));
    EXPECT_EQ(marks->item(1, 2)->text(), "28");
    EXPECT_EQ(marks->item(2, 2)->text(), "34");
    EXPECT_TRUE(first.isWindowModified());
    EXPECT_EQ(workspace->session().project(a.documents[0]).lengthUnit, "cm");
    EXPECT_EQ(workspace->session().project(a.documents[0]).clip.frameDurationMs, 50);
    EXPECT_DOUBLE_EQ(workspace->session().document(b.documents[0]).movieFrameTimes.at(1), .125);
    ASSERT_TRUE(first.saveWorkspace(temporary.filePath("finished.motrack")));
    EXPECT_EQ(motrack::readWorkspaceDocument(temporary.filePath("finished.motrack")).project.pointMasses.front().marks.size(), 3u);
}

TEST(BoundWorkspace, CancelledPendingMatchesResetControlsAndAllowTrackingToRestart)
{
    for (const bool entireSource : {false, true})
    {
        SCOPED_TRACE(entireSource);
        QTemporaryDir temporary;
        auto workspace = std::make_shared<motrack::WorkspaceContext>();
        const auto source = addTrackingExperiment(*workspace, temporary);
        motrack::MainWindow view(workspace, source.documents[0]);
        auto* action = view.findChild<QAction*>("autoTracker");
        auto* slider = view.findChild<QSlider*>("stepSlider");
        auto* marks = view.findChild<QTableWidget*>("marksTable");
        ASSERT_TRUE(action && slider && marks);
        QSemaphore entered;
        QSemaphore release;
        std::atomic_bool timedOut{false};
        auto& pool = workspace->services()->analysisPool();
        const auto cleanup = qScopeGuard(
            [&]
            {
                release.release(2);
                (void)pool.waitForDone(5000);
            });
        for (int index = 0; index < 2; ++index)
            pool.start(
                [&]
                {
                    entered.release();
                    if (!release.tryAcquire(1, 5000))
                        timedOut = true;
                });
        ASSERT_TRUE(entered.tryAcquire(2, 5000));
        action->trigger();
        slider->setValue(1);
        ASSERT_TRUE(action->isChecked());
        if (entireSource)
            workspace->session().cancelSourceWork(source.source);
        else
            workspace->session().cancelJobs(source.documents[0]);
        release.release(2);
        EXPECT_FALSE(timedOut.load());
        ASSERT_TRUE(QTest::qWaitFor([&] { return !action->isChecked(); }));
        EXPECT_EQ(marks->rowCount(), 1);
        slider->setValue(0);
        EXPECT_TRUE(action->isEnabled());
        action->trigger();
        ASSERT_TRUE(QTest::qWaitFor([&] { return !action->isChecked() && marks->rowCount() == 3; }));
        EXPECT_EQ(marks->item(1, 2)->text(), "28");
        EXPECT_EQ(marks->item(2, 2)->text(), "34");
    }
}

TEST(BoundWorkspace, CancellingPendingMovieFramesResetsTrackingAndPlaybackWithoutFrameDelivery)
{
    for (const bool entireSource : {false, true})
        for (const bool tracking : {false, true})
        {
            SCOPED_TRACE(entireSource);
            SCOPED_TRACE(tracking);
            QTemporaryDir temporary;
            auto workspace = std::make_shared<motrack::WorkspaceContext>();
            const auto other = addStandalone(*workspace);
            auto& model = workspace->session();
            const auto otherJob = model.beginJob(other.documents[0], motrack::WorkspaceJob::AutoTrack);
            const auto moviePath = QString::fromUtf8(MOTRACK_FIXTURES) + "/vfr.mp4";
            motrack::VideoReader reader(moviePath);
            const auto decoded = reader.readNext();
            if (!decoded)
                FAIL() << "Missing first movie fixture frame.";
            std::optional<QPoint> center;
            for (int y = 6; y < decoded->image.height() - 6 && !center; ++y)
                for (int x = 6; x < decoded->image.width() - 6 && !center; ++x)
                    if (motrack::TemplateTracker::fromImage(decoded->image, {x, y}))
                        center = QPoint{x, y};
            if (!center)
                FAIL() << "Movie fixture has no textured tracking patch.";
            motrack::WorkspaceDocument movie;
            movie.movie = true;
            movie.project.clip.stepCount = 4;
            movie.project.clip.sourceFrameCount = 4;
            movie.project.clip.mediaReferences.push_back(moviePath.toStdString());
            movie.project.pointMasses.push_back(
                {.name = "Movie point",
                 .marks = {{.frame = 0, .x = static_cast<double>(center->x()), .y = static_cast<double>(center->y())}},
                 .id = motrack::allocateTrackId(movie.project)});
            auto incoming = std::make_unique<motrack::DocumentSession>();
            const auto path = temporary.filePath("movie.motrack");
            incoming->replace(std::move(movie), path);
            const auto source = model.addDocument(incoming, path);
            const auto& document = model.document(source.documents[0]);
            motrack::MainWindow view(workspace, source.documents[0]);
            auto* canvas = view.findChild<QWidget*>("videoCanvas");
            auto* action = view.findChild<QAction*>("autoTracker");
            auto* slider = view.findChild<QSlider*>("stepSlider");
            auto* play = view.findChild<QToolButton*>("playClip");
            ASSERT_TRUE(canvas && action && slider && play);
            ASSERT_TRUE(QTest::qWaitFor([&] { return canvas->accessibleName() == "Frame 0" && !document.movieFrameTimes.empty(); }));
            int delivered = 0;
            QObject::connect(workspace->services().get(),
                             &motrack::WorkspaceServices::frameDecoded,
                             &view,
                             [&](const motrack::MovieFrameResult& result)
                             {
                                 if (result.request.token.document == source.documents[0])
                                     ++delivered;
                             });
            if (tracking)
            {
                action->trigger();
                ASSERT_TRUE(action->isChecked());
            }
            else
            {
                play->click();
                ASSERT_EQ(play->accessibleName(), "Pause");
            }
            slider->setValue(1); // No event-loop turn: frame delivery and match creation have not happened.
            if (entireSource)
                model.cancelSourceWork(source.source);
            else
                model.cancelJobs(source.documents[0]);
            ASSERT_TRUE(QTest::qWaitFor([&] { return !action->isChecked() && play->accessibleName() == "Play"; }));
            EXPECT_EQ(delivered, 0);
            EXPECT_TRUE(model.accepts(otherJob));
            if (!tracking)
            {
                play->click(); // A canceled frame must be requested again before playback can advance.
                ASSERT_TRUE(QTest::qWaitFor([&] { return slider->value() == slider->maximum() && play->accessibleName() == "Play"; }));
                EXPECT_GT(delivered, 0);
            }
        }
}

TEST(BoundWorkspace, DelayedCancellationDoesNotStopRestartedTrackingOrPlayback)
{
    for (const bool tracking : {false, true})
    {
        SCOPED_TRACE(tracking);
        QTemporaryDir temporary;
        auto workspace = std::make_shared<motrack::WorkspaceContext>();
        const auto source = addTrackingExperiment(*workspace, temporary);
        motrack::MainWindow view(workspace, source.documents[0]);
        auto* action = view.findChild<QAction*>("autoTracker");
        auto* play = view.findChild<QToolButton*>("playClip");
        auto* slider = view.findChild<QSlider*>("stepSlider");
        auto* marks = view.findChild<QTableWidget*>("marksTable");
        ASSERT_TRUE(action && play && slider && marks);
        const auto toggle = [&]
        {
            if (tracking)
                action->trigger();
            else
                play->click();
        };
        toggle();
        workspace->session().cancelJobs(source.documents[0]);
        // Stop and restart before processing the queued cancellation notification.
        toggle();
        toggle();
        EXPECT_TRUE(tracking ? action->isChecked() : play->accessibleName() == "Pause");
        ASSERT_TRUE(QTest::qWaitFor(
            [&] { return slider->value() == slider->maximum() && !action->isChecked() && play->accessibleName() == "Play"; }));
        EXPECT_EQ(marks->rowCount(), tracking ? 3 : 1);
    }
}

} // namespace
