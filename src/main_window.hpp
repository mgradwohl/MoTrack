#pragma once

#include "image_sequence.hpp"
#include "legacy_project.hpp"

#include <QCoreApplication>
#include <QMainWindow>

class QLabel;
class QSlider;
class QTableWidget;
class QToolButton;

namespace motrack
{

class VideoCanvas;

class MainWindow final : public QMainWindow
{
    Q_DECLARE_TR_FUNCTIONS(MainWindow)

  public:
    MainWindow();
    bool loadProject(const QString& path);

  private:
    void displayStep(int step);
    void displayMarks();

    VideoCanvas* m_Video;
    QLabel* m_FrameLabel;
    QSlider* m_StepSlider;
    QToolButton* m_Previous;
    QToolButton* m_Next;
    QTableWidget* m_Marks;
    LegacyProject m_Project;
    std::optional<ImageSequence> m_Sequence;
};

} // namespace motrack
