#include "capture_pipeline.hpp"

#include "capture_encoder.hpp"
#include "video_reader.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QVideoFrame>
#include <QVideoFrameFormat>

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
            throw VideoError("Could not read a camera frame.");
        const auto format = m_Frame.surfaceFormat();
        if (format.pixelFormat() == QVideoFrameFormat::Format_Jpeg)
        {
            m_Decoded = QImage::fromData(m_Frame.bits(0), m_Frame.mappedBytes(0));
            if (m_Decoded.isNull())
                throw VideoError("Could not decode a Motion JPEG camera frame.");
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

std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return {path.toStdWString()};
#else
    return {QFile::encodeName(path).toStdString()};
#endif
}

QString copyFile(const QString& source, const QString& target, const std::atomic_bool& cancelled)
{
    QFile input(source);
    QFile output(target);
    if (!input.open(QIODevice::ReadOnly))
        return QStringLiteral("Could not read the recording.");
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(target));
    while (!input.atEnd())
    {
        if (cancelled)
        {
            output.remove();
            return QStringLiteral("Saving the recording was canceled.");
        }
        const auto chunk = input.read(COPY_CHUNK);
        if (chunk.isEmpty() || output.write(chunk) != chunk.size())
        {
            output.remove();
            return QStringLiteral("Could not copy the recording to %1.").arg(QDir::toNativeSeparators(target));
        }
    }
    if (!output.flush() || output.size() != input.size())
    {
        output.remove();
        return QStringLiteral("Could not copy the recording to %1.").arg(QDir::toNativeSeparators(target));
    }
    return {};
}

} // namespace

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
    std::optional<CaptureClock> clock;
    std::optional<microseconds> first;
    microseconds latest{0};
    qint64 frames = 0;
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
    const auto fail = [&](const QString& message)
    {
        recording.reset(); // Closes the file before the owner hears of the failure.
        {
            const std::scoped_lock lock(shared.mutex);
            shared.recording = false;
        }
        Q_EMIT owner->failedQueued(message);
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
            {
                try
                {
                    const auto summary = recording->encoder->finish();
                    if (summary.frames == 0)
                    {
                        fail(QStringLiteral("No frames were recorded."));
                        continue;
                    }
                    const CaptureResult result{.path = recording->path,
                                               .frames = summary.frames,
                                               .dropped = queueDropped + recording->skipped,
                                               .duration = summary.duration,
                                               .clock = recording->clock.value_or(CaptureClock::Camera)};
                    recording.reset();
                    Q_EMIT owner->finishedQueued(result);
                }
                catch (const std::exception& error)
                {
                    fail(QString::fromUtf8(error.what()));
                }
            }
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
        const auto start = frame.frame.startTime();
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
                if (!recording->first)
                    recording->first = time;
                recording->latest = time;
                ++recording->frames;
            }
        }
        catch (const std::exception& error)
        {
            fail(QString::fromUtf8(error.what())); // A full disk, for example.
            continue;
        }
        const auto frames = recording->frames;
        const auto dropped = queueDropped + recording->skipped;
        const auto elapsed = recording->first ? (recording->latest - *recording->first).count() : 0;
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

bool CapturePipeline::startRecording(const QString& path)
{
    if (m_Recording)
        return false;
    {
        const std::scoped_lock lock(m_Shared->mutex);
        m_Shared->removeTrailingFrames(); // Only frames that arrive after the request are recorded.
        m_Shared->queue.emplace_back(Shared::Command::Start);
        m_Shared->path = path;
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

QString moveCaptureFile(const QString& source, const QString& target, const std::atomic_bool& cancelled, bool allowRename)
{
    const QFileInfo input(source);
    const QFileInfo output(target);
    if (!input.isFile())
        return QStringLiteral("The recording is no longer available.");
    if (!output.absoluteDir().exists())
        return QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(output.absolutePath()));
    std::error_code error;
    // A rename replaces an existing target in one step, on the same volume.
    if (allowRename)
    {
        std::filesystem::rename(nativePath(source), nativePath(target), error);
        if (!error)
            return {};
    }
    // Another volume: copy beside the target, then rename over it so a failure never leaves half a file.
    const auto staged = output.absoluteDir().filePath(
        QStringLiteral(".%1.%2.part").arg(output.fileName(), QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (auto message = copyFile(source, staged, cancelled); !message.isEmpty())
        return message;
    std::filesystem::rename(nativePath(staged), nativePath(target), error);
    if (error)
    {
        QFile::remove(staged);
        return QStringLiteral("Could not save %1: %2").arg(QDir::toNativeSeparators(target), QString::fromStdString(error.message()));
    }
    QFile::remove(source);
    return {};
}

} // namespace motrack
