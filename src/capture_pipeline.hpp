#pragma once

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

class QDateTime;
class QDir;
class QVideoFrame;

namespace motrack
{

/// Which clock stamped a recording's frames: the camera's own presentation times, or (for a camera that
/// supplies none) the time each frame reached MoTrack. Neither is a nominal frame rate.
enum class CaptureClock : std::uint8_t
{
    Camera,
    Arrival,
};

/// Which QVideoFrame field holds a frame's own presentation time. Qt 6.8 to 6.11's macOS (AVFoundation)
/// camera stamps startTime with the previous frame's time and endTime with this frame's; Qt fixed it for
/// 6.12 (qtmultimedia b423e7ee8f). Other backends use startTime.
enum class FrameTimeField : std::uint8_t
{
    StartTime,
    EndTime,
};

/// A finished recording on disk. Frame times are relative to the first frame; dropped frames reached
/// MoTrack but were not written because the encoder fell behind or the camera repeated a timestamp.
/// Frames the camera or its driver never delivered are not counted there; gaps counts frame intervals
/// longer than 1.5 times the median interval, which is where such losses show.
struct CaptureResult
{
    QString path;
    qint64 frames = 0;
    qint64 dropped = 0;
    std::chrono::microseconds duration{0};
    CaptureClock clock = CaptureClock::Camera;
    qint64 gaps = 0;
    std::chrono::microseconds medianInterval{0};
    std::chrono::microseconds longestInterval{0};
    /// The camera changed its frame size, which ended the recording at the last frame of the first size.
    bool sizeChanged = false;
};

/// Frame-interval statistics for the times of written frames, in order: the median interval, the
/// longest one, and how many exceed 1.5 times the median. All zero with fewer than two frames.
struct CaptureIntervals
{
    qint64 gaps = 0;
    std::chrono::microseconds median{0};
    std::chrono::microseconds longest{0};
};
[[nodiscard]] CaptureIntervals captureIntervals(const std::vector<std::chrono::microseconds>& times);

/// Converts camera frames for preview and records them on one worker thread, so neither costs the GUI
/// thread anything. Frames enter through frameSink(), from any thread (Qt 6.8's backends deliver them
/// on the GUI thread; the sink only queues them). While previewing, only the newest
/// frame waits; while recording, a short queue absorbs bursts and a full queue drops (and counts) the
/// arriving frame. Signals are delivered on the owner's thread. Destruction abandons a recording, closes
/// its file and joins the worker before returning, so the owner may delete the file afterwards.
class CapturePipeline final : public QObject
{
    Q_OBJECT

  public:
    using FrameSink = std::function<void(const QVideoFrame&)>;

    CapturePipeline();
    ~CapturePipeline() override;
    CapturePipeline(const CapturePipeline&) = delete;
    CapturePipeline& operator=(const CapturePipeline&) = delete;

    /// A thread-safe entry for frames. Copies may outlive the pipeline; they then ignore frames.
    [[nodiscard]] FrameSink frameSink() const;
    /// The largest preview image to produce (default 960 × 540).
    void setPreviewBound(QSize bound);
    /// Allows the next preview image; call after showing the previous one.
    void acknowledgePreview();
    /// Records frames that arrive from now on into a new movie at path, timing each by timeField (falling
    /// back to startTime when that field is unset). False while a recording is active.
    bool startRecording(const QString& path, FrameTimeField timeField = FrameTimeField::StartTime);
    /// Writes the frames that arrived before this call, then finishes the movie: recordingFinished or
    /// recordingFailed follows.
    void stopRecording();
    /// Abandons the recording without finishing the movie; recordingCancelled follows once its file is closed.
    void cancelRecording();
    /// True from startRecording() until the recording's final signal.
    [[nodiscard]] bool isRecording() const;

  Q_SIGNALS:
    void previewReady(const QImage& image);
    /// Frames written so far, frames dropped, and the time from the first to the latest written frame (µs).
    void progress(qint64 frames, qint64 dropped, qint64 duration);
    void recordingFinished(const motrack::CaptureResult& result);
    void recordingFailed(const QString& message);
    void recordingCancelled();
    /// The camera delivered a frame the pipeline cannot read; it is reported once per format change.
    void frameRejected(const QString& message);

  private:
    struct Shared;
    static void run(CapturePipeline* owner, Shared& shared);
    // Emitted by the worker; queued connections re-emit them as the public signals on the owner's thread.
    Q_SIGNAL void previewQueued(const QImage& image);
    Q_SIGNAL void progressQueued(qint64 frames, qint64 dropped, qint64 duration);
    Q_SIGNAL void finishedQueued(const motrack::CaptureResult& result);
    Q_SIGNAL void failedQueued(const QString& message);
    Q_SIGNAL void cancelledQueued();
    Q_SIGNAL void rejectedQueued(const QString& message);
    std::shared_ptr<Shared> m_Shared;
    std::thread m_Worker;
    bool m_Recording = false;
};

/// How a capture file move ended.
enum class CaptureMoveStatus : std::uint8_t
{
    Moved,
    /// Without replace, the target already existed; nothing changed.
    TargetExists,
    Failed,
};

struct CaptureMove
{
    CaptureMoveStatus status = CaptureMoveStatus::Failed;
    QString error;
};

/// Moves a finished recording to target atomically: the file appears under its final name complete or
/// not at all. A move within one volume is a single link or rename; across volumes the recording is
/// copied in chunks (checking cancelled between them) to a hidden part file beside the target, which
/// is then moved into place the same way. With replace, an existing target is replaced (the user
/// confirmed it); without, it is never touched and TargetExists is returned. allowRename false forces
/// the copy route. On failure or cancellation the source stays and no part file remains. Any thread.
[[nodiscard]] CaptureMove
moveCaptureFile(const QString& source, const QString& target, const std::atomic_bool& cancelled, bool replace, bool allowRename = true);

/// The folders automatic saving tries, in order: MoTrack in the user's Movies folder, then MoTrack in
/// Documents (for systems without a Movies location).
[[nodiscard]] QStringList defaultCaptureFolders();
/// The first of folders that exists or can be created, or empty when none can.
[[nodiscard]] QString prepareCaptureFolder(const QStringList& folders);
/// "Capture yyyy-MM-dd HH-mm-ss.mov" for time, with " (2)", " (3)" … added for the nth candidate.
[[nodiscard]] QString captureFileName(const QDateTime& time, int candidate = 1);

/// Saves a finished recording in folder under the first free captureFileName for time, without ever
/// replacing an existing file. The saved path, or empty with error set. Any thread.
struct CaptureSave
{
    QString path;
    QString error;
};
[[nodiscard]] CaptureSave
saveCaptureAutomatically(const QString& source, const QString& folder, const QDateTime& time, const std::atomic_bool& cancelled);

/// The lock file each capture folder holds (QLockFile) while its window owns it.
[[nodiscard]] QString captureLockName();
/// Removes motrack-capture-* folders under root that no running MoTrack owns: their lock's process has
/// ended (a crash leaves an unfinished movie behind), or they have no lock and are a day old. Returns
/// how many were removed.
int sweepCaptureFolders(const QDir& root);

} // namespace motrack
