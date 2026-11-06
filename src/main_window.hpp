#pragma once

#include "image_sequence.hpp"
#include "legacy_project.hpp"
#include "track_edits.hpp"

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QMainWindow>
#include <QThreadPool>

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

class MainWindow final : public QMainWindow
{
    Q_DECLARE_TR_FUNCTIONS(MainWindow)

  public:
    MainWindow();
    ~MainWindow() override;
    bool loadProject(const QString& path);
    bool saveMeasurements(const QString& path);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void displayStep(int step);
    void displayMarks();
    void displayFrame(const ImageFrame& frame, const QString& failureMessage);
    void requestMovieFrame(int frame);
    void refreshEditViews(int track);
    void refreshPlot();
    void markAt(const QPointF& position);
    void repeatPreviousMark();
    [[nodiscard]] bool confirmDiscard();

    VideoCanvas* m_Video;
    QLabel* m_FrameLabel;
    QSlider* m_StepSlider;
    QToolButton* m_Previous;
    QToolButton* m_Next;
    QTableWidget* m_Marks;
    QAction* m_Export;
    QAction* m_NewPointMass;
    QAction* m_Undo;
    QAction* m_Redo;
    QComboBox* m_TrackPicker;
    QComboBox* m_PlotQuantity;
    PositionPlot* m_Plot;
    LegacyProject m_Project;
    TrackEdits m_Edits;
    std::optional<ImageSequence> m_Sequence;
    std::optional<ImageFrame> m_DisplayedFrame;
    QString m_MoviePath;
    QThreadPool m_MovieWorkers;
    std::unique_ptr<QFutureWatcher<QImage>> m_FrameWatcher;
    std::shared_ptr<std::atomic_uint64_t> m_RequestGeneration = std::make_shared<std::atomic_uint64_t>(0);
};

} // namespace motrack
