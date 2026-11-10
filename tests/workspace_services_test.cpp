#include "workspace_services.hpp"

#include "main_window.hpp"
#include "movie_worker.hpp"
#include "track_identity.hpp"
#include "tracker_archive.hpp"

#include <QAction>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSlider>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <gtest/gtest.h>

#include <array>
#include <stdexcept>

namespace
{

using namespace std::chrono_literals;

QString fixturePath(const char* name = "../corpus/fixtures/linked-mov/constant-rate.mov")
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + '/' + name;
}

motrack::MovieFrameRequest request(int frame = 0)
{
    motrack::MovieFrameRequest result{.token = {},
                                      .media = QUuid::createUuid(),
                                      .path = fixturePath(),
                                      .frame = frame,
                                      .sequence = static_cast<quint64>(frame),
                                      .orientation = motrack::VideoOrientation::Encoded,
                                      .timestamp = frame * 125ms,
                                      .archive = {}};
    result.token = {
        .document = QUuid::createUuid(), .kind = motrack::WorkspaceJob::Frame, .cancelled = std::make_shared<std::atomic_bool>(false)};
    return result;
}

QImage expectedFrame(const QString& path, int frame, motrack::VideoOrientation orientation = motrack::VideoOrientation::Encoded)
{
    motrack::VideoReader reader(path, orientation);
    for (int number = 0; number <= frame; ++number)
    {
        const auto result = reader.readNext();
        if (!result)
            throw std::runtime_error("Missing test movie frame.");
        if (number == frame)
            return result->image;
    }
    throw std::runtime_error("Invalid expected frame number.");
}

TEST(WorkspaceServices, SharesBoundedCacheAcrossTwoDecoderOwnersAndEvictsTheLeastRecent)
{
    constexpr qsizetype ONE_FRAME = qsizetype{32} * 24 * 4;
    motrack::MovieWorker worker(ONE_FRAME);
    auto a = request();
    auto b = request(1);
    auto c = request(2);
    for (auto* current : {&a, &b, &c, &a})
    {
        const auto result = worker.readFrame(*current);
        ASSERT_FALSE(result.image.isNull());
        EXPECT_EQ(result.image.pixelColor(7, 9), QColor(7 + 16 * current->frame, 9, 32 * current->frame));
        EXPECT_EQ(worker.cacheBytes(), ONE_FRAME);
        EXPECT_LE(worker.decoderCount(), 2u);
    }
    EXPECT_EQ(worker.decoderCount(), 2u);
    EXPECT_EQ(worker.readFrame(a).image, expectedFrame(a.path, 0)); // Cached repeat.
    worker.clear(a.token.document);
    EXPECT_EQ(worker.decoderCount(), 1u);
    EXPECT_EQ(worker.cacheBytes(), 0);
    worker.clearAll();
    EXPECT_EQ(worker.decoderCount(), 0u);
    motrack::MovieWorker uncached(0);
    EXPECT_FALSE(uncached.readFrame(a).image.isNull());
    EXPECT_EQ(uncached.cacheBytes(), 0);
}

TEST(WorkspaceServices, ValidatesPathOrientationAndMediaGenerationBeforeUsingCachedFrames)
{
    QFile original(fixturePath("vfr.mp4"));
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    auto bytes = original.readAll();
    const auto header = bytes.indexOf("tkhd");
    ASSERT_GE(header, 0);
    ASSERT_EQ(bytes.at(header + 4), '\0');
    constexpr std::int32_t UNIT = 1 << 16;
    constexpr std::int32_t HOMOGENEOUS_UNIT = 1 << 30;
    constexpr std::array<std::int32_t, 9> ROTATION{0, UNIT, 0, -UNIT, 0, 0, 0, 0, HOMOGENEOUS_UNIT};
    for (std::size_t index = 0; index < ROTATION.size(); ++index)
        qToBigEndian(ROTATION[index], bytes.data() + header + 44 + static_cast<qsizetype>(index) * 4);
    QTemporaryDir temporary;
    QFile rotated(temporary.filePath("rotated.mp4"));
    ASSERT_TRUE(rotated.open(QIODevice::WriteOnly));
    ASSERT_EQ(rotated.write(bytes), bytes.size());
    rotated.close();
    motrack::MovieWorker worker;
    auto current = request();
    const auto movie = worker.readFrame(current).image;
    current.path = rotated.fileName(); // Same document/media ID and ordinal, different physical media.
    current.timestamp = 40ms;
    const auto encoded = worker.readFrame(current).image;
    EXPECT_EQ(encoded, expectedFrame(current.path, 0));
    EXPECT_NE(encoded, movie);
    current.orientation = motrack::VideoOrientation::Display;
    const auto display = worker.readFrame(current).image;
    EXPECT_EQ(display.size(), QSize(24, 32));
    EXPECT_EQ(display, expectedFrame(current.path, 0, current.orientation));
    current.media = QUuid::createUuid();
    EXPECT_EQ(worker.readFrame(current).image, display);
    current.frame = 3;
    current.timestamp = 368ms;
    EXPECT_EQ(worker.readFrame(current).image, expectedFrame(current.path, 3, current.orientation));
    current.frame = 1;
    current.timestamp = 168ms;
    EXPECT_EQ(worker.readFrame(current).image, expectedFrame(current.path, 1, current.orientation));
    current.timestamp.reset();
    EXPECT_EQ(worker.readFrame(current).image, expectedFrame(current.path, 1, current.orientation));
}

TEST(WorkspaceServices, RefusesCancelledAndMalformedRequestsAndRecoversAfterDecodeFailure)
{
    motrack::MovieWorker worker;
    auto current = request();
    current.token.cancelled->store(true);
    EXPECT_TRUE(worker.readFrame(current).image.isNull());
    EXPECT_EQ(worker.decoderCount(), 0u);
    current.token.cancelled = std::make_shared<std::atomic_bool>(false);
    current.path = fixturePath("missing.mp4");
    EXPECT_TRUE(worker.readFrame(current).image.isNull());
    EXPECT_EQ(worker.decoderCount(), 0u);
    current.path = fixturePath();
    current.frame = 100;
    current.timestamp.reset();
    EXPECT_TRUE(worker.readFrame(current).image.isNull());
    current.frame = 2;
    current.timestamp = 999ms; // Failed timestamp seek falls back to presentation-order decoding.
    EXPECT_EQ(worker.readFrame(current).image, expectedFrame(current.path, 2));
    current.frame = -1;
    EXPECT_TRUE(worker.readFrame(current).image.isNull());
    current.frame = 0;
    current.token.kind = motrack::WorkspaceJob::MediaIndex;
    EXPECT_TRUE(worker.readFrame(current).image.isNull());
    current.token.cancelled.reset();
    EXPECT_TRUE(worker.readFrame(current).image.isNull());
}

TEST(WorkspaceServices, DecoderRetainsArchiveUntilItsHandleCloses)
{
    motrack::MovieWorker worker;
    std::weak_ptr<const motrack::TrackerArchive> weak;
    QString owned;
    QUuid document;
    {
        auto current = request();
        current.archive = std::make_shared<motrack::TrackerArchive>(
            motrack::TrackerArchive::open(fixturePath("../corpus/fixtures/linked-mov/archived-movie.trz")));
        auto project = motrack::importTrackerDocument(current.archive->projectPath("experiment.trk")).project;
        current.archive->resolveMedia(project, "experiment.trk");
        ASSERT_FALSE(project.clip.mediaReferences.empty());
        current.path = QDir(current.archive->root()).filePath(QString::fromStdString(project.clip.mediaReferences.front()));
        owned = current.archive->sourcePath();
        weak = current.archive;
        document = current.token.document;
        ASSERT_FALSE(worker.readFrame(current).image.isNull());
    }
    EXPECT_FALSE(weak.expired());
    EXPECT_TRUE(QFileInfo::exists(owned));
    worker.clear(document);
    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(QFileInfo::exists(owned));
}

TEST(WorkspaceServices, CoalescesEachDocumentsFramesWithoutCancellingAnotherDocument)
{
    motrack::WorkspaceServices services;
    EXPECT_EQ(services.analysisPool().maxThreadCount(), 2);
    EXPECT_EQ(services.savePool().maxThreadCount(), 1);
    std::vector<motrack::MovieFrameResult> results;
    QObject::connect(
        &services, &motrack::WorkspaceServices::frameDecoded, &services, [&](const auto& result) { results.push_back(result); });
    auto a = request();
    auto b = request();
    services.requestFrame(a);
    services.requestFrame(b);
    auto newestB = b;
    newestB.token.cancelled = std::make_shared<std::atomic_bool>(false);
    newestB.frame = 1;
    newestB.timestamp = 125ms;
    newestB.sequence = 1;
    services.requestFrame(newestB);
    auto newestA = a;
    newestA.token.cancelled = std::make_shared<std::atomic_bool>(false);
    newestA.frame = 3;
    newestA.timestamp = 375ms;
    newestA.sequence = 3;
    services.requestFrame(newestA);
    EXPECT_TRUE(a.token.cancelled->load());
    EXPECT_TRUE(b.token.cancelled->load());
    EXPECT_FALSE(newestB.token.cancelled->load());
    ASSERT_TRUE(QTest::qWaitFor([&] { return results.size() == 2; }));
    EXPECT_EQ(results[0].request.token.document, b.token.document);
    EXPECT_EQ(results[0].request.frame, 1);
    EXPECT_EQ(results[0].image, expectedFrame(b.path, 1));
    EXPECT_EQ(results[1].request.token.document, a.token.document);
    EXPECT_EQ(results[1].request.frame, 3);
    EXPECT_EQ(results[1].image, expectedFrame(a.path, 3));
}

TEST(WorkspaceServices, ClearingOneDocumentLeavesAnotherActiveAndCanAcceptAFreshGeneration)
{
    motrack::WorkspaceServices services;
    std::vector<motrack::MovieFrameResult> results;
    QObject::connect(
        &services, &motrack::WorkspaceServices::frameDecoded, &services, [&](const auto& result) { results.push_back(result); });
    auto a = request(2);
    auto b = request(1);
    services.requestFrame(a);
    services.requestFrame(b);
    services.clearFrames(b.token.document);
    services.clearFrames(b.token.document);
    EXPECT_TRUE(b.token.cancelled->load());
    EXPECT_FALSE(a.token.cancelled->load());
    auto fresh = b;
    fresh.token.cancelled = std::make_shared<std::atomic_bool>(false);
    fresh.media = QUuid::createUuid();
    fresh.frame = 3;
    fresh.timestamp = 375ms;
    services.requestFrame(fresh);
    auto ignored = request();
    ignored.token.kind = motrack::WorkspaceJob::MediaIndex;
    services.requestFrame(ignored);
    services.requestFrame({});
    ASSERT_TRUE(QTest::qWaitFor([&] { return results.size() == 2; }));
    EXPECT_EQ(results[0].request.token.document, a.token.document);
    EXPECT_EQ(results[0].image, expectedFrame(a.path, 2));
    EXPECT_EQ(results[1].request.media, fresh.media);
    EXPECT_EQ(results[1].image, expectedFrame(fresh.path, 3));
}

TEST(WorkspaceServices, DestructionCancelsActiveAndPendingFramesAndAllowsDeletionFromAResult)
{
    auto a = request();
    auto b = request(1);
    {
        motrack::WorkspaceServices services;
        services.requestFrame(a);
        services.requestFrame(b);
    }
    EXPECT_TRUE(a.token.cancelled->load());
    EXPECT_TRUE(b.token.cancelled->load());
    bool delivered = false;
    bool destroyed = false;
    QObject owner;
    auto frame = request();
    auto* service = new motrack::WorkspaceServices;
    service->setParent(&owner); // Also cleans up if the bounded wait fails before the callback.
    QObject::connect(service, &QObject::destroyed, &owner, [&] { destroyed = true; });
    QObject::connect(service,
                     &motrack::WorkspaceServices::frameDecoded,
                     &owner,
                     [service, &delivered](const auto&)
                     {
                         delivered = true;
                         delete service;
                     });
    service->requestFrame(frame);
    ASSERT_TRUE(QTest::qWaitFor([&] { return delivered; }));
    EXPECT_TRUE(destroyed);
}

TEST(WorkspaceServices, TwoWindowsShareLimitsAndClosingOneKeepsTheOthersTrackingWork)
{
    QTemporaryDir temporary;
    motrack::WorkspaceDocument document;
    document.project.pointMasses.push_back(
        {.name = "Ball", .marks = {{.frame = 0, .x = 22, .y = 22}}, .id = motrack::allocateTrackId(document.project)});
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
    auto services = std::make_shared<motrack::WorkspaceServices>();
    motrack::MainWindow first(services);
    auto second = std::make_unique<motrack::MainWindow>(services);
    EXPECT_EQ(&first.executionServices(), &second->executionServices());
    ASSERT_TRUE(first.loadProject(path));
    ASSERT_TRUE(second->loadProject(fixturePath("movie.trk")));
    auto* otherCanvas = second->findChild<QWidget*>("videoCanvas");
    ASSERT_TRUE(otherCanvas);
    ASSERT_TRUE(QTest::qWaitFor([&] { return otherCanvas->accessibleName() == "Frame 1"; }));
    QSemaphore entered;
    QSemaphore release;
    std::atomic_bool timedOut{false};
    const auto cleanup = qScopeGuard(
        [&]
        {
            release.release(2);
            (void)services->analysisPool().waitForDone(5000);
        });
    for (int index = 0; index < 2; ++index)
        services->analysisPool().start(
            [&]
            {
                entered.release();
                if (!release.tryAcquire(1, 5000))
                    timedOut = true;
            });
    ASSERT_TRUE(entered.tryAcquire(2, 5000));
    auto* action = first.findChild<QAction*>("autoTracker");
    auto* slider = first.findChild<QSlider*>("stepSlider");
    auto* marks = first.findChild<QTableWidget*>("marksTable");
    ASSERT_TRUE(action && slider && marks);
    action->trigger();
    slider->setValue(1);
    ASSERT_TRUE(action->isChecked());
    second.reset();
    EXPECT_FALSE(timedOut.load());
    EXPECT_TRUE(action->isChecked());
    EXPECT_EQ(marks->rowCount(), 1);
    release.release(2);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !action->isChecked() && marks->rowCount() == 3; }));
    EXPECT_EQ(marks->item(1, 2)->text(), "28");
    EXPECT_EQ(marks->item(2, 2)->text(), "34");
    EXPECT_TRUE(first.isWindowModified());
}

TEST(WorkspaceServices, WindowNotifiesDestructionBeforeItsLastServiceOwnerDrainsJobs)
{
    QSemaphore entered;
    QSemaphore release;
    std::atomic_bool timedOut{false};
    bool notified = false;
    auto window = std::make_unique<motrack::MainWindow>();
    const auto cleanup = qScopeGuard(
        [&]
        {
            release.release();
            window.reset();
        });
    QObject::connect(window.get(),
                     &QObject::destroyed,
                     [&]
                     {
                         notified = true;
                         release.release();
                     });
    window->executionServices().savePool().start(
        [&]
        {
            entered.release();
            if (!release.tryAcquire(1, 2500))
                timedOut = true;
        });
    ASSERT_TRUE(entered.tryAcquire(1, 2500));
    window.reset();
    EXPECT_TRUE(notified);
    EXPECT_FALSE(timedOut.load());
}

} // namespace
