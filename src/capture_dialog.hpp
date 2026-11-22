#pragma once

#include "camera_source.hpp"
#include "capture_pipeline.hpp"

#include <QDialog>
#include <QElapsedTimer>
#include <QFuture>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

class QComboBox;
class QLabel;
class QLockFile;
class QPushButton;
class QTemporaryDir;

namespace motrack
{

class CapturePreview;

/// Camera capture window: choose a camera and format, preview, record, then save the clip and hand it
/// to the workspace, which opens it as a new experiment through the normal movie path. No network
/// listener is involved. The clip is recorded into a private temporary folder; discarding it, failing,
/// cancelling, or closing or destroying the window removes that folder after the movie file is closed.
/// Closing asks first while a recording exists that has not been saved. A recording is only offered to
/// the workspace once it is saved somewhere permanent, so a project can never refer to a temporary file.
/// Lives on the GUI thread; recording and saving run on workers.
class CaptureDialog final : public QDialog
{
    Q_OBJECT

  public:
    enum class State : std::uint8_t
    {
        Unavailable,
        WaitingForAccess,
        Starting,
        Previewing,
        Recording,
        Finishing,
        Recorded,
        Saving,
    };

    explicit CaptureDialog(std::unique_ptr<CameraSource> source, QWidget* parent = nullptr);
    ~CaptureDialog() override;
    CaptureDialog(const CaptureDialog&) = delete;
    CaptureDialog& operator=(const CaptureDialog&) = delete;

    [[nodiscard]] State state() const;
    /// The current recording's temporary file, or empty when there is none.
    [[nodiscard]] QString temporaryRecording() const;
    /// The finished recording waiting to be saved, if any.
    [[nodiscard]] const std::optional<CaptureResult>& recording() const;
    [[nodiscard]] QImage previewImage() const;
    /// Saves the finished recording at path (as the Save dialog does); recordingSaved follows on success.
    void saveRecording(const QString& path);
    /// True while recording, finishing, holding a finished clip or saving it: closing would discard it.
    [[nodiscard]] bool hasUnsavedRecording() const;
    /// Asks before an unsaved recording is discarded; true when the window may close. Runs a modal
    /// question, so the caller must expect this window (or its own owner) to be gone afterwards.
    bool confirmDiscard();
    /// Stops and discards any recording without asking, then closes.
    void discardAndClose();
    /// Asks (confirmDiscard), then discards any recording and closes: Esc, the title-bar close and Cancel.
    void reject() override;
    /// How long a recording may receive no frames before the window says so (5 s by default).
    void setStallWarningAfter(std::chrono::milliseconds limit);

  Q_SIGNALS:
    /// The recording is now at path; the window closes next. Frame times come from the file itself.
    void recordingSaved(const QString& path, const motrack::CaptureResult& result);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void refreshDevices();
    void refreshFormats();
    void checkAccess();
    void startCamera();
    void toggleRecording();
    void discardRecording();
    void chooseDestination();
    void finishSaving(const QString& path, const QString& error);
    void showProgress(qint64 frames, qint64 dropped, qint64 duration);
    void releaseTemporary();
    /// The camera failed or disappeared: a recording ends with the frames so far, otherwise capture stops.
    void cameraLost(const QString& message);
    void setState(State state, const QString& message = {});
    [[nodiscard]] QString summary(const CaptureResult& result) const;

    // Destroyed in reverse order: the camera stops delivering, the pipeline closes the movie, then the
    // temporary folder is removed.
    std::unique_ptr<QTemporaryDir> m_Temporary;
    std::unique_ptr<QLockFile> m_TemporaryLock;
    std::unique_ptr<CapturePipeline> m_Pipeline;
    std::unique_ptr<CameraSource> m_Source;
    std::optional<CaptureResult> m_Result;
    QFuture<QString> m_Saving;
    std::shared_ptr<std::atomic_bool> m_SaveCancelled;
    QList<CameraDevice> m_Devices;
    QComboBox* m_Camera = nullptr;
    QComboBox* m_Format = nullptr;
    CapturePreview* m_Preview = nullptr;
    QLabel* m_Status = nullptr;
    QLabel* m_Elapsed = nullptr;
    QPushButton* m_Record = nullptr;
    QPushButton* m_Save = nullptr;
    QPushButton* m_Discard = nullptr;
    QPushButton* m_Close = nullptr;
    QByteArray m_RecordingDevice;
    QTimer m_Stall;
    QElapsedTimer m_LastFrame;
    std::chrono::milliseconds m_StallLimit{5000};
    State m_State = State::Unavailable;
    bool m_CameraActive = false;
    bool m_Closed = false;
    bool m_StallReported = false;
};

} // namespace motrack
