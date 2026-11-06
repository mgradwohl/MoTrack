#include "image_sequence.hpp"
#include "main_window.hpp"
#include "video_canvas.hpp"
#include "workspace_document.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTranslator>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <utility>

namespace
{

QString fixturePath(const QString& name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + '/' + name;
}

TEST(Workspace, LocalizesTheFrameControlsForRegionalLocales)
{
    constexpr std::array LOCALES{
        std::pair{"de_AT", "Vorheriges Bild"}, std::pair{"es_MX", "Fotograma anterior"}, std::pair{"fr_CA", "Image précédente"}};
    for (const auto& [locale, expected] : LOCALES)
    {
        QTranslator translator;
        ASSERT_TRUE(translator.load(QLocale(QString::fromUtf8(locale)), "motrack", "_", QString::fromUtf8(MOTRACK_TRANSLATIONS)));
        ASSERT_TRUE(QCoreApplication::installTranslator(&translator));
        motrack::MainWindow window;
        auto* previous = window.findChild<QToolButton*>("previousFrame");
        EXPECT_NE(previous, nullptr);
        if (previous)
        {
            EXPECT_EQ(previous->accessibleName(), QString::fromUtf8(expected));
        }
        QCoreApplication::removeTranslator(&translator);
    }
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
    EXPECT_TRUE(QTest::qWaitFor([&window] { return window.windowTitle().contains("image-sequence.trk"); }));
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
    ASSERT_TRUE(QTest::qWaitFor([&window] { return window.windowTitle().contains("archive.trz"); }));
    EXPECT_EQ(canvas->accessibleName(), "Frame 0");
    EXPECT_EQ(marks->rowCount(), 2);
    const auto path = temporary.filePath("archived.motrack");
    ASSERT_TRUE(window.saveWorkspace(path));
    ASSERT_TRUE(window.loadProject(fixturePath("point-mass.trk")));
    ASSERT_TRUE(window.loadProject(path));
    EXPECT_EQ(canvas->accessibleName(), "Frame 0");
    EXPECT_EQ(marks->rowCount(), 2);
}

TEST(Workspace, KeepsOnlyTheLatestBackgroundProjectRequest)
{
    motrack::MainWindow window;
    auto* status = window.statusBar();
    window.openProject(fixturePath("archive.trz"));
    window.openProject(fixturePath("point-mass.trk"));
    ASSERT_TRUE(QTest::qWaitFor([&window] { return window.windowTitle().contains("point-mass.trk"); }));
    window.openProject(fixturePath("missing.motrack"));
    ASSERT_TRUE(QTest::qWaitFor([status] { return status->currentMessage() == "Could not open this project."; }));
    EXPECT_TRUE(window.windowTitle().contains("point-mass.trk"));
}

TEST(Workspace, PreservesUnsavedEditsWhenABackgroundOpenIsCancelled)
{
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    auto* add = window.findChild<QAction*>("newPointMass");
    ASSERT_NE(add, nullptr);
    add->trigger();
    ASSERT_TRUE(window.isWindowModified());
    QTimer answer;
    bool cancelled = false;
    QObject::connect(&answer,
                     &QTimer::timeout,
                     &window,
                     [&cancelled]
                     {
                         if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                         {
                             dialog->button(QMessageBox::Cancel)->click();
                             cancelled = true;
                         }
                     });
    answer.start(10);
    window.openProject(fixturePath("point-mass.trk"));
    ASSERT_TRUE(QTest::qWaitFor([&cancelled] { return cancelled; }));
    EXPECT_TRUE(window.windowTitle().contains("image-sequence.trk"));
    EXPECT_TRUE(window.isWindowModified());
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
    quantities->setCurrentIndex(4);
    EXPECT_EQ(plot->accessibleName(), "speed (m/s) versus Time (s)");
    quantities->setCurrentIndex(5);
    EXPECT_EQ(plot->accessibleName(), QString::fromUtf8("x acceleration (m/s²) versus Time (s)"));
    quantities->setCurrentIndex(7);
    EXPECT_EQ(plot->accessibleName(), QString::fromUtf8("x momentum (kg·m/s) versus Time (s)"));
    quantities->setCurrentIndex(9);
    EXPECT_EQ(plot->accessibleName(), QString::fromUtf8("kinetic energy (kg·m²/s²) versus Time (s)"));
}

TEST(Workspace, PreservesAndDrawsVectorsAtTheirOriginalFrame)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    auto document = motrack::WorkspaceDocument{};
    document.project.clip.stepCount = 1;
    document.project.clip.mediaReferences.push_back(fixturePath("frames/frame00.ppm").toStdString());
    document.project.vectors.push_back({.name = "Vector A",
                                        .samples = {{.frame = 0, .tailX = 1, .tailY = 1, .componentX = 2, .componentY = 1},
                                                    {.frame = 2, .tailX = 1, .tailY = 2, .componentX = 1, .componentY = 0}}});
    const auto input = temporary.filePath("vectors.motrack");
    motrack::writeWorkspaceDocument(input, document);

    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(input));
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    ASSERT_NE(canvas, nullptr);
    const auto pixels = canvas->grab().toImage();
    const auto color = QColor(Qt::green);
    bool foundArrow = false;
    for (int y = 0; y < pixels.height() && !foundArrow; ++y)
    {
        for (int x = 0; x < pixels.width(); ++x)
        {
            if (pixels.pixelColor(x, y) == color)
            {
                foundArrow = true;
                break;
            }
        }
    }
    EXPECT_TRUE(foundArrow);
    const auto savedPath = temporary.filePath("vectors-saved.motrack");
    ASSERT_TRUE(window.saveWorkspace(savedPath));
    const auto saved = motrack::readWorkspaceDocument(savedPath);
    ASSERT_EQ(saved.project.vectors.size(), 1u);
    const auto& vectors = saved.project.vectors.front();
    EXPECT_EQ(vectors.name, "Vector A");
    ASSERT_EQ(vectors.samples.size(), 2u);
    EXPECT_EQ(vectors.samples.back().frame, 2);
    EXPECT_DOUBLE_EQ(vectors.samples.front().componentY, 1.0);
}

TEST(Workspace, ExportsTheSelectedPlotWithItsUnitsAndTimebase)
{
    motrack::MainWindow window;
    window.show();
    auto* exportPlot = window.findChild<QAction*>("exportPlotData");
    auto* quantities = window.findChild<QComboBox*>("plotQuantity");
    ASSERT_TRUE(exportPlot && quantities);
    EXPECT_FALSE(exportPlot->isEnabled());
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    quantities->setCurrentIndex(3);
    EXPECT_FALSE(exportPlot->isEnabled());
    quantities->setCurrentIndex(1);
    EXPECT_TRUE(exportPlot->isEnabled());

    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    triggerFileDialog(*exportPlot, window, temporary.filePath("series"));
    QFile csv(temporary.filePath("series.csv"));
    ASSERT_TRUE(csv.open(QIODevice::ReadOnly));
    const auto rows = csv.readAll();
    EXPECT_TRUE(rows.startsWith("frame,time_s,y position (m)\r\n"));
    EXPECT_TRUE(rows.contains("\r\n0,-0.5,0.625\r\n"));
}

TEST(Workspace, ShowsALinearFitForTheSelectedQuantity)
{
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    auto* fit = window.findChild<QCheckBox*>("linearFit");
    auto* summary = window.findChild<QLabel*>("linearFitSummary");
    auto* quantities = window.findChild<QComboBox*>("plotQuantity");
    auto* plot = window.findChild<QWidget*>("positionPlot");
    ASSERT_TRUE(fit && summary && quantities && plot);
    EXPECT_FALSE(summary->isVisible());
    fit->click();
    EXPECT_TRUE(summary->isVisible());
    EXPECT_TRUE(summary->text().contains(QString::fromUtf8("R² 1")));
    EXPECT_FALSE(plot->grab().isNull());
    quantities->setCurrentIndex(2);
    EXPECT_EQ(summary->text(), "Linear fit needs two distinct horizontal values.");
    fit->click();
    EXPECT_FALSE(summary->isVisible());
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

TEST(Workspace, IndexesAnImportedMoviesTimebase)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixturePath("movie.trk")));
    auto* plot = window.findChild<QWidget*>("positionPlot");
    ASSERT_NE(plot, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([plot] { return plot->accessibleName() == "x position (px) versus Time (s)"; }));
    const auto path = temporary.filePath("timed-clip.motrack");
    ASSERT_TRUE(window.saveWorkspace(path));
    const auto saved = motrack::readWorkspaceDocument(path);
    EXPECT_TRUE(saved.alignMovieToClip);
    ASSERT_EQ(saved.movieFrameTimes.size(), 4u);
    EXPECT_NEAR(saved.movieFrameTimes.at(1), 0.128, 1e-12);
    EXPECT_NEAR(saved.movieFrameTimes.at(3), 0.328, 1e-12);
    EXPECT_DOUBLE_EQ(saved.project.clip.startTimeMs, -200.0);
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
    EXPECT_TRUE(motrack::readWorkspaceDocument(destination).useDisplayOrientation);
    ASSERT_TRUE(window.loadProject(destination));
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 0"; }));
    EXPECT_EQ(slider->maximum(), 3);
}

TEST(Workspace, OpensStandaloneImagesAndSavesTheirTiming)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    motrack::MainWindow window;
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    ASSERT_TRUE(canvas && slider);
    EXPECT_FALSE(window.openImageSequence({}, 0.0));
    EXPECT_FALSE(window.openImageSequence({fixturePath("frames/frame00.ppm")}, -1.0));
    ASSERT_TRUE(window.openImageSequence({fixturePath("frames/frame00.ppm"), fixturePath("frames/frame02.ppm")}, 4.0));
    EXPECT_EQ(canvas->accessibleName(), "Frame 0");
    EXPECT_EQ(slider->maximum(), 1);
    slider->setValue(1);
    EXPECT_EQ(canvas->accessibleName(), "Frame 1");
    EXPECT_FALSE(window.openImageSequence({fixturePath("frames/missing.ppm")}, 0.0));
    EXPECT_EQ(canvas->accessibleName(), "Frame 1");
    const auto destination = temporary.filePath("images.motrack");
    ASSERT_TRUE(window.saveWorkspace(destination));
    EXPECT_EQ(motrack::readWorkspaceDocument(destination).project.clip.frameDurationMs, 250.0);
    ASSERT_TRUE(window.loadProject(destination));
    EXPECT_EQ(canvas->accessibleName(), "Frame 0");
    EXPECT_EQ(slider->maximum(), 1);
}

TEST(Workspace, OpensAnImageThroughTheFileMenu)
{
    motrack::MainWindow window;
    window.show();
    auto* open = window.findChild<QAction*>("openImages");
    ASSERT_NE(open, nullptr);
    QTimer timing;
    QObject::connect(&timing,
                     &QTimer::timeout,
                     &window,
                     []
                     {
                         if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                         {
                             dialog->setDoubleValue(5.0);
                             dialog->accept();
                         }
                     });
    timing.start(10);
    triggerFileDialog(*open, window, fixturePath("frames/frame00.ppm"));
    EXPECT_TRUE(window.windowTitle().contains("frame00.ppm"));
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

TEST(Workspace, PlacesCoordinateAxesAndUpdatesMeasurements)
{
    motrack::MainWindow window;
    window.show();
    auto* editAxes = window.findChild<QAction*>("setCoordinateAxes");
    auto* showAxes = window.findChild<QAction*>("showCoordinateAxes");
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* marks = window.findChild<QTableWidget*>("marksTable");
    auto* undo = window.findChild<QAction*>("undoEdit");
    ASSERT_TRUE(editAxes && showAxes && canvas && marks && undo);
    EXPECT_FALSE(editAxes->isEnabled());
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    const auto original = marks->item(0, 4)->text();
    editAxes->trigger();
    EXPECT_TRUE(showAxes->isChecked());
    const auto origin = canvas->rect().center();
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, origin);
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, origin);
    EXPECT_TRUE(editAxes->isChecked());
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, origin + QPoint(30, 0));
    EXPECT_FALSE(editAxes->isChecked());
    EXPECT_NE(marks->item(0, 4)->text(), original);
    EXPECT_FALSE(canvas->grab().isNull());
    undo->trigger();
    EXPECT_EQ(marks->item(0, 4)->text(), original);
    showAxes->trigger();
    EXPECT_FALSE(showAxes->isChecked());
}

TEST(Workspace, MeasuresDistanceAndAngleOnTheDisplayedFrame)
{
    motrack::MainWindow window;
    window.show();
    auto* distance = window.findChild<QAction*>("measureDistance");
    auto* angle = window.findChild<QAction*>("measureAngle");
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    ASSERT_TRUE(distance && angle && canvas && slider);
    EXPECT_FALSE(distance->isEnabled());
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    ASSERT_TRUE(distance->isEnabled());
    const auto center = canvas->rect().center();
    distance->trigger();
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, center);
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, center + QPoint(40, 0));
    EXPECT_FALSE(distance->isChecked());
    EXPECT_TRUE(window.statusBar()->currentMessage().startsWith("Distance "));
    EXPECT_TRUE(window.statusBar()->currentMessage().endsWith(" m"));
    angle->trigger();
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, center);
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, center);
    EXPECT_EQ(window.statusBar()->currentMessage(), "Choose a different measurement point.");
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, center + QPoint(40, 0));
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, center - QPoint(0, 40));
    EXPECT_FALSE(angle->isChecked());
    EXPECT_EQ(window.statusBar()->currentMessage(), QString::fromUtf8("Angle 90°"));
    EXPECT_FALSE(canvas->grab().isNull());
    slider->setValue(1);
    EXPECT_TRUE(window.statusBar()->currentMessage().isEmpty());
}

TEST(Workspace, KeepsMarkCoordinatesAccurateWhileZoomingAndPanning)
{
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* zoom = window.findChild<QAction*>("zoomVideoIn");
    auto* zoomOut = window.findChild<QAction*>("zoomVideoOut");
    auto* reset = window.findChild<QAction*>("resetVideoZoom");
    auto* marks = window.findChild<QTableWidget*>("marksTable");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    ASSERT_TRUE(canvas && zoom && zoomOut && reset && marks && slider);
    const auto center = canvas->rect().center();
    const auto click = center + QPoint(40, 0);
    const auto mark = [&]
    {
        slider->setValue(0);
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::ShiftModifier, click);
        return marks->item(0, 2)->text().toDouble();
    };
    const auto fitted = mark();
    zoom->trigger();
    EXPECT_LT(mark(), fitted);
    zoomOut->trigger();
    EXPECT_NEAR(mark(), fitted, 1e-9);
    for (int level = 0; level < 4; ++level)
    {
        zoom->trigger();
    }
    const auto beforePan = mark();
    slider->setValue(0);
    QTest::mousePress(canvas, Qt::MiddleButton, Qt::NoModifier, center);
    QTest::mouseMove(canvas, center + QPoint(32, 0));
    QTest::mouseRelease(canvas, Qt::MiddleButton, Qt::NoModifier, center + QPoint(32, 0));
    EXPECT_LT(mark(), beforePan);
    reset->trigger();
    EXPECT_NEAR(mark(), fitted, 1e-9);
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
    slider->setValue(1);
    action->trigger();
    slider->setValue(0);
    ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(5'000));
    QCoreApplication::processEvents();
    EXPECT_EQ(marks->rowCount(), 1);
    EXPECT_FALSE(action->isChecked());

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

TEST(Workspace, RenamesAndRemovesPointMassDataWithUndo)
{
    motrack::MainWindow window;
    window.show();
    auto* rename = window.findChild<QAction*>("renamePointMass");
    auto* removeTrack = window.findChild<QAction*>("removePointMass");
    auto* removeMark = window.findChild<QAction*>("removeMark");
    auto* undo = window.findChild<QAction*>("undoEdit");
    auto* marks = window.findChild<QTableWidget*>("marksTable");
    ASSERT_TRUE(rename && removeTrack && removeMark && undo && marks);
    EXPECT_FALSE(rename->isEnabled());
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    ASSERT_TRUE(rename->isEnabled());
    QTimer::singleShot(0,
                       &window,
                       []
                       {
                           if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                           {
                               dialog->setTextValue("Cart");
                               dialog->accept();
                           }
                       });
    rename->trigger();
    ASSERT_EQ(marks->rowCount(), 2);
    EXPECT_EQ(marks->item(0, 0)->text(), "Cart");
    ASSERT_TRUE(removeMark->isEnabled());
    removeMark->trigger();
    EXPECT_EQ(marks->rowCount(), 1);
    EXPECT_FALSE(removeMark->isEnabled());
    undo->trigger();
    EXPECT_EQ(marks->rowCount(), 2);
    ASSERT_TRUE(removeTrack->isEnabled());
    removeTrack->trigger();
    EXPECT_EQ(marks->rowCount(), 0);
    EXPECT_FALSE(rename->isEnabled());
    undo->trigger();
    ASSERT_EQ(marks->rowCount(), 2);
    EXPECT_EQ(marks->item(0, 0)->text(), "Cart");
}

TEST(Workspace, EditsAndSavesTheSelectedMass)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixturePath("point-mass.trk")));
    auto* action = window.findChild<QAction*>("setPointMassValue");
    ASSERT_TRUE(action && action->isEnabled());
    QTimer::singleShot(0,
                       &window,
                       []
                       {
                           if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                           {
                               dialog->setTextValue(QLocale().toString(250.25));
                               dialog->accept();
                           }
                       });
    action->trigger();
    const auto path = temporary.filePath("mass.motrack");
    ASSERT_TRUE(window.saveWorkspace(path));
    const auto saved = motrack::readWorkspaceDocument(path);
    EXPECT_EQ(saved.project.massUnit, "g");
    ASSERT_EQ(saved.project.pointMasses.size(), 1u);
    EXPECT_DOUBLE_EQ(saved.project.pointMasses.front().mass, 250.25);
}

TEST(Workspace, CreatesEditsRepeatsAndReopensVectorTracks)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    motrack::MainWindow window;
    window.show();
    auto* add = window.findChild<QAction*>("newVector");
    ASSERT_TRUE(add);
    EXPECT_FALSE(add->isEnabled());
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    auto* canvas = static_cast<motrack::VideoCanvas*>(window.findChild<QWidget*>("videoCanvas"));
    auto* picker = window.findChild<QComboBox*>("trackPicker");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    auto* undo = window.findChild<QAction*>("undoEdit");
    auto* redo = window.findChild<QAction*>("redoEdit");
    auto* remove = window.findChild<QAction*>("removeMark");
    ASSERT_TRUE(canvas && picker && slider && undo && redo && remove);
    add->trigger();
    EXPECT_EQ(picker->currentText(), "Vector 1 (vector)");
    EXPECT_FALSE(window.findChild<QAction*>("autoTracker")->isEnabled());
    EXPECT_FALSE(window.findChild<QAction*>("setPointMassValue")->isEnabled());
    EXPECT_FALSE(window.findChild<QAction*>("exportPlotData")->isEnabled());
    EXPECT_FALSE(window.findChild<QComboBox*>("plotQuantity")->isEnabled());
    ASSERT_TRUE(canvas->pick({1, 2}, true));
    EXPECT_EQ(slider->value(), 0);
    ASSERT_TRUE(canvas->pick({3, 1}, true));
    EXPECT_EQ(slider->value(), 1);
    auto* repeat = window.findChild<QAction*>("repeatMark");
    repeat->trigger();
    const auto path = temporary.filePath("edited-vectors.motrack");
    ASSERT_TRUE(window.saveWorkspace(path));
    auto saved = motrack::readWorkspaceDocument(path);
    ASSERT_EQ(saved.project.vectors.size(), 1u);
    const auto& samples = saved.project.vectors.front().samples;
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples.front().frame, 0);
    EXPECT_EQ(samples.back().frame, 2);
    EXPECT_DOUBLE_EQ(samples.front().tailX, 1);
    EXPECT_DOUBLE_EQ(samples.front().tailY, 2);
    EXPECT_DOUBLE_EQ(samples.front().componentX, 2);
    EXPECT_DOUBLE_EQ(samples.front().componentY, -1);
    EXPECT_DOUBLE_EQ(samples.back().componentY, -1);
    // Replacing a vector must be one undoable edit, including both endpoints.
    slider->setValue(0);
    canvas->pick({2, 1}, true);
    canvas->pick({1, 1}, true);
    undo->trigger();
    redo->trigger();
    slider->setValue(0);
    ASSERT_TRUE(remove->isEnabled());
    remove->trigger();
    EXPECT_FALSE(remove->isEnabled());
    undo->trigger();
    QTimer::singleShot(0,
                       &window,
                       []
                       {
                           if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                           {
                               dialog->setTextValue("Force");
                               dialog->accept();
                           }
                       });
    window.findChild<QAction*>("renamePointMass")->trigger();
    EXPECT_EQ(picker->currentText(), "Force (vector)");
    window.findChild<QAction*>("removePointMass")->trigger();
    EXPECT_EQ(picker->count(), 1);
    undo->trigger();
    ASSERT_TRUE(window.saveWorkspace(path));
    ASSERT_TRUE(window.loadProject(path));
    saved = motrack::readWorkspaceDocument(path);
    ASSERT_EQ(saved.project.vectors.size(), 1u);
    EXPECT_EQ(saved.project.vectors.front().name, "Force");
    EXPECT_DOUBLE_EQ(saved.project.vectors.front().samples.front().tailX, 2);
    EXPECT_DOUBLE_EQ(saved.project.vectors.front().samples.front().componentX, -1);
    EXPECT_EQ(saved.project.pointMasses.front().marks.size(), 2u);
    picker->setCurrentIndex(1);
    EXPECT_FALSE(window.findChild<QAction*>("autoTracker")->isEnabled());
    picker->setCurrentIndex(0);
    EXPECT_TRUE(window.findChild<QAction*>("setPointMassValue")->isEnabled());
}

TEST(Workspace, CancelsPartialVectorsOnFrameTrackToolAndEscapeChanges)
{
    QTemporaryDir temporary;
    motrack::MainWindow window;
    window.show();
    window.activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(&window));
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    auto* canvas = static_cast<motrack::VideoCanvas*>(window.findChild<QWidget*>("videoCanvas"));
    auto* picker = window.findChild<QComboBox*>("trackPicker");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    window.findChild<QAction*>("newVector")->trigger();
    const auto path = temporary.filePath("partial.motrack");
    ASSERT_TRUE(window.saveWorkspace(path));
    canvas->pick({1, 1}, true);
    EXPECT_FALSE(window.isWindowModified());
    slider->setValue(1);
    canvas->pick({2, 2}, true);
    EXPECT_EQ(window.statusBar()->currentMessage(), "Shift-click the vector tip. Escape cancels.");
    picker->setCurrentIndex(0);
    picker->setCurrentIndex(1);
    canvas->pick({2, 2}, true);
    window.findChild<QAction*>("calibrationStick")->trigger();
    window.findChild<QAction*>("calibrationStick")->trigger();
    canvas->pick({2, 2}, true);
    window.findChild<QAction*>("measureDistance")->trigger();
    window.findChild<QAction*>("measureDistance")->trigger();
    canvas->pick({2, 2}, true);
    // The stack-owned window stays alive across both events; avoid keyClick's QPointer analysis path.
    QTest::keyPress(&window, Qt::Key_Escape);
    QTest::keyRelease(&window, Qt::Key_Escape);
    EXPECT_TRUE(window.statusBar()->currentMessage().isEmpty());
    ASSERT_TRUE(window.saveWorkspace(path));
    auto saved = motrack::readWorkspaceDocument(path);
    EXPECT_TRUE(saved.project.vectors.front().samples.empty());
    // A real mouse click starts a new vector after Escape; it must not finish the old one.
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::ShiftModifier, canvas->rect().center());
    EXPECT_FALSE(window.isWindowModified());
    window.findChild<QAction*>("undoEdit")->trigger();
    EXPECT_EQ(picker->count(), 1);
    window.findChild<QAction*>("redoEdit")->trigger();
    picker->setCurrentIndex(1);
    canvas->pick({1, 1}, true);
    canvas->pick({2, 2}, true);
    ASSERT_TRUE(window.saveWorkspace(path));
    saved = motrack::readWorkspaceDocument(path);
    ASSERT_EQ(saved.project.vectors.front().samples.size(), 1u);
    EXPECT_EQ(saved.project.vectors.front().samples.front().frame, 2);
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
