#include "capture_pipeline.hpp"

#include "capture_encoder.hpp"
#include "filesystem_path.hpp"
#include "video_reader.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QStandardPaths>
#include <QUuid>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

namespace motrack
{
namespace
{

using namespace std::chrono;

// Frames waiting for the worker while recording: about a quarter second at 30 Hz.
constexpr std::size_t RECORDING_QUEUE = 8;
constexpr qint64 COPY_CHUNK = qint64{1} << 20;
// A capture folder without a lock file is left alone this long, in case its owner is about to lock it.
constexpr qint64 UNLOCKED_FOLDER_AGE_SECONDS = qint64{24} * 60 * 60;

// Messages users see; explicit contexts, because lupdate does not resolve this file's classes.
VideoError frameError(const char* text)
{
    return VideoError{QCoreApplication::translate("motrack::CapturePipeline", text).toStdString()};
}

std::optional<CapturePixelFormat> capturePixelFormat(QVideoFrameFormat::PixelFormat format)
{
    switch (format)
    {
    case QVideoFrameFormat::Format_ARGB8888:
    case QVideoFrameFormat::Format_ARGB8888_Premultiplied:
        return CapturePixelFormat::Argb;
    case QVideoFrameFormat::Format_XRGB8888:
        return CapturePixelFormat::Xrgb;
    case QVideoFrameFormat::Format_BGRA8888:
    case QVideoFrameFormat::Format_BGRA8888_Premultiplied:
        return CapturePixelFormat::Bgra;
    case QVideoFrameFormat::Format_BGRX8888:
        return CapturePixelFormat::Bgrx;
    case QVideoFrameFormat::Format_ABGR8888:
        return CapturePixelFormat::Abgr;
    case QVideoFrameFormat::Format_XBGR8888:
        return CapturePixelFormat::Xbgr;
    case QVideoFrameFormat::Format_RGBA8888:
        return CapturePixelFormat::Rgba;
    case QVideoFrameFormat::Format_RGBX8888:
        return CapturePixelFormat::Rgbx;
    case QVideoFrameFormat::Format_YUV420P:
        return CapturePixelFormat::Yuv420p;
    case QVideoFrameFormat::Format_YV12:
        return CapturePixelFormat::Yv12;
    case QVideoFrameFormat::Format_YUV422P:
        return CapturePixelFormat::Yuv422p;
    case QVideoFrameFormat::Format_NV12:
        return CapturePixelFormat::Nv12;
    case QVideoFrameFormat::Format_NV21:
        return CapturePixelFormat::Nv21;
    case QVideoFrameFormat::Format_YUYV:
        return CapturePixelFormat::Yuyv;
    case QVideoFrameFormat::Format_UYVY:
        return CapturePixelFormat::Uyvy;
    case QVideoFrameFormat::Format_Y8:
        return CapturePixelFormat::Gray8;
    default:
        return std::nullopt;
    }
}

// A mapped camera frame. Compressed (Motion JPEG) camera frames are decoded into an image first.
class MappedFrame
{
  public:
    explicit MappedFrame(QVideoFrame source) : m_Frame(std::move(source))
    {
        if (!m_Frame.isValid() || !m_Frame.map(QVideoFrame::ReadOnly))
            throw frameError(QT_TRANSLATE_NOOP("motrack::CapturePipeline", "Could not read a camera frame."));
        const auto format = m_Frame.surfaceFormat();
        if (format.pixelFormat() == QVideoFrameFormat::Format_Jpeg)
        {
            m_Decoded = QImage::fromData(m_Frame.bits(0), m_Frame.mappedBytes(0));
            if (m_Decoded.isNull())
                throw frameError(QT_TRANSLATE_NOOP("motrack::CapturePipeline", "Could not decode a Motion JPEG camera frame."));
            m_Image = captureImageFromQImage(m_Decoded);
            if (m_Image.width == 0)
            {
                m_Decoded = m_Decoded.convertToFormat(QImage::Format_RGB32);
                m_Image = captureImageFromQImage(m_Decoded);
            }
            return;
        }
        const auto pixels = capturePixelFormat(format.pixelFormat());
        if (!pixels)
            throw VideoError(QVideoFrameFormat::pixelFormatToString(format.pixelFormat()).toStdString());
        m_Image = {.width = m_Frame.width(),
                   .height = m_Frame.height(),
                   .format = *pixels,
                   .fullRange = format.colorRange() == QVideoFrameFormat::ColorRange_Full,
                   .bt709 = format.colorSpace() == QVideoFrameFormat::ColorSpace_BT709};
        for (int plane = 0; plane < std::min(m_Frame.planeCount(), 3); ++plane)
        {
            m_Image.planes.at(static_cast<std::size_t>(plane)) = m_Frame.bits(plane);
            m_Image.strides.at(static_cast<std::size_t>(plane)) = m_Frame.bytesPerLine(plane);
        }
    }
    ~MappedFrame()
    {
        if (m_Frame.isMapped())
            m_Frame.unmap();
    }
    MappedFrame(const MappedFrame&) = delete;
    MappedFrame& operator=(const MappedFrame&) = delete;

    [[nodiscard]] const CaptureImage& image() const
    {
        return m_Image;
    }

  private:
    QVideoFrame m_Frame;
    QImage m_Decoded;
    CaptureImage m_Image;
};

// The MSVC library describes system errors in the ANSI code page; the portable condition's text is ASCII.
QString errorText(const std::error_code& error)
{
    const auto condition = error.default_error_condition();
    if (condition.category() == std::generic_category())
        return QString::fromStdString(condition.message());
    return QCoreApplication::translate("motrack::CapturePipeline", "system error %1").arg(error.value());
}

QString copyFile(const QString& source, const QString& target, const std::atomic_bool& cancelled)
{
    QFile input(source);
    QFile output(target);
    const auto native = QDir::toNativeSeparators(target);
    if (!input.open(QIODevice::ReadOnly))
        return QCoreApplication::translate("motrack::CapturePipeline", "Could not read the recording.");
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return QCoreApplication::translate("motrack::CapturePipeline", "Could not create %1.").arg(native);
    while (!input.atEnd())
    {
        if (cancelled)
        {
            output.remove();
            return QCoreApplication::translate("motrack::CapturePipeline", "Saving the recording was canceled.");
        }
        const auto chunk = input.read(COPY_CHUNK);
        if (chunk.isEmpty() || output.write(chunk) != chunk.size())
        {
            output.remove();
            return QCoreApplication::translate("motrack::CapturePipeline", "Could not copy the recording to %1.").arg(native);
        }
    }
    if (!output.flush() || output.size() != input.size())
    {
        output.remove();
        return QCoreApplication::translate("motrack::CapturePipeline", "Could not copy the recording to %1.").arg(native);
    }
    return {};
}

} // namespace

CaptureIntervals captureIntervals(const std::vector<microseconds>& times)
{
    if (times.size() < 2)
        return {};
    std::vector<microseconds> intervals;
    intervals.reserve(times.size() - 1);
    for (std::size_t frame = 1; frame < times.size(); ++frame)
        intervals.push_back(times[frame] - times[frame - 1]);
    auto sorted = intervals;
    std::ranges::sort(sorted);
    const auto middle = sorted.size() / 2;
    const auto median = sorted.size() % 2 == 1 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
    // A gap is an interval longer than 1.5 times the median: 3 * interval > 2 * median, in integers.
    const auto gaps =
        std::ranges::count_if(intervals, [median](microseconds interval) { return 2 * interval.count() > 3 * median.count(); });
    return {.gaps = static_cast<qint64>(gaps), .median = median, .longest = sorted.back()};
}

struct CapturePipeline::Shared
{
    struct Frame
    {
        QVideoFrame frame;
        microseconds arrival;
    };
    enum class Command : std::uint8_t
    {
        Start,
        Stop,
        Cancel,
    };
    using Item = std::variant<Frame, Command>;

    // Owner thread and frame sources, under mutex.
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Item> queue;
    std::size_t queuedFrames = 0;
    QString path;
    FrameTimeField timeField = FrameTimeField::StartTime;
    QSize previewBound{960, 540};
    qint64 dropped = 0;
    bool recording = false;
    bool accepting = true;
    bool quit = false;
    std::atomic_bool previewPending{false};

    void removeTrailingFrames()
    {
        while (!queue.empty() && std::holds_alternative<Frame>(queue.back()))
        {
            queue.pop_back();
            --queuedFrames;
        }
    }
};

namespace
{

// The worker's recording; only the worker thread touches it.
struct Recording
{
    std::unique_ptr<CaptureEncoder> encoder;
    QString path;
    FrameTimeField timeField = FrameTimeField::StartTime;
    std::optional<CaptureClock> clock;
    std::optional<QSize> size;
    std::vector<microseconds> times;
    qint64 skipped = 0;
};

} // namespace

CapturePipeline::CapturePipeline() : m_Shared(std::make_shared<Shared>())
{
    // The worker emits the private signals; queued connections deliver them on this owner's thread. The
    // destructor joins the worker before the QObject part ends, which also discards undelivered ones.
    connect(this, &CapturePipeline::previewQueued, this, &CapturePipeline::previewReady, Qt::QueuedConnection);
    connect(this, &CapturePipeline::progressQueued, this, &CapturePipeline::progress, Qt::QueuedConnection);
    connect(this, &CapturePipeline::rejectedQueued, this, &CapturePipeline::frameRejected, Qt::QueuedConnection);
    connect(
        this,
        &CapturePipeline::finishedQueued,
        this,
        [this](const CaptureResult& result)
        {
            m_Recording = false;
            Q_EMIT recordingFinished(result);
        },
        Qt::QueuedConnection);
    connect(
        this,
        &CapturePipeline::failedQueued,
        this,
        [this](const QString& message)
        {
            m_Recording = false;
            Q_EMIT recordingFailed(message);
        },
        Qt::QueuedConnection);
    connect(
        this,
        &CapturePipeline::cancelledQueued,
        this,
        [this]
        {
            m_Recording = false;
            Q_EMIT recordingCancelled();
        },
        Qt::QueuedConnection);
    // The worker borrows the shared state and this owner; the destructor joins it before either ends.
    m_Worker = std::thread(&CapturePipeline::run, this, std::ref(*m_Shared));
}

void CapturePipeline::run(CapturePipeline* owner, Shared& shared)
{
    CaptureConverter converter;
    std::optional<Recording> recording;
    QString rejected;
    const auto endRecording = [&]
    {
        const std::scoped_lock lock(shared.mutex);
        shared.recording = false;
    };
    const auto fail = [&](const QString& message)
    {
        recording.reset(); // Closes the file before the owner hears of the failure.
        endRecording();
        Q_EMIT owner->failedQueued(message);
    };
    const auto finish = [&](qint64 queueDropped, bool sizeChanged)
    {
        if (!recording->encoder)
        {
            fail(QCoreApplication::translate("motrack::CapturePipeline", "No frames were recorded."));
            return;
        }
        try
        {
            const auto summary = recording->encoder->finish();
            if (summary.frames == 0)
            {
                fail(QCoreApplication::translate("motrack::CapturePipeline", "No frames were recorded."));
                return;
            }
            const auto intervals = captureIntervals(recording->times);
            const CaptureResult result{.path = recording->path,
                                       .frames = summary.frames,
                                       .dropped = queueDropped + recording->skipped,
                                       .duration = summary.duration,
                                       .clock = recording->clock.value_or(CaptureClock::Camera),
                                       .gaps = intervals.gaps,
                                       .medianInterval = intervals.median,
                                       .longestInterval = intervals.longest,
                                       .sizeChanged = sizeChanged};
            recording.reset();
            endRecording();
            Q_EMIT owner->finishedQueued(result);
        }
        catch (const std::exception& error)
        {
            fail(QString::fromUtf8(error.what()));
        }
    };
    const auto reject = [&](const QString& message)
    {
        if (recording)
            ++recording->skipped;
        if (message != rejected)
        {
            rejected = message;
            Q_EMIT owner->rejectedQueued(message);
        }
    };
    for (;;)
    {
        Shared::Item item;
        QSize bound;
        qint64 queueDropped = 0;
        {
            std::unique_lock lock(shared.mutex);
            shared.wake.wait(lock, [&] { return shared.quit || !shared.queue.empty(); });
            if (shared.quit)
                return; // Destruction abandons a recording; its encoder closes the file.
            item = std::move(shared.queue.front());
            shared.queue.pop_front();
            if (std::holds_alternative<Shared::Frame>(item))
                --shared.queuedFrames;
            else if (std::get<Shared::Command>(item) == Shared::Command::Start)
            {
                recording.emplace();
                recording->path = shared.path;
                recording->timeField = shared.timeField;
            }
            bound = shared.previewBound;
            queueDropped = shared.dropped;
        }
        if (const auto* command = std::get_if<Shared::Command>(&item))
        {
            if (*command == Shared::Command::Start && recording)
            {
                try
                {
                    recording->encoder = std::make_unique<CaptureEncoder>(recording->path);
                }
                catch (const std::exception& error)
                {
                    fail(QString::fromUtf8(error.what()));
                }
            }
            else if (recording && *command == Shared::Command::Cancel)
            {
                recording.reset();
                Q_EMIT owner->cancelledQueued();
            }
            else if (recording && *command == Shared::Command::Stop)
                finish(queueDropped, false);
            continue;
        }
        const auto& frame = std::get<Shared::Frame>(item);
        std::optional<MappedFrame> mapped;
        try
        {
            mapped.emplace(frame.frame);
            if (!shared.previewPending.exchange(true))
            {
                Q_EMIT owner->previewQueued(converter.toImage(mapped->image(), bound));
            }
        }
        catch (const VideoError& error)
        {
            shared.previewPending = false;
            reject(QString::fromUtf8(error.what()));
            continue;
        }
        rejected.clear();
        if (!recording || !recording->encoder)
            continue;
        // A new frame size would distort the recorded geometry and any calibration: end the clip there.
        const QSize size(mapped->image().width, mapped->image().height);
        if (recording->size && *recording->size != size)
        {
            finish(queueDropped, true);
            continue;
        }
        const auto end = frame.frame.endTime();
        const auto start = recording->timeField == FrameTimeField::EndTime && end >= 0 ? end : frame.frame.startTime();
        if (!recording->clock)
            recording->clock = start >= 0 ? CaptureClock::Camera : CaptureClock::Arrival;
        const bool camera = *recording->clock == CaptureClock::Camera;
        const auto time = camera ? microseconds{start} : frame.arrival;
        try
        {
            // A frame without the recording's clock, or one that repeats a time, cannot be placed; skip it.
            if ((camera && start < 0) || !recording->encoder->write(mapped->image(), time))
                ++recording->skipped;
            else
            {
                recording->size = size;
                recording->times.push_back(time);
            }
        }
        catch (const std::exception& error)
        {
            fail(QString::fromUtf8(error.what())); // A full disk, for example.
            continue;
        }
        const auto frames = static_cast<qint64>(recording->times.size());
        const auto dropped = queueDropped + recording->skipped;
        const auto elapsed = frames > 0 ? (recording->times.back() - recording->times.front()).count() : 0;
        Q_EMIT owner->progressQueued(frames, dropped, elapsed);
    }
}

CapturePipeline::~CapturePipeline()
{
    {
        const std::scoped_lock lock(m_Shared->mutex);
        m_Shared->accepting = false;
        m_Shared->quit = true;
        m_Shared->queue.clear();
        m_Shared->queuedFrames = 0;
    }
    m_Shared->wake.notify_all();
    m_Worker.join();
}

CapturePipeline::FrameSink CapturePipeline::frameSink() const
{
    return [shared = m_Shared](const QVideoFrame& frame)
    {
        const auto arrival = duration_cast<microseconds>(steady_clock::now().time_since_epoch());
        {
            const std::scoped_lock lock(shared->mutex);
            if (!shared->accepting)
                return;
            if (!shared->recording)
                shared->removeTrailingFrames(); // Preview needs only the newest frame.
            else if (shared->queuedFrames >= RECORDING_QUEUE)
            {
                ++shared->dropped;
                return;
            }
            shared->queue.emplace_back(Shared::Frame{.frame = frame, .arrival = arrival});
            ++shared->queuedFrames;
        }
        shared->wake.notify_one();
    };
}

void CapturePipeline::setPreviewBound(QSize bound)
{
    const std::scoped_lock lock(m_Shared->mutex);
    m_Shared->previewBound = bound;
}

void CapturePipeline::acknowledgePreview()
{
    m_Shared->previewPending = false;
}

bool CapturePipeline::startRecording(const QString& path, FrameTimeField timeField)
{
    if (m_Recording)
        return false;
    {
        const std::scoped_lock lock(m_Shared->mutex);
        m_Shared->removeTrailingFrames(); // Only frames that arrive after the request are recorded.
        m_Shared->queue.emplace_back(Shared::Command::Start);
        m_Shared->path = path;
        m_Shared->timeField = timeField;
        m_Shared->dropped = 0;
        m_Shared->recording = true;
    }
    m_Shared->wake.notify_one();
    m_Recording = true;
    return true;
}

void CapturePipeline::stopRecording()
{
    if (!m_Recording)
        return;
    {
        const std::scoped_lock lock(m_Shared->mutex);
        m_Shared->queue.emplace_back(Shared::Command::Stop);
        m_Shared->recording = false;
    }
    m_Shared->wake.notify_one();
}

void CapturePipeline::cancelRecording()
{
    if (!m_Recording)
        return;
    {
        const std::scoped_lock lock(m_Shared->mutex);
        std::erase_if(m_Shared->queue, [](const Shared::Item& item) { return std::holds_alternative<Shared::Frame>(item); });
        m_Shared->queuedFrames = 0;
        m_Shared->queue.emplace_back(Shared::Command::Cancel);
        m_Shared->recording = false;
    }
    m_Shared->wake.notify_one();
}

bool CapturePipeline::isRecording() const
{
    return m_Recording;
}

namespace
{

// Moves from to to within one volume, in one step. Without replace, an existing to is never replaced:
// a hard link fails if to exists, and file systems without hard links fall back to a checked rename.
std::error_code placeFile(const QString& from, const QString& to, bool replace)
{
    std::error_code error;
    if (replace)
    {
        std::filesystem::rename(toFilesystemPath(from), toFilesystemPath(to), error);
        return error;
    }
    std::filesystem::create_hard_link(toFilesystemPath(from), toFilesystemPath(to), error);
    if (!error)
    {
        std::filesystem::remove(toFilesystemPath(from), error);
        return {};
    }
    if (error == std::errc::file_exists || QFileInfo::exists(to))
        return std::make_error_code(std::errc::file_exists);
    if (error == std::errc::cross_device_link)
        return error;
    std::filesystem::rename(toFilesystemPath(from), toFilesystemPath(to), error);
    return error;
}

} // namespace

CaptureMove moveCaptureFile(const QString& source, const QString& target, const std::atomic_bool& cancelled, bool replace, bool allowRename)
{
    const QFileInfo input(source);
    const QFileInfo output(target);
    const auto failed = [](QString message)
    {
        return CaptureMove{.status = CaptureMoveStatus::Failed, .error = std::move(message)};
    };
    if (!input.isFile())
        return failed(QCoreApplication::translate("motrack::CapturePipeline", "The recording is no longer available."));
    if (!output.absoluteDir().exists())
        return failed(QCoreApplication::translate("motrack::CapturePipeline", "The folder %1 does not exist.")
                          .arg(QDir::toNativeSeparators(output.absolutePath())));
    if (!replace && (output.exists() || output.isSymLink()))
        return {.status = CaptureMoveStatus::TargetExists, .error = {}};
    // The same volume: one rename or link puts the complete file in place.
    if (allowRename)
    {
        const auto error = placeFile(source, target, replace);
        if (!error)
            return {.status = CaptureMoveStatus::Moved, .error = {}};
        if (error == std::errc::file_exists)
            return {.status = CaptureMoveStatus::TargetExists, .error = {}};
    }
    // Another volume: copy to a hidden part file beside the target, then move that into place, so the
    // final name never shows half a file.
    const auto staged = output.absoluteDir().filePath(
        QStringLiteral(".%1.%2.part").arg(output.fileName(), QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (auto message = copyFile(source, staged, cancelled); !message.isEmpty())
        return failed(message);
    const auto error = placeFile(staged, target, replace);
    if (error)
    {
        QFile::remove(staged);
        if (error == std::errc::file_exists)
            return {.status = CaptureMoveStatus::TargetExists, .error = {}};
        return failed(QCoreApplication::translate("motrack::CapturePipeline", "Could not save %1: %2")
                          .arg(QDir::toNativeSeparators(target), errorText(error)));
    }
    QFile::remove(source);
    return {.status = CaptureMoveStatus::Moved, .error = {}};
}

QStringList defaultCaptureFolders()
{
    QStringList folders;
    for (const auto location : {QStandardPaths::MoviesLocation, QStandardPaths::DocumentsLocation})
        if (const auto base = QStandardPaths::writableLocation(location); !base.isEmpty())
            folders << QDir(base).filePath(QStringLiteral("MoTrack"));
    return folders;
}

QString prepareCaptureFolder(const QStringList& folders)
{
    for (const auto& folder : folders)
        if (!folder.isEmpty() && QDir().mkpath(folder) && QFileInfo(folder).isDir())
            return QDir(folder).absolutePath();
    return {};
}

QString captureFileName(const QDateTime& time, int candidate)
{
    const auto stamp = time.toString(QStringLiteral("yyyy-MM-dd HH-mm-ss"));
    return candidate <= 1 ? QStringLiteral("Capture %1.mov").arg(stamp) : QStringLiteral("Capture %1 (%2).mov").arg(stamp).arg(candidate);
}

CaptureSave saveCaptureAutomatically(const QString& source, const QString& folder, const QDateTime& time, const std::atomic_bool& cancelled)
{
    constexpr int MAX_CANDIDATES = 1000;
    const QDir directory(folder);
    for (int candidate = 1; candidate <= MAX_CANDIDATES; ++candidate)
    {
        const auto target = directory.filePath(captureFileName(time, candidate));
        const auto move = moveCaptureFile(source, target, cancelled, false);
        if (move.status == CaptureMoveStatus::Moved)
            return {.path = target, .error = {}};
        if (move.status == CaptureMoveStatus::Failed)
            return {.path = {}, .error = move.error};
    }
    return {.path = {},
            .error = QCoreApplication::translate("motrack::CapturePipeline", "Could not find a free file name in %1.")
                         .arg(QDir::toNativeSeparators(folder))};
}

QString captureLockName()
{
    return QStringLiteral("capture.lock");
}

int sweepCaptureFolders(const QDir& root)
{
    int removed = 0;
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto& name : root.entryList({QStringLiteral("motrack-capture-*")}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
    {
        QDir folder(root.filePath(name));
        const auto lockPath = folder.filePath(captureLockName());
        if (QFileInfo::exists(lockPath))
        {
            // Only the owner's death makes its lock stale; a running MoTrack keeps its folder.
            QLockFile lock(lockPath);
            lock.setStaleLockTime(0);
            if (!lock.tryLock(0))
                continue;
            lock.unlock();
        }
        else if (QFileInfo(folder.path()).lastModified().toUTC().secsTo(now) < UNLOCKED_FOLDER_AGE_SECONDS)
            continue;
        if (folder.removeRecursively())
            ++removed;
    }
    return removed;
}

} // namespace motrack
