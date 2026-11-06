#pragma once

#include "document_session.hpp"
#include "image_sequence.hpp"
#include "legacy_project.hpp"
#include "template_tracker.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"
#include "video_reader.hpp"

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QMainWindow>
#include <QPointF>
#include <QThread>
#include <QTimer>

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
class MovieWorker;

class MainWindow final : public QMainWindow
{
    Q_DECLARE_TR_FUNCTIONS(MainWindow)

  public:
    MainWindow();
    ~MainWindow() override;
    bool loadProject(const QString& path);
    void openProject(const QString& path);
    void openMovie(const QString& path);
    bool openImageSequence(const QStringList& paths, double frameRate);
    bool saveMeasurements(const QString& path);
    bool saveWorkspace(const QString& path);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    struct PreparedProject;
    [[nodiscard]] static PreparedProject prepareProject(const QString& path, const std::atomic_bool* cancelled);
    bool applyProject(const QString& path, PreparedProject prepared, const std::atomic_bool* cancelled);
    void cancelProjectOpen();
    /// Defer owned open results during a save. Each completion must recheck its cancellation flag.
    void completeAfterSave(std::function<void()> completion);
    void displayStep(int step);
    void displayMarks();
    void displayFrame(const ImageFrame& frame, const QString& failureMessage);
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
    void syncClipControls();
    void editClipSettings();
    void undoOrRedo(bool redo);
    void cancelMovieOpen();
    void releaseArchive();
    bool chooseWorkspacePath();
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
    std::vector<std::function<void()>> m_AfterSave;
    std::vector<std::chrono::nanoseconds> m_MoviePresentationTimes;
    std::shared_ptr<std::atomic_bool> m_MovieOpenCancelled;
    std::unique_ptr<QFutureWatcher<MovieIndex>> m_MovieOpenWatcher;
    std::shared_ptr<std::atomic_bool> m_ProjectOpenCancelled;
    std::unique_ptr<QFutureWatcher<std::shared_ptr<PreparedProject>>> m_ProjectOpenWatcher;
    QThread m_MovieThread;
    MovieWorker* m_MovieWorker = nullptr;
    std::atomic<quint64> m_RequestGeneration = 0;
};

} // namespace motrack
