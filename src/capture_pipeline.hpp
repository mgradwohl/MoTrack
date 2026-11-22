#pragma once

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

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

/// A finished recording on disk. Frame times are relative to the first frame; dropped frames reached
/// MoTrack but were not written because the encoder fell behind or the camera repeated a timestamp.
struct CaptureResult
{
    QString path;
    qint64 frames = 0;
    qint64 dropped = 0;
    std::chrono::microseconds duration{0};
    CaptureClock clock = CaptureClock::Camera;
};

/// Converts camera frames for preview and records them on one worker thread, so neither costs the GUI
/// thread anything. Frames enter through frameSink(), from any thread. While previewing, only the newest
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
    /// Records frames that arrive from now on into a new movie at path. False while a recording is active.
    bool startRecording(const QString& path);
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

/// Moves a finished recording to target, replacing an existing target only once the copy is complete.
/// Tries a rename first unless allowRename is false, then copies in chunks, checking cancelled between
/// them. On failure or cancellation the source stays and no partial target remains. Empty on success,
/// otherwise a description of the failure. Runs on any thread.
[[nodiscard]] QString
moveCaptureFile(const QString& source, const QString& target, const std::atomic_bool& cancelled, bool allowRename = true);

} // namespace motrack
