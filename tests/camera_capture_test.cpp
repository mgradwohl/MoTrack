#include "capture_dialog.hpp"
#include "video_reader.hpp"
#include "workspace_window.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLockFile>
#include <QMediaFormat>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace
{

using State = motrack::CaptureDialog::State;

// Camera times (µs) with uneven intervals, and the frame times MoTrack must report for them: relative
// to the first frame, at the movie's 1/90000 s timescale. Never a nominal frame rate.
constexpr std::array<qint64, 5> CAMERA_TIMES{5'000'000, 5'033'333, 5'070'000, 5'099'000, 5'140'500};
constexpr std::array<double, 5> FRAME_TIMES{0.0, 3000.0 / 90000, 6300.0 / 90000, 8910.0 / 90000, 12645.0 / 90000};

/// A deterministic camera: devices, formats, permission and the frame-time field are fields; frames are
/// delivered on demand from another thread (Qt 6.8 uses the GUI thread; the sink must cope with either).
class FakeCamera final : public motrack::CameraSource
{
  public:
    QList<motrack::CameraDevice> cameras{
        {.id = "bench",
         .description = "Bench camera",
         .isDefault = true,
         .formats = {{.resolution = {32, 24}, .minimumFrameRate = 15, .maximumFrameRate = 60, .pixelFormat = "YUYV"},
                     {.resolution = {64, 48}, .minimumFrameRate = 30, .maximumFrameRate = 30, .pixelFormat = "NV12"},
                     {.resolution = {64, 48}, .minimumFrameRate = 30, .maximumFrameRate = 30, .pixelFormat = "NV12"}}},
        {.id = "spare",
         .description = "Spare camera",
         .isDefault = false,
         .formats = {{.resolution = {16, 12}, .minimumFrameRate = 0, .maximumFrameRate = 0, .pixelFormat = "MJPEG"}}}};
    motrack::CameraAccess permission = motrack::CameraAccess::Granted;
    motrack::CameraAccess answer = motrack::CameraAccess::Granted;
    motrack::FrameTimeField timeField = motrack::FrameTimeField::StartTime;
    QByteArray device;
    int format = -2;
    int starts = 0;
    int requests = 0;
    bool running = false;

    [[nodiscard]] QList<motrack::CameraDevice> devices() const override
    {
        return cameras;
    }
    [[nodiscard]] motrack::CameraAccess access() const override
    {
        return permission;
    }
    void requestAccess() override
    {
        ++requests;
        QTimer::singleShot(0,
                           this,
                           [this]
                           {
                               permission = answer;
                               Q_EMIT accessChanged();
                           });
    }
    void start(const QByteArray& id, int index, FrameSink frames) override
    {
        device = id;
        format = index;
        ++starts;
        m_Sink = std::move(frames);
        running = true;
        QTimer::singleShot(0,
                           this,
                           [this]
                           {
                               if (running)
                                   Q_EMIT started();
                           });
    }
    void stop() override
    {
        running = false;
        m_Sink = {};
    }
    [[nodiscard]] motrack::FrameTimeField frameTimeField() const override
    {
        return timeField;
    }
    void deliver(const QVideoFrame& frame) const
    {
        if (!m_Sink)
            return;
        std::thread([sink = m_Sink, frame] { sink(frame); }).join();
    }
    void deliver(QColor color, qint64 time, qint64 end = -1, QSize size = {64, 48}) const
    {
        QImage image(size, QImage::Format_RGB32);
        image.fill(color);
        QVideoFrame frame(image);
        frame.setStartTime(time);
        frame.setEndTime(end);
        deliver(frame);
    }

  private:
    FrameSink m_Sink;
};

QString fixture(const char* name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/" + name;
}

template <typename Widget> Widget* child(const QObject& owner, const char* name)
{
    auto* widget = owner.findChild<Widget*>(name);
    if (!widget)
        throw std::runtime_error(std::string("Missing ") + name);
    return widget;
}

bool waitForState(const motrack::CaptureDialog& dialog, State state)
{
    return QTest::qWaitFor([&] { return dialog.state() == state; });
}

QString statusText(const motrack::CaptureDialog& dialog)
{
    return child<QLabel>(dialog, "captureStatus")->text();
}

void record(motrack::CaptureDialog& dialog, const FakeCamera& camera, std::size_t frames = CAMERA_TIMES.size())
{
    child<QPushButton>(dialog, "captureRecord")->click();
    ASSERT_EQ(dialog.state(), State::Recording);
    for (std::size_t frame = 0; frame < frames; ++frame)
        camera.deliver(QColor::fromHsv(static_cast<int>(frame) * 60, 200, 200), CAMERA_TIMES.at(frame));
}

// Answers the discard question with the given buttons in turn, as a user would; Cancel afterwards.
class DiscardAnswers
{
  public:
    explicit DiscardAnswers(std::vector<QMessageBox::StandardButton> answers) : m_Answers(std::move(answers))
    {
        QObject::connect(&m_Timer,
                         &QTimer::timeout,
                         [this]
                         {
                             auto* question = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                             if (question && question->objectName() == "captureDiscardQuestion")
                                 question->button(m_Next < m_Answers.size() ? m_Answers[m_Next++] : QMessageBox::Cancel)->click();
                         });
        m_Timer.start(1);
    }
    [[nodiscard]] std::size_t count() const
    {
        return m_Next;
    }

  private:
    QTimer m_Timer;
    std::vector<QMessageBox::StandardButton> m_Answers;
    std::size_t m_Next = 0;
};

struct Capture
{
    std::unique_ptr<motrack::WorkspaceWindow> window = std::make_unique<motrack::WorkspaceWindow>();
    QPointer<FakeCamera> camera;
    QPointer<motrack::CaptureDialog> dialog;

    Capture()
    {
        window->setCameraSourceFactory(
            [this]
            {
                auto source = std::make_unique<FakeCamera>();
                camera = source.get();
                return source;
            });
        window->show();
    }
    void open()
    {
        child<QAction>(*window, "workspaceCaptureCamera")->trigger();
        dialog = window->findChild<motrack::CaptureDialog*>();
        if (!dialog || !waitForState(*dialog, State::Previewing))
            throw std::runtime_error("The capture window did not start previewing.");
    }
};

TEST(CameraCapture, RecordsCameraFrameTimesAndOpensTheSavedClipAsANewExperiment)
{
    QTemporaryDir temporary;
    Capture capture;
    capture.open();
    auto& dialog = *capture.dialog;
    EXPECT_EQ(capture.window->captureFromCamera(), capture.dialog.data()); // One capture window at a time.
    EXPECT_EQ(capture.camera->device, "bench");
    EXPECT_EQ(capture.camera->format, -1);
    capture.camera->deliver(Qt::red, 0);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !dialog.previewImage().isNull(); }));
    EXPECT_EQ(dialog.previewImage().size(), QSize(64, 48));
    EXPECT_GT(qRed(dialog.previewImage().pixel(10, 10)), 200);

    record(dialog, *capture.camera);
    const auto clip = dialog.temporaryRecording();
    EXPECT_TRUE(QFileInfo(clip).absoluteDir().exists());
    EXPECT_FALSE(child<QComboBox>(dialog, "captureCamera")->isEnabled());
    ASSERT_TRUE(QTest::qWaitFor([&] { return child<QLabel>(dialog, "captureElapsed")->text().contains("Frames 5"); }));
    child<QPushButton>(dialog, "captureRecord")->click();
    ASSERT_TRUE(waitForState(dialog, State::Recorded));
    ASSERT_TRUE(dialog.recording());
    EXPECT_EQ(dialog.recording()->frames, 5);
    EXPECT_EQ(dialog.recording()->dropped, 0);
    EXPECT_EQ(dialog.recording()->duration, std::chrono::microseconds{140'500});
    EXPECT_EQ(dialog.recording()->clock, motrack::CaptureClock::Camera);
    EXPECT_TRUE(statusText(dialog).contains("Recorded 5 frames"));
    EXPECT_TRUE(QFileInfo::exists(clip));

    // The Save dialog offers a permanent file; the clip then opens as a new experiment.
    const auto saved = temporary.filePath(QString::fromUtf8("lab clip 測定.mov"));
    QSignalSpy opened(capture.window->context().get(), &motrack::WorkspaceContext::inputOpened);
    child<QPushButton>(dialog, "captureSave")->click();
    auto* chooser = child<QFileDialog>(dialog, "captureSaveDialog");
    // Qt leaves a focused filename editor unchanged in selectFile().
    if (auto* focused = chooser->focusWidget())
        focused->clearFocus();
    chooser->selectFile(saved);
    ASSERT_TRUE(QTest::qWaitFor([&] { return chooser->selectedFiles() == QStringList{saved}; }, 1500));
    QMetaObject::invokeMethod(chooser, "accept");
    ASSERT_TRUE(QTest::qWaitFor([&] { return opened.count() == 1; }));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !capture.dialog; }));
    EXPECT_TRUE(QFileInfo::exists(saved));
    EXPECT_FALSE(QFileInfo::exists(clip));
    EXPECT_FALSE(QFileInfo(clip).absoluteDir().exists());
    EXPECT_TRUE(capture.window->statusBar()->currentMessage().contains("Recording saved"));

    const auto document = capture.window->currentDocument();
    ASSERT_TRUE(document);
    auto& session = capture.window->context()->session();
    const auto& experiment = session.document(*document);
    EXPECT_TRUE(experiment.movie);
    EXPECT_EQ(QFileInfo(session.sourcePath(session.sourceId(*document))).fileName(), QFileInfo(saved).fileName());
    EXPECT_EQ(experiment.project.clip.stepCount, 5);
    EXPECT_FALSE(experiment.project.clip.frameDurationMs); // Frame times come from the file, not a nominal rate.
    ASSERT_EQ(experiment.movieFrameTimes.size(), FRAME_TIMES.size());
    for (std::size_t frame = 0; frame < FRAME_TIMES.size(); ++frame)
        EXPECT_NEAR(experiment.movieFrameTimes.at(frame), FRAME_TIMES.at(frame), 1e-9) << frame;
    EXPECT_GT(std::abs((experiment.movieFrameTimes[2] - experiment.movieFrameTimes[1]) -
                       (experiment.movieFrameTimes[1] - experiment.movieFrameTimes[0])),
              1e-3);
}

TEST(CameraCapture, CancelWindowCloseAndDestructionStopRecordingAndRemoveTheTemporaryClip)
{
    {
        // Cancel asks first; keeping the recording leaves it running.
        Capture capture;
        capture.open();
        record(*capture.dialog, *capture.camera, 2);
        const auto folder = QFileInfo(capture.dialog->temporaryRecording()).absolutePath();
        ASSERT_TRUE(QDir(folder).exists());
        {
            const DiscardAnswers answers({QMessageBox::Cancel});
            child<QPushButton>(*capture.dialog, "captureCancel")->click();
            EXPECT_EQ(answers.count(), 1u);
        }
        EXPECT_EQ(capture.dialog->state(), State::Recording);
        EXPECT_TRUE(QDir(folder).exists());
        {
            const DiscardAnswers answers({QMessageBox::Discard});
            child<QPushButton>(*capture.dialog, "captureCancel")->click();
            EXPECT_EQ(answers.count(), 1u);
        }
        ASSERT_TRUE(QTest::qWaitFor([&] { return !capture.dialog; }));
        EXPECT_FALSE(QDir(folder).exists());
        EXPECT_FALSE(capture.camera);
        EXPECT_TRUE(capture.window->context()->session().sources().empty());
    }
    {
        // Closing the workspace asks too: keeping the recording cancels the close.
        Capture capture;
        capture.open();
        record(*capture.dialog, *capture.camera, 3);
        const auto folder = QFileInfo(capture.dialog->temporaryRecording()).absolutePath();
        {
            const DiscardAnswers answers({QMessageBox::Cancel});
            EXPECT_FALSE(capture.window->close());
            EXPECT_EQ(answers.count(), 1u);
        }
        EXPECT_TRUE(capture.window->isVisible());
        EXPECT_EQ(capture.dialog->state(), State::Recording);
        {
            const DiscardAnswers answers({QMessageBox::Discard});
            EXPECT_TRUE(capture.window->close());
            EXPECT_EQ(answers.count(), 1u);
        }
        ASSERT_TRUE(QTest::qWaitFor([&] { return !capture.dialog; }));
        EXPECT_FALSE(QDir(folder).exists());
    }
    {
        // Destroying the workspace mid-recording closes the movie and removes it before returning.
        Capture capture;
        capture.open();
        record(*capture.dialog, *capture.camera, 3);
        const auto folder = QFileInfo(capture.dialog->temporaryRecording()).absolutePath();
        capture.window.reset();
        EXPECT_FALSE(capture.dialog);
        EXPECT_FALSE(QDir(folder).exists());
    }
    {
        // Esc on a finished but unsaved recording asks; discarding removes it with the window.
        Capture capture;
        capture.open();
        record(*capture.dialog, *capture.camera);
        child<QPushButton>(*capture.dialog, "captureRecord")->click();
        ASSERT_TRUE(waitForState(*capture.dialog, State::Recorded));
        const auto clip = capture.dialog->temporaryRecording();
        EXPECT_EQ(child<QPushButton>(*capture.dialog, "captureCancel")->text(), "Discard and close");
        const auto escape = [&]
        {
            QTest::keyPress(capture.dialog.data(), Qt::Key_Escape);
            if (capture.dialog)
                QTest::keyRelease(capture.dialog.data(), Qt::Key_Escape);
        };
        {
            const DiscardAnswers answers({QMessageBox::Cancel});
            escape();
            EXPECT_EQ(answers.count(), 1u);
        }
        ASSERT_TRUE(capture.dialog);
        EXPECT_EQ(capture.dialog->state(), State::Recorded);
        EXPECT_TRUE(QFileInfo::exists(clip));
        {
            const DiscardAnswers answers({QMessageBox::Discard});
            escape();
            EXPECT_EQ(answers.count(), 1u);
        }
        EXPECT_FALSE(QFileInfo::exists(clip));
        ASSERT_TRUE(QTest::qWaitFor([&] { return !capture.dialog; }));
        EXPECT_TRUE(capture.window->context()->session().sources().empty());
    }
    {
        // Nothing to lose: closing while previewing does not ask.
        Capture capture;
        capture.open();
        const DiscardAnswers answers({});
        capture.dialog->reject();
        EXPECT_EQ(answers.count(), 0u);
        ASSERT_TRUE(QTest::qWaitFor([&] { return !capture.dialog; }));
    }
}

TEST(CameraCapture, MacOsFrameTimesComeFromEndTimeAndADisconnectedCameraKeepsTheClip)
{
    // Qt 6.8 to 6.11's AVFoundation camera puts the previous frame's time in startTime and the frame's
    // own in endTime; reading startTime would shift every interval by one frame.
    auto source = std::make_unique<FakeCamera>();
    auto* camera = source.get();
    camera->timeField = motrack::FrameTimeField::EndTime;
    motrack::CaptureDialog dialog(std::move(source));
    ASSERT_TRUE(waitForState(dialog, State::Previewing));
    child<QPushButton>(dialog, "captureRecord")->click();
    for (std::size_t frame = 0; frame < CAMERA_TIMES.size(); ++frame)
        camera->deliver(Qt::red, CAMERA_TIMES.at(frame == 0 ? 0 : frame - 1), CAMERA_TIMES.at(frame));
    ASSERT_TRUE(QTest::qWaitFor([&] { return child<QLabel>(dialog, "captureElapsed")->text().contains("Frames 5"); }));
    // V4L2 reports an unplugged camera only by its disappearance from the device list.
    camera->cameras.removeFirst();
    Q_EMIT camera->devicesChanged();
    ASSERT_TRUE(waitForState(dialog, State::Recorded));
    ASSERT_TRUE(dialog.recording());
    EXPECT_EQ(dialog.recording()->frames, 5);
    EXPECT_TRUE(statusText(dialog).contains("disconnected") || statusText(dialog).contains("camera stopped"));
    const std::atomic_bool cancelled{false};
    const auto index = motrack::buildMovieIndex(dialog.recording()->path, motrack::VideoOrientation::Display, cancelled);
    ASSERT_EQ(index.frameTimes.size(), FRAME_TIMES.size());
    for (std::size_t frame = 0; frame < FRAME_TIMES.size(); ++frame)
        EXPECT_NEAR(index.frameTimes.at(frame), FRAME_TIMES.at(frame), 1e-9) << frame;
}

TEST(CameraCapture, ReportsStalledCamerasGapsAndEndsAClipWhenTheFrameSizeChanges)
{
    auto source = std::make_unique<FakeCamera>();
    auto* camera = source.get();
    motrack::CaptureDialog dialog(std::move(source));
    dialog.setStallWarningAfter(std::chrono::milliseconds(50));
    ASSERT_TRUE(waitForState(dialog, State::Previewing));
    child<QPushButton>(dialog, "captureRecord")->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return statusText(dialog).contains("No frames have arrived"); }, 5000));
    // A frame clears the warning. Intervals 10, 10, 10 and 40 ms: one exceeds 1.5 times the 10 ms median.
    for (const qint64 time : {0, 10'000, 20'000, 30'000, 70'000})
        camera->deliver(Qt::green, time);
    ASSERT_TRUE(QTest::qWaitFor([&] { return child<QLabel>(dialog, "captureElapsed")->text().contains("Frames 5"); }));
    EXPECT_FALSE(statusText(dialog).contains("No frames have arrived"));
    // A new frame size ends the clip at the last frame of the original size, instead of scaling it.
    camera->deliver(Qt::green, 80'000, -1, {32, 24});
    ASSERT_TRUE(waitForState(dialog, State::Recorded));
    ASSERT_TRUE(dialog.recording());
    EXPECT_TRUE(dialog.recording()->sizeChanged);
    EXPECT_EQ(dialog.recording()->frames, 5);
    EXPECT_EQ(dialog.recording()->gaps, 1);
    EXPECT_EQ(dialog.recording()->medianInterval, std::chrono::microseconds{10'000});
    EXPECT_EQ(dialog.recording()->longestInterval, std::chrono::microseconds{40'000});
    EXPECT_TRUE(statusText(dialog).contains("changed its frame size"));
    EXPECT_TRUE(statusText(dialog).contains("longer than 1.5 times the median"));

    using std::chrono::microseconds;
    EXPECT_EQ(motrack::captureIntervals({}).gaps, 0);
    EXPECT_EQ(motrack::captureIntervals({microseconds{5}}).median, microseconds{0});
    const auto even = motrack::captureIntervals({microseconds{0}, microseconds{10}, microseconds{30}, microseconds{45}});
    EXPECT_EQ(even.median, microseconds{15});
    EXPECT_EQ(even.longest, microseconds{20});
    EXPECT_EQ(even.gaps, 0); // 20 is not more than 1.5 × 15.
}

TEST(CameraCapture, SweepRemovesOnlyFoldersNoRunningMoTrackOwns)
{
    QTemporaryDir root;
    const QDir folders(root.path());
    for (const auto* name : {"motrack-capture-stale", "motrack-capture-live", "motrack-capture-recent", "unrelated"})
        ASSERT_TRUE(folders.mkpath(name));
    // A lock exactly as QLockFile writes it, but for a process that no longer exists (a crash).
    {
        QLockFile probe(root.filePath("probe.lock"));
        ASSERT_TRUE(probe.lock());
        QFile written(root.filePath("probe.lock"));
        ASSERT_TRUE(written.open(QIODevice::ReadOnly));
        auto lines = written.readAll().split('\n');
        lines[0] = "2147483644";
        QFile stale(root.filePath("motrack-capture-stale/" + motrack::captureLockName()));
        ASSERT_TRUE(stale.open(QIODevice::WriteOnly));
        stale.write(lines.join('\n'));
    }
    QLockFile live(root.filePath("motrack-capture-live/" + motrack::captureLockName()));
    ASSERT_TRUE(live.lock());
    QFile clip(root.filePath("motrack-capture-stale/capture.mov"));
    ASSERT_TRUE(clip.open(QIODevice::WriteOnly));
    clip.close();
    EXPECT_EQ(motrack::sweepCaptureFolders(folders), 1);
    EXPECT_FALSE(folders.exists("motrack-capture-stale"));
    EXPECT_TRUE(folders.exists("motrack-capture-live"));   // Its owner is running.
    EXPECT_TRUE(folders.exists("motrack-capture-recent")); // No lock, but too new to judge.
    EXPECT_TRUE(folders.exists("unrelated"));
}

TEST(CameraCapture, QtsMediaBackendLoadsBesideMoTracksFFmpeg)
{
    // Loads Qt Multimedia's FFmpeg backend (and Qt's own FFmpeg libraries) without opening a camera, then
    // decodes with MoTrack's FFmpeg in the same process.
    EXPECT_FALSE(QMediaFormat().supportedFileFormats(QMediaFormat::Decode).isEmpty());
    const std::atomic_bool cancelled{false};
    const auto index = motrack::buildMovieIndex(fixture("vfr.mp4"), motrack::VideoOrientation::Display, cancelled);
    EXPECT_EQ(index.frameCount, 4);
    EXPECT_NEAR(index.frameTimes.back(), 0.328, 1e-12);
}
TEST(CameraCapture, FailedRecordingsAndSavesLeaveTheWorkspaceUnchanged)
{
    QTemporaryDir temporary;
    Capture capture;
    capture.open();
    auto& dialog = *capture.dialog;
    // Stopping before any frame arrives records nothing.
    child<QPushButton>(dialog, "captureRecord")->click();
    child<QPushButton>(dialog, "captureRecord")->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return dialog.state() == State::Previewing && statusText(dialog).contains("No frames"); }));
    EXPECT_TRUE(dialog.temporaryRecording().isEmpty());

    // A camera that stops mid-recording keeps the frames recorded so far; discarding them removes the clip.
    record(dialog, *capture.camera, 2);
    const auto folder = QFileInfo(dialog.temporaryRecording()).absolutePath();
    Q_EMIT capture.camera->failed("unplugged");
    EXPECT_EQ(dialog.state(), State::Finishing);
    EXPECT_TRUE(statusText(dialog).contains("unplugged"));
    ASSERT_TRUE(waitForState(dialog, State::Recorded));
    ASSERT_TRUE(dialog.recording());
    EXPECT_EQ(dialog.recording()->frames, 2);
    EXPECT_TRUE(statusText(dialog).contains("camera stopped during the recording"));
    Q_EMIT capture.camera->failed("still unplugged");
    EXPECT_EQ(dialog.state(), State::Recorded);
    child<QPushButton>(dialog, "captureDiscard")->click();
    EXPECT_FALSE(QDir(folder).exists());
    // Discarding restarts the camera; a failure while previewing leaves only the camera choice.
    ASSERT_TRUE(waitForState(dialog, State::Previewing));
    Q_EMIT capture.camera->failed("unplugged");
    EXPECT_EQ(dialog.state(), State::Unavailable);
    EXPECT_FALSE(child<QPushButton>(dialog, "captureRecord")->isEnabled());

    // Choosing the camera again restarts it.
    child<QComboBox>(dialog, "captureCamera")->setCurrentIndex(1);
    child<QComboBox>(dialog, "captureCamera")->setCurrentIndex(0);
    ASSERT_TRUE(waitForState(dialog, State::Previewing));
    record(dialog, *capture.camera);
    child<QPushButton>(dialog, "captureRecord")->click();
    ASSERT_TRUE(waitForState(dialog, State::Recorded));
    const auto clip = dialog.temporaryRecording();

    // A destination that cannot be written keeps the recording for another try.
    dialog.saveRecording(temporary.filePath("missing/clip.mov"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return dialog.state() == State::Recorded && statusText(dialog).contains("Could not save"); }));
    EXPECT_TRUE(QFileInfo::exists(clip));
    child<QPushButton>(dialog, "captureDiscard")->click();
    EXPECT_EQ(dialog.state(), State::Previewing);
    EXPECT_FALSE(QFileInfo::exists(clip));
    EXPECT_TRUE(capture.window->context()->session().sources().empty());
    EXPECT_EQ(capture.window->findChild<QTabWidget*>("experimentTabs")->count(), 0);
}

TEST(CameraCapture, ReportsMissingCamerasDeniedAccessAndUnreadableFormats)
{
    {
        auto source = std::make_unique<FakeCamera>();
        auto* camera = source.get();
        camera->cameras.clear();
        motrack::CaptureDialog dialog(std::move(source));
        EXPECT_EQ(dialog.state(), State::Unavailable);
        EXPECT_TRUE(statusText(dialog).contains("No camera"));
        EXPECT_FALSE(child<QPushButton>(dialog, "captureRecord")->isEnabled());
        EXPECT_FALSE(child<QComboBox>(dialog, "captureCamera")->isEnabled());
        camera->cameras = FakeCamera{}.cameras;
        Q_EMIT camera->devicesChanged(); // A camera is connected.
        ASSERT_TRUE(waitForState(dialog, State::Previewing));
        EXPECT_EQ(camera->device, "bench");
    }
    {
        auto source = std::make_unique<FakeCamera>();
        source->permission = motrack::CameraAccess::Denied;
        motrack::CaptureDialog dialog(std::move(source));
        EXPECT_EQ(dialog.state(), State::Unavailable);
        EXPECT_TRUE(statusText(dialog).contains("not allowed"));
    }
    for (const auto answer : {motrack::CameraAccess::Granted, motrack::CameraAccess::Denied, motrack::CameraAccess::Undetermined})
    {
        auto source = std::make_unique<FakeCamera>();
        auto* camera = source.get();
        camera->permission = motrack::CameraAccess::Undetermined;
        camera->answer = answer;
        motrack::CaptureDialog dialog(std::move(source));
        EXPECT_EQ(dialog.state(), State::WaitingForAccess);
        const auto expected = answer == motrack::CameraAccess::Granted ? State::Previewing : State::Unavailable;
        ASSERT_TRUE(waitForState(dialog, expected));
        EXPECT_EQ(camera->requests, 1);
        EXPECT_EQ(camera->starts, answer == motrack::CameraAccess::Granted ? 1 : 0);
    }
    {
        auto source = std::make_unique<FakeCamera>();
        auto* camera = source.get();
        motrack::CaptureDialog dialog(std::move(source));
        ASSERT_TRUE(waitForState(dialog, State::Previewing));
        // Formats list largest first without duplicates; choosing one restarts the camera with it.
        auto* formats = child<QComboBox>(dialog, "captureFormat");
        ASSERT_EQ(formats->count(), 3);
        EXPECT_EQ(formats->itemText(1), QString::fromUtf8("64 × 48, 30 fps (NV12)"));
        EXPECT_EQ(formats->itemText(2), QString::fromUtf8("32 × 24, 15–60 fps (YUYV)"));
        formats->setCurrentIndex(2);
        ASSERT_TRUE(waitForState(dialog, State::Previewing));
        EXPECT_EQ(camera->format, 0);
        EXPECT_EQ(camera->starts, 2);
        // A pixel layout MoTrack cannot read is reported instead of silently recorded as nothing.
        camera->deliver(QVideoFrame(QVideoFrameFormat({8, 8}, QVideoFrameFormat::Format_AYUV)));
        ASSERT_TRUE(QTest::qWaitFor([&] { return statusText(dialog).contains("cannot read this camera format"); }));
        // A camera failure during preview leaves the camera choice available.
        Q_EMIT camera->failed("busy");
        EXPECT_EQ(dialog.state(), State::Unavailable);
        EXPECT_TRUE(child<QComboBox>(dialog, "captureCamera")->isEnabled());
        // A mode without a reported frame rate shows none.
        child<QComboBox>(dialog, "captureCamera")->setCurrentIndex(1);
        ASSERT_TRUE(waitForState(dialog, State::Previewing));
        EXPECT_EQ(camera->device, "spare");
        EXPECT_EQ(formats->itemText(1), QString::fromUtf8("16 × 12 (MJPEG)"));
    }
}

TEST(CameraCapture, FramesWithoutCameraTimesAreTimedOnArrivalAndMixedClocksAreSkipped)
{
    QTemporaryDir temporary;
    auto source = std::make_unique<FakeCamera>();
    auto* camera = source.get();
    motrack::CaptureDialog dialog(std::move(source));
    ASSERT_TRUE(waitForState(dialog, State::Previewing));
    child<QPushButton>(dialog, "captureRecord")->click();
    for (int frame = 0; frame < 3; ++frame)
    {
        camera->deliver(Qt::blue, -1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    child<QPushButton>(dialog, "captureRecord")->click();
    ASSERT_TRUE(waitForState(dialog, State::Recorded));
    ASSERT_TRUE(dialog.recording());
    EXPECT_EQ(dialog.recording()->clock, motrack::CaptureClock::Arrival);
    EXPECT_EQ(dialog.recording()->frames, 3);
    EXPECT_GE(dialog.recording()->duration, std::chrono::microseconds{9'000});
    EXPECT_TRUE(statusText(dialog).contains("timed by when it reached MoTrack"));
    child<QPushButton>(dialog, "captureDiscard")->click();

    child<QPushButton>(dialog, "captureRecord")->click();
    camera->deliver(Qt::blue, 1'000);
    camera->deliver(Qt::blue, -1);    // No camera time: cannot be placed on the camera clock.
    camera->deliver(Qt::blue, 1'000); // Repeats the previous time.
    camera->deliver(Qt::blue, 41'000);
    child<QPushButton>(dialog, "captureRecord")->click();
    ASSERT_TRUE(waitForState(dialog, State::Recorded));
    ASSERT_TRUE(dialog.recording());
    EXPECT_EQ(dialog.recording()->clock, motrack::CaptureClock::Camera);
    EXPECT_EQ(dialog.recording()->frames, 2);
    EXPECT_EQ(dialog.recording()->dropped, 2);
    EXPECT_EQ(dialog.recording()->duration, std::chrono::microseconds{40'000});
    EXPECT_TRUE(statusText(dialog).contains("dropped"));
    EXPECT_TRUE(child<QLabel>(dialog, "captureElapsed")->text().contains("Dropped 2"));
}

TEST(CameraCapture, MovingARecordingReplacesTheTargetOnlyWhenComplete)
{
    QTemporaryDir temporary;
    const auto write = [&](const QString& name, const QByteArray& content)
    {
        QFile file(temporary.filePath(name));
        return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
    };
    const std::atomic_bool running{false};
    ASSERT_TRUE(write("one.mov", "first"));
    ASSERT_TRUE(write("existing.mov", "old"));
    EXPECT_EQ(motrack::moveCaptureFile(temporary.filePath("one.mov"), temporary.filePath("existing.mov"), running), QString{});
    EXPECT_FALSE(QFileInfo::exists(temporary.filePath("one.mov")));
    QFile replaced(temporary.filePath("existing.mov"));
    ASSERT_TRUE(replaced.open(QIODevice::ReadOnly));
    EXPECT_EQ(replaced.readAll(), "first");
    replaced.close();

    // The copy route (another volume) stages beside the target, then renames over it.
    ASSERT_TRUE(write("two.mov", QByteArray(3 << 20, 'x')));
    EXPECT_EQ(motrack::moveCaptureFile(temporary.filePath("two.mov"), temporary.filePath("existing.mov"), running, false), QString{});
    EXPECT_EQ(QFileInfo(temporary.filePath("existing.mov")).size(), 3 << 20);
    EXPECT_FALSE(QFileInfo::exists(temporary.filePath("two.mov")));
    EXPECT_EQ(QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);

    ASSERT_TRUE(write("three.mov", "third"));
    const std::atomic_bool cancelled{true};
    EXPECT_TRUE(
        motrack::moveCaptureFile(temporary.filePath("three.mov"), temporary.filePath("new.mov"), cancelled, false).contains("canceled"));
    EXPECT_TRUE(QFileInfo::exists(temporary.filePath("three.mov")));
    EXPECT_FALSE(QFileInfo::exists(temporary.filePath("new.mov")));
    EXPECT_EQ(QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden).size(), 2);
    EXPECT_FALSE(motrack::moveCaptureFile(temporary.filePath("missing.mov"), temporary.filePath("new.mov"), running).isEmpty());
    EXPECT_FALSE(motrack::moveCaptureFile(temporary.filePath("three.mov"), temporary.filePath("missing/new.mov"), running).isEmpty());
    ASSERT_TRUE(QDir(temporary.path()).mkdir("folder"));
    EXPECT_FALSE(motrack::moveCaptureFile(temporary.filePath("three.mov"), temporary.filePath("folder"), running, false).isEmpty());
    EXPECT_TRUE(QFileInfo::exists(temporary.filePath("three.mov")));
    EXPECT_EQ(QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden).size(), 2);
}

} // namespace
