#pragma once

#include "image_sequence.hpp"
#include "legacy_project.hpp"
#include "template_tracker.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QMainWindow>
#include <QPointF>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>

class QLabel;
class QAction;
class QCloseEvent;
class QComboBox;
class QSlider;
class QTableWidget;
class QToolButton;

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
    bool saveMeasurements(const QString& path);
    bool saveWorkspace(const QString& path);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    struct PreparedProject;
    [[nodiscard]] static PreparedProject prepareProject(const QString& path, const std::atomic_bool* cancelled);
    bool applyProject(const QString& path, PreparedProject prepared, const std::atomic_bool* cancelled);
    void cancelProjectOpen();
    void displayStep(int step);
    void displayMarks();
    void displayFrame(const ImageFrame& frame, const QString& failureMessage);
    void requestMovieFrame(int frame);
    void setPlaying(bool playing);
    void schedulePlayback();
    void startAutoTracking();
    void stopAutoTracking(const QString& message = {});
    void updateAutoTrackerAction();
    void autoTrackFrame(const ImageFrame& frame);
    void activateProject(const QString& path);
    void syncClipControls();
    void editClipSettings();
    void undoOrRedo(bool redo);
    void cancelMovieOpen();
    void releaseArchive();
    bool chooseWorkspacePath();
    bool saveCurrentWorkspace();
    void refreshEditViews(int track);
    void refreshPlot();
    void markAt(const QPointF& position);
    void repeatPreviousMark();
    void pickCalibrationPoint(const QPointF& position);
    void pickAxesPoint(const QPointF& position);
    void refreshAxesOverlay();
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
    QAction* m_SaveAs;
    QAction* m_Save;
    QAction* m_NewPointMass;
    QAction* m_Undo;
    QAction* m_Redo;
    QAction* m_Calibrate;
    QAction* m_ClipSettings;
    QAction* m_AutoTracker;
    QAction* m_ShowAxes;
    QAction* m_EditAxes;
    QComboBox* m_TrackPicker;
    QComboBox* m_PlotQuantity;
    PositionPlot* m_Plot;
    LegacyProject m_Project;
    TrackEdits m_Edits;
    std::optional<ImageSequence> m_Sequence;
    std::optional<ImageFrame> m_DisplayedFrame;
    std::optional<QPointF> m_CalibrationFirst;
    std::optional<QPointF> m_AxesOrigin;
    struct AutoTrackSession
    {
        TemplateTracker pattern;
        std::size_t track;
        int lastFrame;
        QPoint predicted;
    };
    std::optional<AutoTrackSession> m_AutoTrack;
    QTimer m_AutoTrackTimer;
    QString m_MoviePath;
    bool m_MovieUsesDisplayOrientation = false;
    QString m_ProjectPath;
    QString m_MediaProjectPath;
    QString m_ArchiveSourcePath;
    std::optional<TrackerArchive> m_Archive;
    QString m_SavePath;
    struct MovieIndex
    {
        int frameCount = 0;
        std::vector<double> frameTimes;
    };
    std::vector<double> m_MovieFrameTimes;
    std::shared_ptr<std::atomic_bool> m_MovieOpenCancelled;
    std::unique_ptr<QFutureWatcher<MovieIndex>> m_MovieOpenWatcher;
    std::shared_ptr<std::atomic_bool> m_ProjectOpenCancelled;
    std::unique_ptr<QFutureWatcher<std::shared_ptr<PreparedProject>>> m_ProjectOpenWatcher;
    QThread m_MovieThread;
    MovieWorker* m_MovieWorker = nullptr;
    std::atomic<quint64> m_RequestGeneration = 0;
};

} // namespace motrack
