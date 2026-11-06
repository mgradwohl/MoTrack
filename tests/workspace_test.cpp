#include "image_sequence.hpp"
#include "main_window.hpp"
#include "workspace_document.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <gtest/gtest.h>

#include <algorithm>

namespace
{

QString fixturePath(const QString& name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + '/' + name;
}

void triggerFileDialog(QAction& action, QWidget& window, const QString& path)
{
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog,
                     &QTimer::timeout,
                     &window,
                     []
                     {
                         if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                         {
                             dialog->reject();
                         }
                     });
    watchdog.start(2000);
    QTimer::singleShot(0,
                       &window,
                       [path]
                       {
                           if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                           {
                               dialog->selectFile(path);
                               QMetaObject::invokeMethod(dialog, "accept");
                           }
                       });
    action.trigger();
}

TEST(ImageSequence, ReportsMissingImagesAndRejectsOutOfRangeSteps)
{
    motrack::Clip clip;
    EXPECT_FALSE(motrack::ImageSequence::open(fixturePath("image-sequence.trk"), clip).has_value());
    clip.mediaReferences = {"https://example.invalid/remote.ppm", "archive.trz!/frame.ppm", "missing.ppm"};
    const auto sequence = motrack::ImageSequence::open(fixturePath("image-sequence.trk"), clip);
    if (!sequence)
    {
        FAIL() << "Could not open the test image sequence.";
    }
    EXPECT_EQ(sequence->stepCount(), 3);
    EXPECT_FALSE(sequence->readStep(-1).has_value());
    EXPECT_FALSE(sequence->readStep(3).has_value());
    for (int step = 0; step < sequence->stepCount(); ++step)
    {
        const auto frame = sequence->readStep(step);
        if (!frame)
        {
            FAIL() << "Could not read test image sequence step " << step;
        }
        EXPECT_EQ(frame->number, step);
        EXPECT_TRUE(frame->image.isNull());
    }
    clip.firstFrame = 3;
    EXPECT_FALSE(motrack::ImageSequence::open(fixturePath("image-sequence.trk"), clip).has_value());
}

TEST(Workspace, OpensAProjectThroughTheFileMenu)
{
    motrack::MainWindow window;
    window.show();
    auto* open = window.findChild<QAction*>("openProject");
    ASSERT_NE(open, nullptr);
    triggerFileDialog(*open, window, fixturePath("image-sequence.trk"));
    EXPECT_TRUE(window.windowTitle().contains("image-sequence.trk"));
}

TEST(Workspace, OpensATrackerArchiveAndReopensASavedCopy)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    motrack::MainWindow window;
    window.show();
    auto* open = window.findChild<QAction*>("openProject");
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* marks = window.findChild<QTableWidget*>("marksTable");
    ASSERT_TRUE(open && canvas && marks);
    triggerFileDialog(*open, window, fixturePath("archive.trz"));
    EXPECT_TRUE(window.windowTitle().contains("archive.trz"));
    EXPECT_EQ(canvas->accessibleName(), "Frame 0");
    EXPECT_EQ(marks->rowCount(), 2);
    const auto path = temporary.filePath("archived.motrack");
    ASSERT_TRUE(window.saveWorkspace(path));
    ASSERT_TRUE(window.loadProject(fixturePath("point-mass.trk")));
    ASSERT_TRUE(window.loadProject(path));
    EXPECT_EQ(canvas->accessibleName(), "Frame 0");
    EXPECT_EQ(marks->rowCount(), 2);
}

TEST(Workspace, DisplaysWorldCoordinatesAndExportsMeasurements)
{
    motrack::MainWindow window;
    window.show();
    auto* action = window.findChild<QAction*>("exportMeasurements");
    auto* marks = window.findChild<QTableWidget*>("marksTable");
    ASSERT_TRUE(action && marks);
    EXPECT_FALSE(action->isEnabled());
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    EXPECT_TRUE(action->isEnabled());
    ASSERT_NE(marks->item(0, 4), nullptr);
    ASSERT_NE(marks->item(0, 5), nullptr);
    EXPECT_EQ(marks->item(0, 4)->text(), "0.75");
    EXPECT_EQ(marks->item(0, 5)->text(), "0.625");

    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    triggerFileDialog(*action, window, temporary.filePath("measurements"));
    QFile csv(temporary.filePath("measurements.csv"));
    ASSERT_TRUE(csv.open(QIODevice::ReadOnly));
    EXPECT_TRUE(csv.readAll().contains("Ball,0,1.5,1.5,0.75,0.625,m\r\n"));
    EXPECT_FALSE(window.saveMeasurements(temporary.filePath("missing/measurements.csv")));
}

TEST(Workspace, PlotsPositionAndSeeksToASelectedMeasurement)
{
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    auto* plot = window.findChild<QWidget*>("positionPlot");
    auto* quantities = window.findChild<QComboBox*>("plotQuantity");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    ASSERT_TRUE(plot && quantities && slider);
    EXPECT_EQ(plot->accessibleName(), "x position (m) versus Time (s)");
    EXPECT_FALSE(plot->grab().isNull());
    QTest::mouseClick(plot, Qt::LeftButton, Qt::NoModifier, QPoint(plot->width() - 17, 24));
    EXPECT_EQ(slider->value(), 1);
    quantities->setCurrentIndex(3);
    EXPECT_EQ(plot->accessibleName(), "y velocity (m/s) versus Time (s)");
    EXPECT_FALSE(plot->grab().isNull());
    QTest::mouseClick(plot, Qt::LeftButton, Qt::NoModifier, plot->rect().center());
    EXPECT_EQ(slider->value(), 1);
}

TEST(Workspace, CalibratesAFrameUsingTwoClickedEndpoints)
{
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* plot = window.findChild<QWidget*>("positionPlot");
    auto* marks = window.findChild<QTableWidget*>("marksTable");
    auto* action = window.findChild<QAction*>("calibrationStick");
    auto* undo = window.findChild<QAction*>("undoEdit");
    ASSERT_TRUE(canvas && plot && marks && action && undo);
    ASSERT_TRUE(action->isEnabled());

    const auto center = canvas->rect().center();
    const auto edge = std::min(canvas->width(), canvas->height());
    const QPoint first = center - QPoint(edge / 4, 0);
    const QPoint second = center + QPoint(edge / 4, 0);
    action->trigger();
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, first);
    EXPECT_FALSE(canvas->grab().isNull());
    QTimer::singleShot(0,
                       &window,
                       []
                       {
                           if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                           {
                               dialog->findChild<QDoubleSpinBox*>("knownDistance")->setValue(2);
                               dialog->findChild<QLineEdit*>("lengthUnit")->setText("cm");
                               dialog->accept();
                           }
                       });
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, second);
    EXPECT_FALSE(action->isChecked());
    EXPECT_TRUE(undo->isEnabled());
    EXPECT_TRUE(plot->accessibleName().contains("(cm)"));
    const auto pixelDistance = 4.0 * (second.x() - first.x()) / edge;
    EXPECT_NEAR(marks->item(0, 4)->text().toDouble(), 3.0 / pixelDistance, 1e-8);
    undo->trigger();
    EXPECT_EQ(marks->item(0, 4)->text(), "0.75");
}

TEST(Workspace, StepsThroughImagesAndKeepsImportedMarks)
{
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    auto* slider = window.findChild<QSlider*>("stepSlider");
    auto* previous = window.findChild<QToolButton*>("previousFrame");
    auto* next = window.findChild<QToolButton*>("nextFrame");
    auto* label = window.findChild<QLabel*>("frameLabel");
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* marks = window.findChild<QTableWidget*>("marksTable");
    ASSERT_TRUE(slider && previous && next && label && canvas && marks);
    EXPECT_EQ(marks->rowCount(), 2);
    EXPECT_EQ(slider->maximum(), 1);
    EXPECT_EQ(label->text(), "Frame 0");
    EXPECT_FALSE(previous->isEnabled());
    EXPECT_TRUE(next->isEnabled());
    const auto firstImage = canvas->grab().toImage();
    ASSERT_FALSE(firstImage.isNull());
    const auto firstColor = firstImage.pixelColor(firstImage.rect().center());
    next->click();
    EXPECT_EQ(label->text(), "Frame 2");
    EXPECT_TRUE(previous->isEnabled());
    EXPECT_FALSE(next->isEnabled());
    const auto lastImage = canvas->grab().toImage();
    ASSERT_FALSE(lastImage.isNull());
    EXPECT_NE(lastImage.pixelColor(lastImage.rect().center()), firstColor);
    previous->click();
    EXPECT_EQ(label->text(), "Frame 0");
    EXPECT_FALSE(window.loadProject(fixturePath("missing.trk")));
    EXPECT_EQ(marks->rowCount(), 2);
    EXPECT_EQ(label->text(), "Frame 0");
    ASSERT_TRUE(window.loadProject(fixturePath("point-mass.trk")));
    EXPECT_FALSE(slider->isEnabled());
    EXPECT_TRUE(label->text().isEmpty());
}

TEST(Workspace, StepsThroughAMovieOnABackgroundWorker)
{
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixturePath("movie.trk")));
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    ASSERT_TRUE(canvas && slider);
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 1"; }));
    slider->setValue(1);
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 3"; }));
    slider->setValue(0);
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 1"; }));
    slider->setValue(1);
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    QCoreApplication::processEvents();
    EXPECT_EQ(canvas->accessibleName(), "Frame 0");
}

TEST(Workspace, OpensStandaloneMovieAndKeepsTheWorkspaceAfterAFailedOpen)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    motrack::MainWindow window;
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    auto* status = window.statusBar();
    ASSERT_TRUE(canvas && slider && status);
    window.openMovie(fixturePath("vfr.mp4"));
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 0"; }));
    EXPECT_EQ(slider->maximum(), 3);
    slider->setValue(3);
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 3"; }));
    slider->setValue(2);
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 2"; }));
    slider->setValue(3);
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 3"; }));
    window.openMovie(fixturePath("missing.mp4"));
    ASSERT_TRUE(QTest::qWaitFor([status] { return status->currentMessage() == "Could not open this video."; }));
    EXPECT_EQ(canvas->accessibleName(), "Frame 3");
    const auto destination = temporary.filePath("movie.motrack");
    ASSERT_TRUE(window.saveWorkspace(destination));
    ASSERT_TRUE(window.loadProject(destination));
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 0"; }));
    EXPECT_EQ(slider->maximum(), 3);
}

TEST(Workspace, TrimsAnImageSequenceAndRestoresItWithUndo)
{
    motrack::MainWindow window;
    window.show();
    auto* settings = window.findChild<QAction*>("clipSettings");
    auto* undo = window.findChild<QAction*>("undoEdit");
    auto* redo = window.findChild<QAction*>("redoEdit");
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    ASSERT_TRUE(settings && undo && redo && canvas && slider);
    EXPECT_FALSE(settings->isEnabled());
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    ASSERT_TRUE(settings->isEnabled());
    QTimer::singleShot(0,
                       &window,
                       []
                       {
                           if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                           {
                               dialog->findChild<QSpinBox*>("clipStart")->setValue(2);
                               dialog->accept();
                           }
                       });
    settings->trigger();
    EXPECT_EQ(canvas->accessibleName(), "Frame 2");
    EXPECT_EQ(slider->maximum(), 0);
    undo->trigger();
    EXPECT_EQ(canvas->accessibleName(), "Frame 0");
    EXPECT_EQ(slider->maximum(), 1);
    redo->trigger();
    EXPECT_EQ(canvas->accessibleName(), "Frame 2");
}

TEST(Workspace, AutoTracksATexturedMarkAndStopsWhenItDisappears)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    motrack::WorkspaceDocument document;
    document.project.pointMasses.push_back({.name = "Ball", .marks = {{.frame = 0, .x = 22, .y = 22}}});
    for (int frame = 0; frame < 3; ++frame)
    {
        const auto name = QString("frame%1.png").arg(frame);
        document.project.clip.mediaReferences.push_back(name.toStdString());
        QImage image(64, 48, QImage::Format_Grayscale8);
        image.fill(30);
        if (frame < 2)
        {
            const int center = 22 + frame * 6;
            for (int y = -5; y <= 5; ++y)
            {
                for (int x = -5; x <= 5; ++x)
                {
                    const int value = ((x + 5) * 71 + (y + 5) * 103 + (x * y + 25) * 19) % 190 + 30;
                    image.scanLine(22 + y)[center + x] = static_cast<uchar>(value);
                }
            }
        }
        ASSERT_TRUE(image.save(temporary.filePath(name)));
    }
    const auto path = temporary.filePath("autotrack.motrack");
    motrack::writeWorkspaceDocument(path, document);
    motrack::MainWindow window;
    auto* action = window.findChild<QAction*>("autoTracker");
    auto* marks = window.findChild<QTableWidget*>("marksTable");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    auto* undo = window.findChild<QAction*>("undoEdit");
    ASSERT_TRUE(action && marks && slider && undo);
    EXPECT_FALSE(action->isEnabled());
    ASSERT_TRUE(window.loadProject(path));
    ASSERT_TRUE(action->isEnabled());
    slider->setValue(1);
    action->trigger();
    EXPECT_FALSE(action->isChecked());
    EXPECT_EQ(window.statusBar()->currentMessage(), "Mark a point mass on this frame to start AutoTracker.");
    slider->setValue(0);
    action->trigger();
    EXPECT_TRUE(action->isChecked());
    action->trigger();
    QCoreApplication::processEvents();
    EXPECT_EQ(slider->value(), 0);
    action->trigger();
    ASSERT_TRUE(QTest::qWaitFor([slider, action] { return slider->value() == 2 && !action->isChecked(); }));
    ASSERT_EQ(marks->rowCount(), 2);
    EXPECT_EQ(marks->item(1, 1)->text(), "1");
    EXPECT_EQ(marks->item(1, 2)->text(), "28");
    EXPECT_EQ(window.statusBar()->currentMessage(), "AutoTracker could not find a reliable match. Mark this frame to continue.");
    undo->trigger();
    EXPECT_EQ(marks->rowCount(), 1);

    document.project.clip.stepCount = 2;
    motrack::writeWorkspaceDocument(path, document);
    ASSERT_TRUE(window.loadProject(path));
    action->trigger();
    ASSERT_TRUE(QTest::qWaitFor([slider, action] { return slider->value() == 1 && !action->isChecked(); }));
    EXPECT_EQ(window.statusBar()->currentMessage(), "AutoTracker reached the end of the clip.");

    document.project.pointMasses.front().marks.push_back({.frame = 1, .x = 30, .y = 22});
    motrack::writeWorkspaceDocument(path, document);
    ASSERT_TRUE(window.loadProject(path));
    action->trigger();
    ASSERT_TRUE(QTest::qWaitFor([slider, action] { return slider->value() == 1 && !action->isChecked(); }));
    ASSERT_EQ(marks->rowCount(), 2);
    EXPECT_EQ(marks->item(1, 2)->text(), "30");
    EXPECT_EQ(window.statusBar()->currentMessage(), "AutoTracker stopped at an existing mark.");
}

TEST(Workspace, PlaysTheClipAndStopsAtTheLastFrame)
{
    motrack::MainWindow window;
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    auto* play = window.findChild<QToolButton*>("playClip");
    ASSERT_TRUE(canvas && slider && play);
    EXPECT_FALSE(play->isEnabled());
    window.openMovie(fixturePath("vfr.mp4"));
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 0"; }));
    play->click();
    EXPECT_EQ(play->accessibleName(), "Pause");
    play->click();
    EXPECT_EQ(play->accessibleName(), "Play");
    EXPECT_EQ(slider->value(), 0);
    play->click();
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 3"; }));
    EXPECT_EQ(play->accessibleName(), "Play");
    slider->setValue(1);
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 1"; }));
    slider->setValue(3);
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 3"; }));
    play->click();
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 0"; }));
    play->click();
}

TEST(Workspace, SavesEditsAndReopensMediaFromADifferentDirectory)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* marks = window.findChild<QTableWidget*>("marksTable");
    auto* undo = window.findChild<QAction*>("undoEdit");
    auto* redo = window.findChild<QAction*>("redoEdit");
    auto* save = window.findChild<QAction*>("saveWorkspace");
    auto* saveCurrent = window.findChild<QAction*>("saveProject");
    ASSERT_TRUE(canvas && marks && undo && redo && save && saveCurrent);
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::ShiftModifier, canvas->rect().center());
    ASSERT_TRUE(window.isWindowModified());
    const auto destination = temporary.filePath("experiment.motrack");
    EXPECT_FALSE(window.saveWorkspace(temporary.filePath("missing/experiment.motrack")));
    EXPECT_FALSE(window.saveWorkspace(temporary.filePath("experiment.trk")));
    EXPECT_TRUE(window.isWindowModified());
    triggerFileDialog(*save, window, temporary.filePath("experiment"));
    ASSERT_TRUE(QFile::exists(destination));
    EXPECT_FALSE(window.isWindowModified());
    undo->trigger();
    EXPECT_TRUE(window.isWindowModified());
    redo->trigger();
    EXPECT_FALSE(window.isWindowModified());
    undo->trigger();
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::ShiftModifier, canvas->rect().center() + QPoint(10, 0));
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_FALSE(redo->isEnabled());
    const auto updatedX = marks->item(1, 2)->text();
    saveCurrent->trigger();
    EXPECT_FALSE(window.isWindowModified());
    ASSERT_TRUE(window.loadProject(destination));
    EXPECT_FALSE(window.isWindowModified());
    EXPECT_EQ(canvas->accessibleName(), "Frame 0");
    EXPECT_EQ(marks->rowCount(), 2);
    EXPECT_EQ(marks->item(1, 2)->text(), updatedX);
    EXPECT_FALSE(window.loadProject(temporary.filePath("missing.motrack")));
    EXPECT_EQ(marks->rowCount(), 2);
    QFile invalid(temporary.filePath("invalid.motrack"));
    ASSERT_TRUE(invalid.open(QIODevice::WriteOnly));
    ASSERT_GT(invalid.write(R"({"format":"MoTrack","version":999})"), 0);
    invalid.close();
    EXPECT_FALSE(window.loadProject(invalid.fileName()));
    EXPECT_EQ(marks->rowCount(), 2);
}

TEST(Workspace, ReportsUnavailableMovieFrames)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    ASSERT_TRUE(QFile::copy(fixturePath("vfr.mp4"), temporary.filePath("vfr.mp4")));
    QFile source(fixturePath("movie.trk"));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    auto xml = source.readAll();
    xml.replace(R"(name="stepcount" type="int">2)", R"(name="stepcount" type="int">3)");
    QFile project(temporary.filePath("movie.trk"));
    ASSERT_TRUE(project.open(QIODevice::WriteOnly));
    ASSERT_EQ(project.write(xml), xml.size());
    project.close();

    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(project.fileName()));
    auto* slider = window.findChild<QSlider*>("stepSlider");
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    ASSERT_TRUE(slider && canvas);
    slider->setValue(2);
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "This video frame could not be decoded."; }));
}

TEST(Workspace, MarksASelectedPointMassAdvancesAndSupportsUndo)
{
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    auto* picker = window.findChild<QComboBox*>("trackPicker");
    auto* marks = window.findChild<QTableWidget*>("marksTable");
    auto* add = window.findChild<QAction*>("newPointMass");
    auto* undo = window.findChild<QAction*>("undoEdit");
    auto* redo = window.findChild<QAction*>("redoEdit");
    auto* repeat = window.findChild<QAction*>("repeatMark");
    ASSERT_TRUE(canvas && slider && picker && marks && add && undo && redo && repeat);
    EXPECT_FALSE(undo->isEnabled());
    add->trigger();
    EXPECT_EQ(picker->count(), 2);
    EXPECT_EQ(picker->currentText(), "Point Mass 2");
    EXPECT_TRUE(window.isWindowModified());
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, canvas->rect().center());
    EXPECT_EQ(marks->rowCount(), 2);
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::ShiftModifier, canvas->rect().center());
    EXPECT_EQ(marks->rowCount(), 3);
    EXPECT_EQ(slider->value(), 1);
    repeat->trigger();
    EXPECT_EQ(marks->rowCount(), 4);
    EXPECT_EQ(marks->item(2, 2)->text(), marks->item(3, 2)->text());
    EXPECT_EQ(marks->item(2, 3)->text(), marks->item(3, 3)->text());

    undo->trigger();
    EXPECT_EQ(marks->rowCount(), 3);
    EXPECT_TRUE(redo->isEnabled());
    redo->trigger();
    EXPECT_EQ(marks->rowCount(), 4);
}

TEST(Workspace, LetsTheUserCancelOrDiscardUnsavedTrackingEdits)
{
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    auto* add = window.findChild<QAction*>("newPointMass");
    ASSERT_NE(add, nullptr);
    add->trigger();

    const auto choose = [&window](QMessageBox::StandardButton choice)
    {
        QTimer::singleShot(0,
                           &window,
                           [choice]
                           {
                               if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                               {
                                   dialog->button(choice)->click();
                               }
                           });
        return window.close();
    };
    EXPECT_FALSE(choose(QMessageBox::Cancel));
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_TRUE(choose(QMessageBox::Discard));
}

} // namespace

int main(int argc, char** argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const QApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
