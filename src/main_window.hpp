#pragma once

#include "document_session.hpp"
#include "image_sequence.hpp"
#include "legacy_project.hpp"
#include "template_tracker.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"
#include "video_reader.hpp"
#include "workspace_context.hpp"
#include "workspace_services.hpp"

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QMainWindow>
#include <QPointF>
#include <QTimer>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

class QLabel;
class QAction;
class QCloseEvent;
class QCheckBox;
class QComboBox;
class QSlider;
class QTableWidget;
class QToolButton;
class QThreadPool;

namespace motrack
{

class VideoCanvas;
class PositionPlot;

class MainWindow final : public QMainWindow
{
    Q_DECLARE_TR_FUNCTIONS(MainWindow)

  public:
    explicit MainWindow(std::shared_ptr<WorkspaceServices> services = {});
    /// Bind a prepared retained record. Opening/replacing sources belongs to the workspace shell.
    MainWindow(std::shared_ptr<WorkspaceContext> workspace, QUuid document);
    ~MainWindow() override;
    bool loadProject(const QString& path);
    void openProject(const QString& path);
    void openMovie(const QString& path);
    bool openImageSequence(const QStringList& paths, double frameRate);
    bool saveMeasurements(const QString& path);
    bool saveWorkspace(const QString& path);
    bool saveTrackerProject(const QString& path, bool recoverPending = false);
    bool exportTrackerArchive(const QString& path);
    /// Shared services outlive every borrowing view; default construction creates one workspace owner.
    [[nodiscard]] WorkspaceServices& executionServices();

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    [[nodiscard]] const WorkspaceDocument& viewDocument() const;
    [[nodiscard]] LegacyProject& viewProject();
    [[nodiscard]] const LegacyProject& viewProject() const;
    [[nodiscard]] TrackEdits& viewHistory();
    [[nodiscard]] bool viewModified() const;
    [[nodiscard]] bool viewRequiresPairedSave() const;
    [[nodiscard]] QString viewSourcePath() const;
    [[nodiscard]] QString viewNativeSavePath() const;
    [[nodiscard]] QString viewStandardSavePath() const;
    [[nodiscard]] QString viewRecoveryPath() const;
    [[nodiscard]] DocumentSession::SaveCheckpoint viewCheckpoint() const;
    bool saveBoundWorkspace(const QString& path, bool paired, bool recoverPending = false);
    bool recoverBoundWorkspace();
    struct PreparedProject;
    [[nodiscard]] static PreparedProject prepareProject(const QString& path, const std::atomic_bool* cancelled);
    bool applyProject(const QString& path, PreparedProject prepared, const std::atomic_bool* cancelled);
    void cancelProjectOpen();
    /// Defer owned open results during a save. Each completion must recheck its cancellation flag.
    void completeAfterSave(std::function<void()> completion);
    void displayStep(int step);
    void displayMarks();
    void displayFrame(const ImageFrame& frame, const QString& failureMessage, bool advanceTracking = true);
    void requestMovieFrame(int frame);
    void scheduleMovieIndex(const QString& path, bool standalone);
    void setPlaying(bool playing);
    void schedulePlayback();
    void startAutoTracking();
    void stopAutoTracking(const QString& message = {});
    void updateAutoTrackerAction();
    void autoTrackFrame(const ImageFrame& frame);
    void applyAutoTrackMatch(int frame, const std::optional<QPoint>& point);
    void activateProject(const QString& path);
    void refreshImageMediaLocations();
    void syncClipControls();
    void editClipSettings();
    void undoOrRedo(bool redo);
    void cancelMovieOpen();
    void releaseArchive();
    bool chooseWorkspacePath();
    bool chooseTrackerPath();
    bool chooseArchivePath();
    bool saveTrackerWithRecoveryPrompt(const QString& path);
    void finishSaving();
    bool saveCurrentWorkspace();
    bool saveCsv(const QString& path, const std::string& csv);
    void refreshEditViews(int track);
    void refreshPlot();
    void markAt(const QPointF& position);
    [[nodiscard]] std::optional<std::size_t> selectedVector() const;
    void cancelVectorMark();
    void markVectorAt(const QPointF& position);
    void renameSelectedPointMass();
    void setSelectedPointMassValue();
    void removeSelectedPointMass();
    void removeCurrentMark();
    void updatePointMassActions();
    void repeatPreviousMark();
    void pickCalibrationPoint(const QPointF& position);
    void pickAxesPoint(const QPointF& position);
    void refreshAxesOverlay();
    void beginMeasurement(bool angle);
    void cancelMeasurement();
    void pickMeasurementPoint(const QPointF& point);
    void clearCalibrationPoint();
    [[nodiscard]] bool confirmDiscard();

    WorkspaceServices* m_Services; // Borrowed from a parent-owned lease, released after QObject::destroyed.
    std::array<QAction*, 3> m_OpenActions{};
    VideoCanvas* m_Video;
    QLabel* m_FrameLabel;
    QSlider* m_StepSlider;
    QToolButton* m_Previous;
    QToolButton* m_Next;
    QToolButton* m_Play;
    QTimer m_PlayTimer;
    bool m_Playing = false;
    QTableWidget* m_Marks;
    QAction* m_Export;
    QAction* m_ExportPlot;
    QAction* m_SaveAs;
    QAction* m_Save;
    QAction* m_SaveTracker;
    QAction* m_ExportArchive;
    QAction* m_NewPointMass;
    QAction* m_NewVector;
    QAction* m_RenamePointMass;
    QAction* m_SetPointMass;
    QAction* m_RemovePointMass;
    QAction* m_RemoveMark;
    QAction* m_Undo;
    QAction* m_Redo;
    QAction* m_Calibrate;
    QAction* m_ClipSettings;
    QAction* m_AutoTracker;
    QAction* m_ShowAxes;
    QAction* m_EditAxes;
    QAction* m_MeasureDistance;
    QAction* m_MeasureAngle;
    QComboBox* m_TrackPicker;
    QComboBox* m_PlotQuantity;
    QCheckBox* m_FitLine;
    QLabel* m_FitSummary;
    PositionPlot* m_Plot;
    DocumentSession m_Session;
    std::shared_ptr<WorkspaceContext> m_Workspace;
    QUuid m_WorkspaceDocument;
    std::optional<WorkspaceJobToken> m_IndexJob;
    std::optional<WorkspaceJobToken> m_TrackingJob;
    std::optional<ImageSequence> m_Sequence;
    std::optional<ImageFrame> m_DisplayedFrame;
    std::optional<QPointF> m_CalibrationFirst;
    std::optional<QPointF> m_VectorTail;
    std::optional<QPointF> m_AxesOrigin;
    QList<QPointF> m_MeasurementPoints;
    struct AutoTrackSession
    {
        TemplateTracker pattern;
        std::size_t track;
        int lastFrame;
        QPoint predicted;
    };
    std::optional<AutoTrackSession> m_AutoTrack;
    QTimer m_AutoTrackTimer;
    QFutureWatcher<std::optional<QPoint>>* m_AutoTrackMatch = nullptr;
    QString m_MoviePath;
    QString m_MediaProjectPath;
    QThreadPool* m_SavePool;
    bool m_Saving = false;
    std::shared_ptr<std::atomic_bool> m_SaveCancelled;
    std::vector<std::function<void()>> m_AfterSave;
    std::vector<std::chrono::nanoseconds> m_MoviePresentationTimes;
    std::shared_ptr<std::atomic_bool> m_MovieOpenCancelled;
    std::unique_ptr<QFutureWatcher<MovieIndex>> m_MovieOpenWatcher;
    std::shared_ptr<std::atomic_bool> m_ProjectOpenCancelled;
    std::unique_ptr<QFutureWatcher<std::shared_ptr<PreparedProject>>> m_ProjectOpenWatcher;
    QUuid m_FrameDocument = QUuid::createUuid();
    QUuid m_MediaGeneration = QUuid::createUuid();
    std::atomic<quint64> m_RequestGeneration = 0;
};

} // namespace motrack
