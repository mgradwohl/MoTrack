#pragma once

#include "camera_source.hpp"
#include "capture_pipeline.hpp"

#include <QDateTime>
#include <QDialog>
#include <QElapsedTimer>
#include <QFuture>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLockFile;
class QPushButton;
class QTemporaryDir;

namespace motrack
{

class CapturePreview;

/// Camera capture window: choose a camera and format, preview and record. When a recording stops, it is
/// saved automatically under a collision-free "Capture <date> <time>.mov" in the captures folder
/// (Movies/MoTrack, else Documents/MoTrack) and handed to the workspace, which opens it as a new
/// experiment through the normal movie path; the window stays open for the next take. "Ask where to save
/// each recording", or a captures folder that cannot be created, asks for a location instead. No network
/// listener is involved. The clip is recorded into a private temporary folder, and an experiment only ever
/// refers to the saved file. Closing asks first while a recording exists that is not saved yet (while
/// recording or finishing, or a finished clip waiting for its location); discarding, failing or
/// destroying the window removes the temporary folder after the movie file is closed. Lives on the GUI
/// thread; recording and saving run on workers.
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
        /// A finished recording waits for the user to choose where to save it.
        Recorded,
        Saving,
    };

    explicit CaptureDialog(std::unique_ptr<CameraSource> source, QWidget* parent = nullptr);
    /// Waits for a save in progress (it keeps the user's recording), then stops the camera, closes any
    /// movie and removes the temporary folder.
    ~CaptureDialog() override;
    CaptureDialog(const CaptureDialog&) = delete;
    CaptureDialog& operator=(const CaptureDialog&) = delete;

    [[nodiscard]] State state() const;
    /// The current recording's temporary file, or empty when there is none.
    [[nodiscard]] QString temporaryRecording() const;
    /// The latest finished recording; its path is the saved file once saving succeeded.
    [[nodiscard]] const std::optional<CaptureResult>& recording() const;
    /// The file the latest recording was saved as, or empty.
    [[nodiscard]] QString lastSavedRecording() const;
    [[nodiscard]] QImage previewImage() const;
    /// The folders automatic saving tries in order (defaultCaptureFolders() unless replaced).
    void setCaptureFolders(QStringList folders);
    /// Saves the finished recording at path, replacing it (as the Save dialog does after confirming);
    /// recordingSaved follows on success.
    void saveRecording(const QString& path);
    /// True while a recording exists that is not saved: recording, finishing, or waiting for a location.
    [[nodiscard]] bool hasUnsavedRecording() const;
    /// Before closing: true when the window may close now (nothing unsaved, or the user chose to discard
    /// it). Asks while a recording is unsaved; choosing to keep a recording in progress stops it, saves
    /// and opens it, then closes the window, and returns false like Cancel. Runs a modal question, so the
    /// caller must expect this window (or its own owner) to be gone afterwards.
    bool confirmClose();
    /// Stops and discards any unsaved recording without asking, then closes. A save in progress finishes.
    void discardAndClose();
    /// Esc, the title-bar close and Close: confirmClose(), then discardAndClose(). While saving, the
    /// window closes once the clip is saved and opened.
    void reject() override;
    /// How long a recording may receive no frames before the window says so (5 s by default).
    void setStallWarningAfter(std::chrono::milliseconds limit);

  Q_SIGNALS:
    /// The recording is now saved at path; the workspace opens it. Frame times come from the file itself.
    void recordingSaved(const QString& path, const motrack::CaptureResult& result);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void refreshDevices();
    void refreshFormats();
    void checkAccess();
    void startCamera();
    void toggleRecording();
    void recordingFinished(const CaptureResult& result);
    void discardRecording();
    void chooseDestination();
    /// Saves into folder under a free automatic name, or at chosen (replacing it) when that is set.
    void startSaving(const QString& folder, const QString& chosen);
    void finishSaving(const CaptureSave& save, const QString& folder);
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
    QString m_Summary;
    QString m_LastSaved;
    QStringList m_Folders = defaultCaptureFolders();
    QDateTime m_StoppedAt;
    QFuture<CaptureSave> m_Saving;
    QList<CameraDevice> m_Devices;
    QComboBox* m_Camera = nullptr;
    QComboBox* m_Format = nullptr;
    QCheckBox* m_AskLocation = nullptr;
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
    bool m_CloseAfterSave = false;
    bool m_StallReported = false;
};

} // namespace motrack
