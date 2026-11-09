#include "image_sequence.hpp"
#include "main_window.hpp"
#include "paired_save.hpp"
#include "tracker_archive.hpp"
#include "tracker_writer.hpp"
#include "video_canvas.hpp"
#include "workspace_document.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QLocale>
#include <QLockFile>
#include <QMessageBox>
#include <QPushButton>
#include <QSemaphore>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace
{
QString fixture(const QString& name = "linked-images.trk")
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/" + name;
}

QByteArray bytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Could not read paired workspace output.");
    return file.readAll();
}

void put(const QString& path, const QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        throw std::runtime_error("Could not write paired workspace fixture.");
}

bool editMass(motrack::MainWindow& window, double value)
{
    auto* action = window.findChild<QAction*>("setPointMassValue");
    if (!action || !action->isEnabled())
        return false;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog,
                     &QTimer::timeout,
                     &window,
                     []
                     {
                         if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                             dialog->reject();
                     });
    watchdog.start(2000);
    bool accepted = false;
    QTimer::singleShot(0,
                       &window,
                       [&]
                       {
                           if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                           {
                               dialog->setTextValue(QLocale().toString(value));
                               accepted = true;
                               dialog->accept();
                           }
                       });
    action->trigger();
    return accepted;
}

TEST(PairedWorkspace, SavesIndexedMovieEditsAndNativeCompanionAfterSourceRemoval)
{
    QTemporaryDir temporary;
    const auto movieFixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-movie/";
    const auto inputDirectory = temporary.filePath("source");
    const auto mediaDirectory = temporary.filePath("media");
    ASSERT_TRUE(QDir().mkpath(inputDirectory));
    ASSERT_TRUE(QDir().mkpath(mediaDirectory));
    const auto source = inputDirectory + "/input.trk";
    auto xml = bytes(movieFixture + "linked-movie.trk");
    xml.replace(">constant-rate.avi<", ">../media/constant-rate.avi<");
    put(source, xml);
    ASSERT_TRUE(QFile::copy(movieFixture + "constant-rate.avi", mediaDirectory + "/constant-rate.avi"));
    const auto target = temporary.filePath("saved.trk");
    const auto native = temporary.filePath("saved.motrack");
    {
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(source));
        auto* plot = window.findChild<QWidget*>("positionPlot");
        auto* canvas = window.findChild<QWidget*>("videoCanvas");
        auto* slider = window.findChild<QSlider*>("stepSlider");
        ASSERT_TRUE(plot && canvas && slider);
        ASSERT_TRUE(QTest::qWaitFor([plot] { return plot->accessibleName().endsWith("Time (s)"); }));
        ASSERT_TRUE(editMass(window, 7));
        auto* video = dynamic_cast<motrack::VideoCanvas*>(canvas);
        auto* axes = window.findChild<QAction*>("setCoordinateAxes");
        auto* calibrate = window.findChild<QAction*>("calibrationStick");
        auto* undo = window.findChild<QAction*>("undoEdit");
        auto* redo = window.findChild<QAction*>("redoEdit");
        ASSERT_TRUE(video && axes && calibrate && undo && redo);
        axes->trigger();
        ASSERT_TRUE(video->pick({10, 18}, false));
        ASSERT_TRUE(video->pick({10, 10}, false));
        calibrate->trigger();
        ASSERT_TRUE(video->pick({4, 12}, false));
        bool configured = false;
        QTimer::singleShot(0,
                           &window,
                           [&configured]
                           {
                               if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                               {
                                   auto* distance = dialog->findChild<QDoubleSpinBox*>("knownDistance");
                                   auto* unit = dialog->findChild<QLineEdit*>("lengthUnit");
                                   if (distance && unit)
                                   {
                                       distance->setValue(2);
                                       unit->setText("cm");
                                       configured = true;
                                   }
                                   dialog->accept();
                               }
                           });
        ASSERT_TRUE(video->pick({12, 12}, false));
        ASSERT_TRUE(configured);
        EXPECT_TRUE(plot->accessibleName().contains("(cm)"));
        undo->trigger();
        EXPECT_TRUE(plot->accessibleName().contains("(m)"));
        redo->trigger();
        EXPECT_TRUE(plot->accessibleName().contains("(cm)"));
        ASSERT_TRUE(QFile::remove(source));
        ASSERT_TRUE(QDir().rmdir(inputDirectory));
        ASSERT_TRUE(window.saveTrackerProject(target));
        EXPECT_FALSE(window.isWindowModified());
        const auto companion = motrack::readWorkspaceDocument(native);
        EXPECT_TRUE(companion.movie && companion.alignMovieToClip);
        EXPECT_EQ(companion.movieFrameTimes, (std::vector<double>{0.0, 0.125, 0.25, 0.375}));
        EXPECT_DOUBLE_EQ(companion.project.pointMasses.front().mass, 7);
        EXPECT_EQ(companion.project.lengthUnit, "cm");
        if (!companion.project.calibration)
            FAIL() << "Paired save lost the calibrated movie.";
        ASSERT_EQ(companion.project.calibration->keyframes.size(), 1);
        const auto& key = companion.project.calibration->keyframes.front();
        EXPECT_DOUBLE_EQ(key.originX, 10);
        EXPECT_DOUBLE_EQ(key.originY, 18);
        EXPECT_DOUBLE_EQ(key.angleDegrees, 90);
        EXPECT_DOUBLE_EQ(key.scaleX, 4);
        EXPECT_DOUBLE_EQ(key.scaleY, 8);
        EXPECT_EQ(motrack::importTrackerDocument(target).project.calibration, companion.project.calibration);
        EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.front().mass, 7);
        slider->setValue(2);
        ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 2"; }));
    }
    motrack::MainWindow reopened;
    ASSERT_TRUE(reopened.loadProject(native));
    auto* canvas = reopened.findChild<QWidget*>("videoCanvas");
    ASSERT_NE(canvas, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 0"; }));
    ASSERT_TRUE(editMass(reopened, 9));
    ASSERT_TRUE(reopened.saveTrackerProject(target));
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(native).project.pointMasses.front().mass, 9);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.front().mass, 9);
    EXPECT_EQ(motrack::importTrackerDocument(target).project.lengthUnit, "cm");
    EXPECT_EQ(motrack::importTrackerDocument(target).project.calibration, motrack::readWorkspaceDocument(native).project.calibration);
}

TEST(PairedWorkspace, SavesImageAndMovieVectorAuthoringThroughDesktopHistory)
{
    for (const bool movie : {false, true})
    {
        SCOPED_TRACE(movie);
        QTemporaryDir temporary;
        const auto directory = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/vector-" + (movie ? "movie/" : "images/");
        const auto input = directory + (movie ? "linked-movie.trk" : "linked-images.trk");
        const auto target = temporary.filePath("saved.trk");
        const auto native = temporary.filePath("saved.motrack");
        {
            motrack::MainWindow window;
            ASSERT_TRUE(window.loadProject(input));
            auto* canvas = dynamic_cast<motrack::VideoCanvas*>(window.findChild<QWidget*>("videoCanvas"));
            auto* create = window.findChild<QAction*>("newVector");
            auto* rename = window.findChild<QAction*>("renamePointMass");
            auto* remove = window.findChild<QAction*>("removePointMass");
            auto* undo = window.findChild<QAction*>("undoEdit");
            auto* redo = window.findChild<QAction*>("redoEdit");
            auto* exportArchive = window.findChild<QAction*>("exportTrackerArchive");
            ASSERT_TRUE(canvas && create && rename && remove && undo && redo && exportArchive);
            ASSERT_TRUE(
                QTest::qWaitFor([canvas, exportArchive] { return canvas->accessibleName() == "Frame 0" && exportArchive->isEnabled(); }));
            create->trigger();
            bool named = false;
            QTimer::singleShot(0,
                               &window,
                               [&]
                               {
                                   if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                                   {
                                       dialog->setTextValue(QString::fromUtf8("media vector β"));
                                       named = true;
                                       dialog->accept();
                                   }
                               });
            rename->trigger();
            ASSERT_TRUE(named);
            ASSERT_TRUE(canvas->pick({0, 0}, true));
            ASSERT_TRUE(canvas->pick({4, 4}, true));
            remove->trigger();
            undo->trigger();
            ASSERT_TRUE(window.saveTrackerProject(target));
            const auto saved = motrack::importTrackerDocument(target).project;
            ASSERT_EQ(saved.vectors.size(), 4);
            EXPECT_EQ(saved.vectors.back().name, QString::fromUtf8("media vector β").toStdString());
            EXPECT_EQ(saved.vectors.back().samples,
                      (std::vector<motrack::VectorSample>{{.frame = 0, .tailX = 0, .tailY = 0, .componentX = 4, .componentY = 4}}));
            EXPECT_EQ(motrack::readWorkspaceDocument(native).project.vectors, saved.vectors);
            redo->trigger();
            ASSERT_TRUE(window.saveTrackerProject(target));
            EXPECT_EQ(motrack::importTrackerDocument(target).project.vectors.size(), 3);
            undo->trigger();
            ASSERT_TRUE(window.saveTrackerProject(target));
            EXPECT_FALSE(window.isWindowModified());
        }
        motrack::MainWindow reopened;
        ASSERT_TRUE(reopened.loadProject(native));
        auto* exportArchive = reopened.findChild<QAction*>("exportTrackerArchive");
        ASSERT_NE(exportArchive, nullptr);
        ASSERT_TRUE(QTest::qWaitFor([exportArchive] { return exportArchive->isEnabled(); }));
        ASSERT_TRUE(reopened.saveTrackerProject(target));
        EXPECT_EQ(motrack::importTrackerDocument(target).project.vectors.size(), 4);
    }
}

TEST(PairedWorkspace, SavesMovieTrackCreationRenameAndDeletionThroughDesktopHistory)
{
    QTemporaryDir temporary;
    const auto directory = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/movie-tracks/";
    const auto input = temporary.filePath("input.trk");
    ASSERT_TRUE(QFile::copy(directory + "linked-movie.trk", input));
    ASSERT_TRUE(QFile::copy(directory + "constant-rate.mov", temporary.filePath("constant-rate.mov")));
    const auto target = temporary.filePath("saved.trk");
    const auto native = temporary.filePath("saved.motrack");
    {
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(input));
        auto* plot = window.findChild<QWidget*>("positionPlot");
        auto* canvas = dynamic_cast<motrack::VideoCanvas*>(window.findChild<QWidget*>("videoCanvas"));
        auto* create = window.findChild<QAction*>("newPointMass");
        auto* rename = window.findChild<QAction*>("renamePointMass");
        auto* remove = window.findChild<QAction*>("removePointMass");
        auto* undo = window.findChild<QAction*>("undoEdit");
        auto* redo = window.findChild<QAction*>("redoEdit");
        ASSERT_TRUE(plot && canvas && create && rename && remove && undo && redo);
        ASSERT_TRUE(QTest::qWaitFor([plot] { return plot->accessibleName().endsWith("Time (s)"); }));
        create->trigger();
        bool named = false;
        QTimer::singleShot(0,
                           &window,
                           [&]
                           {
                               if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                               {
                                   dialog->setTextValue(QString::fromUtf8("new movie β"));
                                   named = true;
                                   dialog->accept();
                               }
                           });
        rename->trigger();
        ASSERT_TRUE(named);
        ASSERT_TRUE(editMass(window, 4));
        ASSERT_TRUE(canvas->pick({8, 4}, true));
        remove->trigger();
        undo->trigger();
        ASSERT_TRUE(QFile::remove(input));
        ASSERT_TRUE(window.saveTrackerProject(target));
        const auto saved = motrack::importTrackerDocument(target).project;
        ASSERT_EQ(saved.pointMasses.size(), 3);
        EXPECT_EQ(saved.pointMasses.back().name, QString::fromUtf8("new movie β").toStdString());
        EXPECT_EQ(saved.pointMasses.back().mass, 4);
        EXPECT_EQ(saved.pointMasses.back().marks, (std::vector<motrack::Mark>{{.frame = 0, .x = 8, .y = 4}}));
        EXPECT_EQ(motrack::readWorkspaceDocument(native).project.pointMasses.back(), saved.pointMasses.back());
        redo->trigger();
        ASSERT_TRUE(window.saveTrackerProject(target));
        EXPECT_EQ(motrack::importTrackerDocument(target).project.pointMasses.size(), 2);
        undo->trigger();
        ASSERT_TRUE(window.saveTrackerProject(target));
        EXPECT_FALSE(window.isWindowModified());
    }
    motrack::MainWindow reopened;
    ASSERT_TRUE(reopened.loadProject(native));
    auto* plot = reopened.findChild<QWidget*>("positionPlot");
    ASSERT_NE(plot, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([plot] { return plot->accessibleName().endsWith("Time (s)"); }));
    ASSERT_TRUE(reopened.saveTrackerProject(target));
    EXPECT_EQ(motrack::importTrackerDocument(target).project.pointMasses.size(), 3);
    EXPECT_EQ(bytes(temporary.filePath("constant-rate.mov")), bytes(directory + "constant-rate.mov"));
}

TEST(PairedWorkspace, SavesMovingMovieCalibrationAtOriginalFramesWithUndoRedo)
{
    QTemporaryDir temporary;
    const auto directory = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/keyed-movie-0/";
    const auto source = temporary.filePath("input.trk");
    const auto target = temporary.filePath("saved.trk");
    const auto native = temporary.filePath("saved.motrack");
    ASSERT_TRUE(QFile::copy(directory + "linked-movie.trk", source));
    ASSERT_TRUE(QFile::copy(directory + "constant-rate.mov", temporary.filePath("constant-rate.mov")));
    {
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(source));
        auto* plot = window.findChild<QWidget*>("positionPlot");
        auto* video = dynamic_cast<motrack::VideoCanvas*>(window.findChild<QWidget*>("videoCanvas"));
        auto* slider = window.findChild<QSlider*>("stepSlider");
        auto* axes = window.findChild<QAction*>("setCoordinateAxes");
        auto* distance = window.findChild<QAction*>("calibrationStick");
        auto* undo = window.findChild<QAction*>("undoEdit");
        auto* redo = window.findChild<QAction*>("redoEdit");
        ASSERT_TRUE(plot && video && slider && axes && distance && undo && redo);
        ASSERT_TRUE(QTest::qWaitFor([plot] { return plot->accessibleName().endsWith("Time (s)"); }));
        slider->setValue(2);
        ASSERT_TRUE(QTest::qWaitFor([video] { return video->accessibleName() == "Frame 2"; }));
        distance->trigger();
        ASSERT_TRUE(video->pick({4, 12}, false));
        bool configured = false;
        QTimer::singleShot(0,
                           &window,
                           [&configured]
                           {
                               if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                               {
                                   auto* length = dialog->findChild<QDoubleSpinBox*>("knownDistance");
                                   auto* unit = dialog->findChild<QLineEdit*>("lengthUnit");
                                   if (length && unit)
                                   {
                                       length->setValue(1);
                                       unit->setText("cm");
                                       configured = true;
                                   }
                                   dialog->accept();
                               }
                           });
        ASSERT_TRUE(video->pick({12, 12}, false));
        ASSERT_TRUE(configured);
        axes->trigger();
        ASSERT_TRUE(video->pick({12, 16}, false));
        ASSERT_TRUE(video->pick({20, 16}, false));
        slider->setValue(1);
        ASSERT_TRUE(QTest::qWaitFor([video] { return video->accessibleName() == "Frame 1"; }));
        axes->trigger();
        ASSERT_TRUE(video->pick({6, 14}, false));
        ASSERT_TRUE(video->pick({6, 22}, false));
        EXPECT_TRUE(plot->accessibleName().contains("(cm)"));
        ASSERT_TRUE(QFile::remove(source));
        undo->trigger();
        ASSERT_TRUE(window.saveTrackerProject(target));
        const auto undone = motrack::readWorkspaceDocument(native);
        if (!undone.project.calibration)
            FAIL() << "Undo save lost movie calibration.";
        EXPECT_EQ(undone.project.calibration->keyframes.size(), 2);
        ASSERT_TRUE(redo->isEnabled());
        redo->trigger();
        ASSERT_TRUE(window.saveTrackerProject(target));
    }
    const auto saved = motrack::readWorkspaceDocument(native);
    EXPECT_EQ(saved.project.lengthUnit, "cm");
    if (!saved.project.calibration)
        FAIL() << "Moving movie calibration was not saved.";
    EXPECT_FALSE(saved.project.calibration->fixedOrigin);
    EXPECT_FALSE(saved.project.calibration->fixedAngle);
    EXPECT_FALSE(saved.project.calibration->fixedScale);
    const auto& keys = saved.project.calibration->keyframes;
    ASSERT_EQ(keys.size(), 3);
    EXPECT_EQ(keys[0].frame, 0);
    EXPECT_EQ(keys[1].frame, 1);
    EXPECT_DOUBLE_EQ(keys[1].originX, 6);
    EXPECT_DOUBLE_EQ(keys[1].originY, 14);
    EXPECT_DOUBLE_EQ(keys[1].angleDegrees, -90);
    EXPECT_DOUBLE_EQ(keys[1].scaleX, 2);
    EXPECT_DOUBLE_EQ(keys[1].scaleY, 4);
    EXPECT_EQ(keys[2].frame, 2);
    EXPECT_DOUBLE_EQ(keys[2].originX, 12);
    EXPECT_DOUBLE_EQ(keys[2].originY, 16);
    EXPECT_DOUBLE_EQ(keys[2].angleDegrees, 0);
    EXPECT_DOUBLE_EQ(keys[2].scaleX, 8);
    EXPECT_DOUBLE_EQ(keys[2].scaleY, 16);
    EXPECT_EQ(motrack::importTrackerDocument(target).project.calibration, saved.project.calibration);
    motrack::MainWindow reopened;
    ASSERT_TRUE(reopened.loadProject(native));
    auto* plot = reopened.findChild<QWidget*>("positionPlot");
    ASSERT_TRUE(plot);
    ASSERT_TRUE(QTest::qWaitFor([plot] { return plot->accessibleName().endsWith("Time (s)"); }));
    ASSERT_TRUE(reopened.saveTrackerProject(target));
    EXPECT_EQ(motrack::importTrackerDocument(target).project.calibration, saved.project.calibration);
}

TEST(PairedWorkspace, SavesMovingVectorAxesThroughDesktopHistoryAndNativeReopen)
{
    for (const bool movie : {false, true})
    {
        SCOPED_TRACE(movie);
        QTemporaryDir temporary;
        const auto kind = movie ? "movie" : "images";
        const auto directory = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/keyed-vector-" + kind + "-0/";
        const auto input = temporary.filePath("input.trk");
        ASSERT_TRUE(QFile::copy(directory + "linked-" + kind + ".trk", input));
        if (movie)
            ASSERT_TRUE(QFile::copy(directory + "constant-rate.mov", temporary.filePath("constant-rate.mov")));
        else
        {
            ASSERT_TRUE(QDir().mkdir(temporary.filePath("images")));
            for (int frame = 0; frame < 4; ++frame)
            {
                const auto image = QString("images/frame%1.png").arg(frame);
                ASSERT_TRUE(QFile::copy(directory + image, temporary.filePath(image)));
            }
        }
        const auto target = temporary.filePath("saved.trk");
        const auto native = temporary.filePath("saved.motrack");
        const auto vectors = motrack::importTrackerDocument(input).project.vectors;
        {
            motrack::MainWindow window;
            ASSERT_TRUE(window.loadProject(input));
            auto* video = dynamic_cast<motrack::VideoCanvas*>(window.findChild<QWidget*>("videoCanvas"));
            auto* slider = window.findChild<QSlider*>("stepSlider");
            auto* axes = window.findChild<QAction*>("setCoordinateAxes");
            auto* undo = window.findChild<QAction*>("undoEdit");
            auto* redo = window.findChild<QAction*>("redoEdit");
            auto* exportArchive = window.findChild<QAction*>("exportTrackerArchive");
            ASSERT_TRUE(video && slider && axes && undo && redo && exportArchive);
            ASSERT_TRUE(QTest::qWaitFor([exportArchive] { return exportArchive->isEnabled(); }));
            slider->setValue(2);
            ASSERT_TRUE(QTest::qWaitFor([video] { return video->accessibleName() == "Frame 2"; }));
            axes->trigger();
            ASSERT_TRUE(video->pick({12, 16}, false));
            ASSERT_TRUE(video->pick({20, 16}, false));
            slider->setValue(1);
            ASSERT_TRUE(QTest::qWaitFor([video] { return video->accessibleName() == "Frame 1"; }));
            axes->trigger();
            ASSERT_TRUE(video->pick({6, 14}, false));
            ASSERT_TRUE(video->pick({6, 22}, false));
            ASSERT_TRUE(QFile::remove(input));
            undo->trigger();
            ASSERT_TRUE(window.saveTrackerProject(target));
            const auto undone = motrack::readWorkspaceDocument(native);
            ASSERT_TRUE(undone.project.calibration);
            EXPECT_EQ(undone.project.calibration->keyframes.size(), 2);
            ASSERT_TRUE(redo->isEnabled());
            redo->trigger();
            ASSERT_TRUE(window.saveTrackerProject(target));
            EXPECT_FALSE(window.isWindowModified());
        }
        const auto saved = motrack::readWorkspaceDocument(native);
        ASSERT_TRUE(saved.project.calibration);
        ASSERT_EQ(saved.project.calibration->keyframes.size(), 3);
        const auto& keys = saved.project.calibration->keyframes;
        EXPECT_EQ(keys[1].frame, 1);
        EXPECT_DOUBLE_EQ(keys[1].originX, 6);
        EXPECT_DOUBLE_EQ(keys[1].originY, 14);
        EXPECT_DOUBLE_EQ(keys[1].angleDegrees, -90);
        EXPECT_EQ(keys[2].frame, 2);
        EXPECT_DOUBLE_EQ(keys[2].originX, 12);
        EXPECT_DOUBLE_EQ(keys[2].originY, 16);
        EXPECT_DOUBLE_EQ(keys[2].angleDegrees, 0);
        EXPECT_EQ(saved.project.vectors, vectors);
        EXPECT_EQ(motrack::importTrackerDocument(target).project.calibration, saved.project.calibration);
        motrack::MainWindow reopened;
        ASSERT_TRUE(reopened.loadProject(native));
        auto* exportArchive = reopened.findChild<QAction*>("exportTrackerArchive");
        ASSERT_TRUE(exportArchive);
        ASSERT_TRUE(QTest::qWaitFor([exportArchive] { return exportArchive->isEnabled(); }));
        ASSERT_TRUE(reopened.saveTrackerProject(target));
        EXPECT_EQ(motrack::importTrackerDocument(target).project.vectors, vectors);
    }
}

TEST(PairedWorkspace, SavesArchivedMovieClipsAndBothCopiesAfterOriginalRemoval)
{
    QTemporaryDir temporary;
    const auto fixturePath = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-mov/archived-movie.trz";
    const auto source = temporary.filePath("original.trz");
    const auto standard = temporary.filePath("saved.trz");
    const auto native = temporary.filePath("saved.motrack");
    ASSERT_TRUE(QFile::copy(fixturePath, source));
    const auto original = motrack::TrackerArchive::open(fixturePath);
    motrack::MainWindow window;
    auto openArchive = [&](const QString& path)
    {
        bool selected = false;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog,
                         &QTimer::timeout,
                         &window,
                         []
                         {
                             if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                                 dialog->reject();
                         });
        watchdog.start(2000);
        QTimer::singleShot(0,
                           &watchdog,
                           [&selected]
                           {
                               if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                               {
                                   dialog->setTextValue("experiment.trk");
                                   selected = true;
                                   dialog->accept();
                               }
                           });
        const bool opened = window.loadProject(path);
        return opened && selected;
    };
    ASSERT_TRUE(openArchive(source));
    auto* plot = window.findChild<QWidget*>("positionPlot");
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    auto* settings = window.findChild<QAction*>("clipSettings");
    auto* save = window.findChild<QAction*>("saveProject");
    auto* undo = window.findChild<QAction*>("undoEdit");
    auto* redo = window.findChild<QAction*>("redoEdit");
    ASSERT_TRUE(plot && canvas && slider && settings && save && undo && redo);
    ASSERT_TRUE(QTest::qWaitFor([plot] { return plot->accessibleName().endsWith("Time (s)"); }));
    ASSERT_TRUE(editMass(window, 7));
    bool accepted = false;
    QTimer::singleShot(0,
                       &window,
                       [&accepted]
                       {
                           if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                           {
                               auto* first = dialog->findChild<QSpinBox*>("clipStart");
                               auto* stride = dialog->findChild<QSpinBox*>("clipStride");
                               if (!first || !stride)
                               {
                                   dialog->reject();
                                   return;
                               }
                               first->setValue(1);
                               stride->setValue(2);
                               accepted = true;
                               dialog->accept();
                           }
                       });
    settings->trigger();
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(window.saveTrackerProject(standard));
    EXPECT_FALSE(window.isWindowModified());
    ASSERT_TRUE(QFile::remove(source));
    undo->trigger();
    save->trigger();
    EXPECT_FALSE(window.isWindowModified());
    EXPECT_EQ(motrack::readWorkspaceDocument(native).project.clip.firstFrame, 0);
    redo->trigger();
    save->trigger();
    EXPECT_FALSE(window.isWindowModified());
    const auto companion = motrack::readWorkspaceDocument(native);
    EXPECT_TRUE(companion.movie && companion.alignMovieToClip);
    EXPECT_EQ(companion.movieFrameTimes, (std::vector<double>{0.0, 0.125, 0.25, 0.375}));
    EXPECT_EQ(companion.project.clip.firstFrame, 1);
    EXPECT_EQ(companion.project.clip.stepSize, 2);
    EXPECT_EQ(companion.project.clip.stepCount, 2);
    ASSERT_TRUE(companion.archivePayload);
    EXPECT_EQ(bytes(companion.archivePayload->sourcePath()), bytes(fixturePath));
    ASSERT_TRUE(window.loadProject(native));
    ASSERT_TRUE(editMass(window, 9));
    ASSERT_TRUE(window.saveTrackerProject(standard));
    ASSERT_TRUE(openArchive(standard));
    ASSERT_TRUE(QTest::qWaitFor([plot] { return plot->accessibleName().endsWith("Time (s)"); }));
    slider->setValue(1);
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 3"; }));
    const auto exported = motrack::TrackerArchive::open(standard);
    const auto project = motrack::importTrackerDocument(exported.projectPath("experiment.trk")).project;
    EXPECT_DOUBLE_EQ(project.pointMasses.at(0).mass, 9);
    EXPECT_EQ(project.clip.firstFrame, 1);
    EXPECT_EQ(project.clip.stepSize, 2);
    EXPECT_EQ(project.clip.stepCount, 2);
    for (const auto& member : QStringList{"second.trk",
                                          "constant-rate.mov",
                                          "images/frame0.png",
                                          "images/frame1.png",
                                          "images/frame2.png",
                                          "images/frame3.png",
                                          QString::fromUtf8("support/notes é.txt"),
                                          "support/opaque.bin"})
        EXPECT_EQ(bytes(QDir(exported.root()).filePath(member)), bytes(QDir(original.root()).filePath(member)));
}

TEST(PairedWorkspace, ReopenedSingleFrameMovieCanExpandToTheCompleteIndexedSource)
{
    QTemporaryDir temporary;
    const auto source = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-movie/linked-movie.trk";
    const auto target = temporary.filePath("single.trk");
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(source));
    auto* plot = window.findChild<QWidget*>("positionPlot");
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    auto* settings = window.findChild<QAction*>("clipSettings");
    ASSERT_TRUE(plot && canvas && slider && settings);
    for (const bool expand : {false, true})
    {
        SCOPED_TRACE(expand);
        ASSERT_TRUE(QTest::qWaitFor([plot] { return plot->accessibleName().endsWith("Time (s)"); }));
        EXPECT_FALSE(window.isWindowModified());
        int maximum = -1;
        bool accepted = false;
        QTimer::singleShot(0,
                           &window,
                           [&]
                           {
                               if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                               {
                                   auto* first = dialog->findChild<QSpinBox*>("clipStart");
                                   auto* last = dialog->findChild<QSpinBox*>("clipEnd");
                                   auto* stride = dialog->findChild<QSpinBox*>("clipStride");
                                   if (!first || !last || !stride)
                                   {
                                       dialog->reject();
                                       return;
                                   }
                                   maximum = last->maximum();
                                   first->setValue(expand ? 0 : 2);
                                   last->setValue(expand ? 3 : 2);
                                   stride->setValue(1);
                                   accepted = true;
                                   dialog->accept();
                               }
                           });
        settings->trigger();
        ASSERT_TRUE(accepted);
        ASSERT_EQ(maximum, 3);
        ASSERT_TRUE(window.saveTrackerProject(target));
        const auto saved = motrack::readWorkspaceDocument(temporary.filePath("single.motrack"));
        EXPECT_EQ(saved.project.clip.sourceFrameCount, 4);
        EXPECT_EQ(saved.project.clip.firstFrame, expand ? 0 : 2);
        EXPECT_EQ(saved.project.clip.stepCount, expand ? 4 : 1);
        EXPECT_EQ(saved.project.pointMasses, motrack::importTrackerDocument(source).project.pointMasses);
        if (!expand)
            ASSERT_TRUE(window.loadProject(target));
    }
    slider->setValue(3);
    ASSERT_TRUE(QTest::qWaitFor([canvas] { return canvas->accessibleName() == "Frame 3"; }));
    EXPECT_EQ(motrack::importTrackerDocument(target).project.clip.stepCount, 4);
}

TEST(PairedWorkspace, ShorterLinkedMovieKeepsSavedClipAndMeasurementsReadable)
{
    QTemporaryDir temporary;
    const auto movieFixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-movie/";
    const auto source = temporary.filePath("longer-clip.trk");
    const auto native = temporary.filePath("retained.motrack");
    // The recorded clip spans five frames, but the available replacement movie has only four.
    auto xml = bytes(movieFixture + "linked-movie.trk");
    xml.replace(R"(name="stepcount" type="int">4<)", R"(name="stepcount" type="int">5<)");
    xml.replace(R"(name="[2]" type="object")", R"(name="[4]" type="object")");
    put(source, xml);
    ASSERT_TRUE(QFile::copy(movieFixture + "constant-rate.avi", temporary.filePath("constant-rate.avi")));
    const auto original = motrack::importTrackerDocument(source).project;
    ASSERT_EQ(original.clip.stepCount, 5);
    ASSERT_EQ(original.pointMasses.at(0).marks.back().frame, 4);
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(source));
    auto* plot = window.findChild<QWidget*>("positionPlot");
    auto* slider = window.findChild<QSlider*>("stepSlider");
    ASSERT_TRUE(plot && slider);
    ASSERT_TRUE(QTest::qWaitFor([plot] { return plot->accessibleName().endsWith("Time (s)"); }));
    EXPECT_FALSE(window.isWindowModified());
    ASSERT_TRUE(window.saveWorkspace(native));
    const auto saved = motrack::readWorkspaceDocument(native);
    EXPECT_EQ(saved.project.clip.sourceFrameCount, 5);
    EXPECT_EQ(saved.project.clip.firstFrame, 0);
    EXPECT_EQ(saved.project.clip.stepSize, 1);
    EXPECT_EQ(saved.project.clip.stepCount, 5);
    EXPECT_EQ(saved.project.pointMasses, original.pointMasses);
    EXPECT_EQ(saved.movieFrameTimes, (std::vector<double>{0.0, 0.125, 0.25, 0.375}));
    const auto standard = temporary.filePath("unchanged.trk");
    put(standard, "existing output");
    EXPECT_FALSE(window.saveTrackerProject(standard));
    EXPECT_EQ(bytes(standard), "existing output");
    ASSERT_TRUE(window.loadProject(native));
    EXPECT_EQ(slider->maximum(), 4);
    EXPECT_FALSE(window.isWindowModified());
    auto trimmed = saved;
    trimmed.project.clip.stepCount = 3;
    trimmed.movieFrameTimes.clear();
    const auto trimmedPath = temporary.filePath("trimmed.motrack");
    motrack::writeWorkspaceDocument(trimmedPath, trimmed);
    ASSERT_TRUE(window.loadProject(trimmedPath));
    ASSERT_TRUE(QTest::qWaitFor([plot] { return plot->accessibleName().endsWith("Time (s)"); }));
    ASSERT_TRUE(window.saveWorkspace(trimmedPath));
    const auto retrimmed = motrack::readWorkspaceDocument(trimmedPath);
    EXPECT_EQ(retrimmed.project.clip.sourceFrameCount, 5);
    EXPECT_EQ(retrimmed.project.pointMasses, original.pointMasses);
}

TEST(PairedWorkspace, SavesEditedLinkedAndArchivedProjectsAndUsesThePairForOrdinarySave)
{
    for (const bool archive : {false, true})
    {
        SCOPED_TRACE(archive);
        QTemporaryDir temporary;
        motrack::MainWindow window;
        auto* paired = window.findChild<QAction*>("saveTrackerProject");
        ASSERT_NE(paired, nullptr);
        EXPECT_FALSE(paired->isEnabled());
        EXPECT_FALSE(window.saveTrackerProject(temporary.filePath("empty.trk")));
        ASSERT_TRUE(window.loadProject(fixture(archive ? "archived-images.trz" : "linked-images.trk")));
        EXPECT_TRUE(paired->isEnabled());
        ASSERT_TRUE(editMass(window, 5));
        const auto target = temporary.filePath(archive ? "saved.trz" : "saved.trk");
        ASSERT_TRUE(window.saveTrackerProject(target));
        EXPECT_FALSE(window.isWindowModified());
        const auto nativePath = temporary.filePath("saved.motrack");
        const auto native = motrack::readWorkspaceDocument(nativePath);
        EXPECT_DOUBLE_EQ(native.project.pointMasses.at(0).mass, 5);
        if (archive)
        {
            const auto standard = motrack::TrackerArchive::open(target);
            EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(standard.projectPath("experiment.trk")).project.pointMasses.at(0).mass, 5);
            ASSERT_TRUE(native.archivePayload);
            EXPECT_EQ(bytes(native.archivePayload->sourcePath()), bytes(fixture("archived-images.trz")));
        }
        else
        {
            EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 5);
            const auto sequence = motrack::ImageSequence::open(nativePath, native.project.clip);
            if (!sequence)
                FAIL() << "The native companion lost its image sequence.";
            const auto frame = sequence->readStep(0);
            if (!frame)
                FAIL() << "The native companion lost its first frame.";
            EXPECT_EQ(frame->image, QImage(fixture("images/frame0.png")));
        }
        ASSERT_TRUE(editMass(window, 7));
        auto* save = window.findChild<QAction*>("saveProject");
        ASSERT_NE(save, nullptr);
        save->trigger();
        EXPECT_FALSE(window.isWindowModified());
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(nativePath).project.pointMasses.at(0).mass, 7);
        ASSERT_TRUE(window.loadProject(nativePath));
        auto* canvas = window.findChild<QWidget*>("videoCanvas");
        ASSERT_NE(canvas, nullptr);
        EXPECT_EQ(canvas->accessibleName(), "Frame 0");
    }
}

TEST(PairedWorkspace, SavesMediaClipSettingsAndReopensTheSameSourceFramesThroughUndoRedo)
{
    for (const bool movie : {false, true})
    {
        SCOPED_TRACE(movie);
        const auto source = movie ? QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-movie/linked-movie.trk" : fixture();
        QTemporaryDir temporary;
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(source));
        if (movie)
        {
            auto* plot = window.findChild<QWidget*>("positionPlot");
            ASSERT_NE(plot, nullptr);
            ASSERT_TRUE(QTest::qWaitFor([plot] { return plot->accessibleName().endsWith("Time (s)"); }));
        }
        auto* settings = window.findChild<QAction*>("clipSettings");
        auto* undo = window.findChild<QAction*>("undoEdit");
        auto* redo = window.findChild<QAction*>("redoEdit");
        auto* save = window.findChild<QAction*>("saveProject");
        auto* slider = window.findChild<QSlider*>("stepSlider");
        ASSERT_TRUE(settings && undo && redo && save && slider);
        ASSERT_TRUE(settings->isEnabled());
        bool accepted = false;
        QTimer::singleShot(0,
                           &window,
                           [&accepted]
                           {
                               if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                               {
                                   auto* first = dialog->findChild<QSpinBox*>("clipStart");
                                   auto* stride = dialog->findChild<QSpinBox*>("clipStride");
                                   if (!first || !stride)
                                   {
                                       dialog->reject();
                                       return;
                                   }
                                   first->setValue(1);
                                   stride->setValue(2);
                                   accepted = true;
                                   dialog->accept();
                               }
                           });
        settings->trigger();
        ASSERT_TRUE(accepted);
        ASSERT_TRUE(window.isWindowModified());
        EXPECT_EQ(slider->maximum(), 1);
        const auto standard = temporary.filePath("clip.trk");
        const auto native = temporary.filePath("clip.motrack");
        ASSERT_TRUE(window.saveTrackerProject(standard));
        EXPECT_FALSE(window.isWindowModified());
        const auto trimmed = motrack::importTrackerDocument(standard).project;
        EXPECT_EQ(trimmed.clip.firstFrame, 1);
        EXPECT_EQ(trimmed.clip.stepSize, 2);
        EXPECT_EQ(trimmed.clip.stepCount, 2);
        EXPECT_EQ(trimmed.pointMasses, motrack::importTrackerDocument(source).project.pointMasses);
        undo->trigger();
        EXPECT_TRUE(window.isWindowModified());
        save->trigger();
        EXPECT_FALSE(window.isWindowModified());
        EXPECT_EQ(motrack::importTrackerDocument(standard).project.clip.firstFrame, 0);
        redo->trigger();
        save->trigger();
        EXPECT_FALSE(window.isWindowModified());
        EXPECT_EQ(motrack::importTrackerDocument(standard).project, trimmed);
        ASSERT_TRUE(window.loadProject(native));
        EXPECT_EQ(slider->maximum(), 1);
        const auto reopened = motrack::readWorkspaceDocument(native);
        if (movie)
        {
            EXPECT_EQ(reopened.movieFrameTimes, (std::vector<double>{0.0, 0.125, 0.25, 0.375}));
            auto* canvas = window.findChild<QWidget*>("videoCanvas");
            ASSERT_NE(canvas, nullptr);
            for (int step = 0; step < 2; ++step)
            {
                slider->setValue(step);
                ASSERT_TRUE(
                    QTest::qWaitFor([canvas, step] { return canvas->accessibleName() == "Frame " + QString::number(1 + step * 2); }));
            }
        }
        else
        {
            const auto sequence = motrack::ImageSequence::open(native, reopened.project.clip);
            if (!sequence)
                FAIL() << "The saved clip lost its image sequence.";
            const auto& savedSequence = sequence.value();
            for (int step = 0; step < 2; ++step)
            {
                const auto frame = savedSequence.readStep(step);
                if (!frame)
                    FAIL() << "The saved clip lost a source frame.";
                EXPECT_EQ(frame->image, QImage(fixture("images/frame" + QString::number(1 + step * 2) + ".png")));
            }
        }
    }
}

TEST(PairedWorkspace, SavesLongNativeAndPairedNamesThroughDesktopActions)
{
    for (const auto& stem : {QString(240, 'a'), QString::fromUtf8("é").repeated(120)})
    {
        SCOPED_TRACE(stem.toStdString());
        QTemporaryDir temporary;
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(fixture()));
        ASSERT_TRUE(editMass(window, 5));
        const auto target = temporary.filePath(stem + ".trk");
        const auto native = temporary.filePath(stem + ".motrack");
        ASSERT_TRUE(window.saveWorkspace(native));
        ASSERT_TRUE(window.saveTrackerProject(target));
        EXPECT_FALSE(window.isWindowModified());
        ASSERT_TRUE(editMass(window, 7));
        auto* save = window.findChild<QAction*>("saveProject");
        ASSERT_NE(save, nullptr);
        save->trigger();
        EXPECT_FALSE(window.isWindowModified());
        EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 7);
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(native).project.pointMasses.at(0).mass, 7);
        ASSERT_TRUE(window.loadProject(native));
        auto* canvas = window.findChild<QWidget*>("videoCanvas");
        ASSERT_NE(canvas, nullptr);
        EXPECT_EQ(canvas->accessibleName(), "Frame 0");
    }
}

TEST(PairedWorkspace, NativeCopiesCannotOverwritePendingPairedOutputs)
{
    for (const bool knownPending : {false, true})
    {
        SCOPED_TRACE(knownPending);
        QTemporaryDir temporary;
        const auto target = temporary.filePath("saved.trk");
        auto earlier = motrack::importTrackerDocument(fixture());
        ASSERT_EQ(motrack::writePairedDocuments(target, earlier, fixture()).status, motrack::PairedSaveStatus::Saved);
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setPointMassValue(earlier.project, 0, 5));
        std::atomic_bool cancelled{false};
        const auto partial =
            motrack::writePairedDocuments(target,
                                          earlier,
                                          fixture(),
                                          &cancelled,
                                          [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
        ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
        const auto standardBytes = bytes(target);
        const auto nativeBytes = bytes(partial.nativePath);
        const auto journalBytes = bytes(partial.recoveryPath);
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(fixture()));
        ASSERT_TRUE(editMass(window, 7));
        if (knownPending)
            EXPECT_FALSE(window.saveTrackerProject(target));
        EXPECT_FALSE(window.saveWorkspace(partial.nativePath));
        EXPECT_TRUE(window.statusBar()->currentMessage().contains("Use Save Tracker project"));
        EXPECT_TRUE(window.isWindowModified());
        EXPECT_EQ(bytes(target), standardBytes);
        EXPECT_EQ(bytes(partial.nativePath), nativeBytes);
        EXPECT_EQ(bytes(partial.recoveryPath), journalBytes);
#ifndef Q_OS_WIN
        const auto alias = temporary.filePath("alias.motrack");
        ASSERT_TRUE(QFile::link(partial.nativePath, alias));
        EXPECT_FALSE(window.saveWorkspace(alias));
        EXPECT_EQ(bytes(partial.nativePath), nativeBytes);
#endif
        const auto copy = temporary.filePath("copy.motrack");
        ASSERT_TRUE(window.saveWorkspace(copy));
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(copy).project.pointMasses.at(0).mass, 7);
        EXPECT_EQ(bytes(target), standardBytes);
        EXPECT_EQ(bytes(partial.nativePath), nativeBytes);
        EXPECT_EQ(bytes(partial.recoveryPath), journalBytes);
        if (!knownPending)
            EXPECT_FALSE(window.saveTrackerProject(target));
        ASSERT_TRUE(window.saveTrackerProject(target, true));
        EXPECT_FALSE(QFileInfo::exists(partial.recoveryPath));
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(partial.nativePath).project.pointMasses.at(0).mass, 7);
        EXPECT_FALSE(window.isWindowModified());
    }
}

TEST(PairedWorkspace, NativeCopiesRespectAnActiveSiblingSaveLock)
{
    QTemporaryDir temporary;
    const auto target = temporary.filePath("saved.motrack");
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    ASSERT_TRUE(window.saveWorkspace(target));
    const auto original = bytes(target);
    ASSERT_TRUE(editMass(window, 7));
    QLockFile lock(temporary.filePath(".saved.motrack-save.lock"));
    lock.setStaleLockTime(0);
    ASSERT_TRUE(lock.tryLock(0));
    EXPECT_FALSE(window.saveWorkspace(target));
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Wait for it to finish and retry"));
    EXPECT_EQ(bytes(target), original);
    EXPECT_TRUE(window.isWindowModified());
    ASSERT_TRUE(window.saveWorkspace(temporary.filePath("copy.motrack")));
    lock.unlock();
    ASSERT_TRUE(window.saveWorkspace(target));
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(target).project.pointMasses.at(0).mass, 7);
}

TEST(PairedWorkspace, RecoversAnOlderPairThenSavesCurrentEdits)
{
    QTemporaryDir temporary;
    const auto target = temporary.filePath("saved.trk");
    auto earlier = motrack::importTrackerDocument(fixture());
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(earlier.project, 0, 5));
    std::atomic_bool cancelled{false};
    const auto partial =
        motrack::writePairedDocuments(target,
                                      earlier,
                                      fixture(),
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    ASSERT_TRUE(editMass(window, 7));
    EXPECT_FALSE(window.saveTrackerProject(target));
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_FALSE(window.saveTrackerProject(target)); // Recovery requires an explicit request.
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 5);
    ASSERT_TRUE(window.saveTrackerProject(target, true));
    EXPECT_FALSE(window.isWindowModified());
    EXPECT_FALSE(QFile::exists(partial.recoveryPath));
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 7);
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(partial.nativePath).project.pointMasses.at(0).mass, 7);
}

TEST(PairedWorkspace, ExternalRecoveryChangesRequireReconfirmationBeforeWritingTheCurrentSnapshot)
{
    for (const bool replaced : {false, true})
    {
        SCOPED_TRACE(replaced);
        QTemporaryDir temporary;
        const auto target = temporary.filePath("saved.trk");
        std::atomic_bool cancelled{false};
        const auto interrupt = [&cancelled](auto state)
        {
            cancelled = state == motrack::PairedSaveProgress::StandardSaved;
        };
        auto external = motrack::importTrackerDocument(fixture());
        const auto first = motrack::writePairedDocuments(target, external, fixture(), &cancelled, interrupt);
        ASSERT_EQ(first.status, motrack::PairedSaveStatus::RecoveryRequired);
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(fixture()));
        ASSERT_TRUE(editMass(window, 5));
        EXPECT_FALSE(window.saveTrackerProject(target));
        ASSERT_EQ(motrack::recoverPairedDocuments(first.recoveryPath).status, motrack::PairedSaveStatus::Saved);
        if (replaced)
        {
            motrack::TrackEdits edits;
            ASSERT_TRUE(edits.setPointMassValue(external.project, 0, 7));
            cancelled = false;
            const auto second = motrack::writePairedDocuments(target, external, fixture(), &cancelled, interrupt);
            ASSERT_EQ(second.status, motrack::PairedSaveStatus::RecoveryRequired);
            const auto standard = bytes(target);
            const auto native = bytes(second.nativePath);
            const auto journal = bytes(second.recoveryPath);
            EXPECT_FALSE(window.saveTrackerProject(target, true));
            EXPECT_TRUE(window.isWindowModified());
            EXPECT_EQ(bytes(target), standard);
            EXPECT_EQ(bytes(second.nativePath), native);
            EXPECT_EQ(bytes(second.recoveryPath), journal);
        }
        ASSERT_TRUE(window.saveTrackerProject(target, true));
        EXPECT_FALSE(window.isWindowModified());
        EXPECT_FALSE(QFile::exists(first.recoveryPath));
        EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 5);
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(first.nativePath).project.pointMasses.at(0).mass, 5);
    }
}

TEST(PairedWorkspace, UnidentifiedJournalsRequireConfirmationAfterRepairOrReplacement)
{
    for (int change = 0; change < 3; ++change)
    {
        SCOPED_TRACE(change); // Repaired, replaced, or completed externally.
        QTemporaryDir temporary;
        const auto target = temporary.filePath("saved.trk");
        std::atomic_bool cancelled{false};
        const auto interrupt = [&cancelled](auto state)
        {
            cancelled = state == motrack::PairedSaveProgress::StandardSaved;
        };
        auto external = motrack::importTrackerDocument(fixture());
        const auto first = motrack::writePairedDocuments(target, external, fixture(), &cancelled, interrupt);
        ASSERT_EQ(first.status, motrack::PairedSaveStatus::RecoveryRequired);
        const auto journal = bytes(first.recoveryPath);
        put(first.recoveryPath, "unreadable journal");
        motrack::MainWindow window;
        ASSERT_TRUE(window.loadProject(fixture()));
        ASSERT_TRUE(editMass(window, 5));
        EXPECT_FALSE(window.saveTrackerProject(target));
        put(first.recoveryPath, journal);
        if (change != 0)
            ASSERT_EQ(motrack::recoverPairedDocuments(first.recoveryPath).status, motrack::PairedSaveStatus::Saved);
        if (change == 1)
        {
            motrack::TrackEdits edits;
            ASSERT_TRUE(edits.setPointMassValue(external.project, 0, 7));
            cancelled = false;
            ASSERT_EQ(motrack::writePairedDocuments(target, external, fixture(), &cancelled, interrupt).status,
                      motrack::PairedSaveStatus::RecoveryRequired);
        }
        if (change != 2)
        {
            const auto standard = bytes(target);
            const auto pending = bytes(first.recoveryPath);
            const bool hasNative = QFile::exists(first.nativePath);
            const auto native = hasNative ? bytes(first.nativePath) : QByteArray{};
            EXPECT_FALSE(window.saveTrackerProject(target, true));
            EXPECT_TRUE(window.isWindowModified());
            EXPECT_EQ(bytes(target), standard);
            EXPECT_EQ(bytes(first.recoveryPath), pending);
            EXPECT_EQ(QFile::exists(first.nativePath), hasNative);
            if (hasNative)
                EXPECT_EQ(bytes(first.nativePath), native);
        }
        ASSERT_TRUE(window.saveTrackerProject(target, true));
        EXPECT_FALSE(window.isWindowModified());
        EXPECT_FALSE(QFile::exists(first.recoveryPath));
        EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 5);
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(first.nativePath).project.pointMasses.at(0).mass, 5);
    }
}

TEST(PairedWorkspace, RecoveryConflictsKeepCurrentEditsAndBothDestinations)
{
    QTemporaryDir temporary;
    const auto target = temporary.filePath("saved.trk");
    std::atomic_bool cancelled{false};
    const auto partial =
        motrack::writePairedDocuments(target,
                                      motrack::importTrackerDocument(fixture()),
                                      fixture(),
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
    const auto preparedStandard = bytes(target);
    put(target, "external change");
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    ASSERT_TRUE(editMass(window, 7));
    EXPECT_FALSE(window.saveTrackerProject(target));
    EXPECT_FALSE(window.saveTrackerProject(target, true));
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Could not finish the interrupted save"));
    EXPECT_EQ(bytes(target), "external change");
    EXPECT_FALSE(QFile::exists(partial.nativePath));
    EXPECT_TRUE(QFile::exists(partial.recoveryPath));
    put(target, preparedStandard);
    ASSERT_TRUE(window.saveTrackerProject(target, true));
    EXPECT_FALSE(window.isWindowModified());
    EXPECT_FALSE(QFile::exists(partial.recoveryPath));
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 7);
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(partial.nativePath).project.pointMasses.at(0).mass, 7);
}

TEST(PairedWorkspace, AppliesPreparedOpensAfterSavingAndRejectsCanceledResults)
{
    for (const bool movie : {false, true})
    {
        for (const bool cancel : {false, true})
        {
            for (const bool failSave : {false, true})
            {
                SCOPED_TRACE(::testing::Message() << "movie=" << movie << " cancel=" << cancel << " failSave=" << failSave);
                QTemporaryDir temporary;
                QSemaphore entered;
                QSemaphore release;
                motrack::MainWindow window;
                ASSERT_TRUE(window.loadProject(fixture()));
                auto* pool = window.findChild<QThreadPool*>("workspaceSavePool");
                ASSERT_NE(pool, nullptr);
                pool->start(
                    [&]
                    {
                        entered.release();
                        static_cast<void>(release.tryAcquire(1, 5000));
                    });
                ASSERT_TRUE(entered.tryAcquire(1, 5000));
                const auto input = movie ? "vfr.mp4" : "image-sequence.trk";
                bool processed = false;
                QTimer::singleShot(0,
                                   &window,
                                   [&]
                                   {
                                       const auto path = QString::fromUtf8(MOTRACK_FIXTURES) + '/' + input;
                                       if (movie)
                                           window.openMovie(path);
                                       else
                                           window.openProject(path);
                                       EXPECT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
                                       QCoreApplication::sendPostedEvents();
                                       EXPECT_TRUE(window.windowTitle().contains("linked-images.trk"));
                                       if (cancel)
                                       {
                                           window.openProject(temporary.filePath("missing.trk"));
                                           EXPECT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
                                           QCoreApplication::sendPostedEvents();
                                       }
                                       processed = true;
                                       release.release();
                                   });
                const auto target = temporary.filePath(failSave ? "missing/saved.trk" : "saved.trk");
                EXPECT_EQ(window.saveTrackerProject(target), !failSave);
                EXPECT_TRUE(processed);
                if (cancel)
                {
                    ASSERT_TRUE(QTest::qWaitFor([&] { return window.statusBar()->currentMessage() == "Could not open this project."; }));
                    EXPECT_TRUE(window.windowTitle().contains(failSave ? "linked-images.trk" : "saved.trk"));
                }
                else
                {
                    ASSERT_TRUE(QTest::qWaitFor([&] { return window.windowTitle().contains(input); }));
                }
                if (!failSave)
                {
                    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 2);
                    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(temporary.filePath("saved.motrack")).project.pointMasses.at(0).mass, 2);
                }
            }
        }
    }
}

TEST(PairedWorkspace, DoesNotApplySaveCompletionToAReplacementDocument)
{
    QTemporaryDir temporary;
    QSemaphore entered;
    QSemaphore release;
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    auto* pool = window.findChild<QThreadPool*>("workspaceSavePool");
    ASSERT_NE(pool, nullptr);
    pool->start(
        [&]
        {
            entered.release();
            static_cast<void>(release.tryAcquire(1, 5000));
        });
    ASSERT_TRUE(entered.tryAcquire(1, 5000));
    QTimer::singleShot(0,
                       &window,
                       [&]
                       {
                           EXPECT_TRUE(window.loadProject(fixture()));
                           release.release();
                       });
    const auto target = temporary.filePath("old.trk");
    EXPECT_FALSE(window.saveTrackerProject(target));
    EXPECT_TRUE(window.windowTitle().contains("linked-images.trk"));
    EXPECT_FALSE(window.isWindowModified());
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 2);
}

TEST(PairedWorkspace, CancellationPreservesBothExistingFilesAndNewerEdits)
{
    QTemporaryDir temporary;
    QSemaphore entered;
    QSemaphore release;
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    ASSERT_TRUE(editMass(window, 5));
    const auto target = temporary.filePath("saved.trk");
    const auto native = temporary.filePath("saved.motrack");
    put(target, "previous standard");
    put(native, "previous native");
    auto* pool = window.findChild<QThreadPool*>("workspaceSavePool");
    ASSERT_NE(pool, nullptr);
    pool->start(
        [&]
        {
            entered.release();
            static_cast<void>(release.tryAcquire(1, 5000));
        });
    ASSERT_TRUE(entered.tryAcquire(1, 5000));
    bool clicked = false;
    QTimer::singleShot(0,
                       &window,
                       [&]
                       {
                           if (auto* cancel = window.findChild<QPushButton*>("cancelPairedSave"))
                           {
                               cancel->click();
                               clicked = true;
                           }
                           EXPECT_FALSE(window.saveTrackerProject(target));
                           EXPECT_FALSE(window.close());
                           release.release();
                       });
    EXPECT_FALSE(window.saveTrackerProject(target));
    EXPECT_TRUE(clicked);
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_EQ(bytes(target), "previous standard");
    EXPECT_EQ(bytes(native), "previous native");
    EXPECT_TRUE(window.statusBar()->currentMessage().startsWith("Save canceled."));
    EXPECT_EQ(window.findChild<QDialog*>("pairedSaveProgress"), nullptr);
}

TEST(PairedWorkspace, StaleCompletionAndNativeOnlySaveLeaveStandardEditsDirty)
{
    QTemporaryDir temporary;
    QSemaphore entered;
    QSemaphore release;
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    auto* pool = window.findChild<QThreadPool*>("workspaceSavePool");
    auto* create = window.findChild<QAction*>("newPointMass");
    auto* undo = window.findChild<QAction*>("undoEdit");
    ASSERT_TRUE(pool && create && undo);
    pool->start(
        [&]
        {
            entered.release();
            static_cast<void>(release.tryAcquire(1, 5000));
        });
    ASSERT_TRUE(entered.tryAcquire(1, 5000));
    QTimer::singleShot(0,
                       &window,
                       [&]
                       {
                           create->trigger();
                           release.release();
                       });
    const auto target = temporary.filePath("saved.trk");
    EXPECT_FALSE(window.saveTrackerProject(target));
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_EQ(motrack::readWorkspaceDocument(temporary.filePath("saved.motrack")).project.pointMasses.size(), 1u);
    ASSERT_TRUE(window.saveWorkspace(temporary.filePath("newer-copy.motrack")));
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_EQ(motrack::readWorkspaceDocument(temporary.filePath("newer-copy.motrack")).project.pointMasses.size(), 2u);
    undo->trigger();
    EXPECT_TRUE(window.isWindowModified());
    ASSERT_TRUE(window.saveTrackerProject(target));
    EXPECT_FALSE(window.isWindowModified());
}

TEST(PairedWorkspace, RejectsUnsupportedEditsAndWrongDestinationsWithoutReplacingFiles)
{
    QTemporaryDir temporary;
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(fixture()));
    const auto target = temporary.filePath("saved.trk");
    const auto native = temporary.filePath("saved.motrack");
    put(target, "previous standard");
    put(native, "previous native");
    EXPECT_FALSE(window.saveTrackerProject(temporary.filePath("wrong.trz")));
    EXPECT_FALSE(window.saveTrackerProject(temporary.filePath("missing/saved.trk")));
    ASSERT_TRUE(window.loadProject(QString::fromUtf8(MOTRACK_FIXTURES) + "/movie.trk"));
    ASSERT_TRUE(editMass(window, 5));
    EXPECT_FALSE(window.saveTrackerProject(target));
    EXPECT_TRUE(window.isWindowModified());
    EXPECT_EQ(bytes(target), "previous standard");
    EXPECT_EQ(bytes(native), "previous native");
}

TEST(PairedWorkspace, FileMenuAsksBeforeReplacingAnUnrelatedCompanion)
{
    for (const bool replace : {false, true})
    {
        SCOPED_TRACE(replace);
        QTemporaryDir temporary;
        motrack::MainWindow window;
        window.show();
        ASSERT_TRUE(window.loadProject(fixture()));
        const auto target = temporary.filePath("saved.trk");
        const auto native = temporary.filePath("saved.motrack");
        put(native, "unrelated companion");
        auto* action = window.findChild<QAction*>("saveTrackerProject");
        ASSERT_NE(action, nullptr);
        QTimer poll;
        QTimer watchdog;
        bool choseFile = false;
        bool choseCompanion = false;
        QObject::connect(
            &poll,
            &QTimer::timeout,
            &window,
            [&]
            {
                if (!choseFile)
                {
                    if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                    {
                        choseFile = true;
                        // Qt leaves a focused filename editor unchanged in selectFile().
                        if (auto* focused = dialog->focusWidget())
                            focused->clearFocus();
                        dialog->selectFile(target);
                        ASSERT_TRUE(QTest::qWaitFor([dialog, &target] { return dialog->selectedFiles() == QStringList{target}; }, 1500))
                            << dialog->selectedFiles().join(", ").toStdString();
                        QMetaObject::invokeMethod(dialog, "accept");
                    }
                }
                else if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                {
                    choseCompanion = true;
                    message->button(replace ? QMessageBox::Yes : QMessageBox::Cancel)->click();
                    poll.stop();
                }
            });
        QObject::connect(&watchdog,
                         &QTimer::timeout,
                         &window,
                         []
                         {
                             if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                                 dialog->reject();
                         });
        poll.start(10);
        watchdog.setSingleShot(true);
        watchdog.start(3000);
        action->trigger();
        EXPECT_TRUE(choseFile && choseCompanion);
        if (replace)
        {
            EXPECT_TRUE(QFile::exists(target));
            EXPECT_EQ(motrack::readWorkspaceDocument(native).project.pointMasses.size(), 1u);
        }
        else
        {
            EXPECT_FALSE(QFile::exists(target));
            EXPECT_EQ(bytes(native), "unrelated companion");
        }
    }
}

TEST(PairedWorkspace, PendingOpensInvalidateTheTrackerChooserAndCompanionConfirmation)
{
    for (const bool duringCompanion : {false, true})
    {
        SCOPED_TRACE(duringCompanion);
        QTemporaryDir temporary;
        const auto replacement = temporary.filePath("replacement.trk");
        auto other = motrack::importTrackerDocument(fixture());
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setPointMassValue(other.project, 0, 7));
        motrack::writeTrackerDocument(replacement, other, fixture());
        const auto originalReplacement = bytes(replacement);
        const auto target = temporary.filePath("saved.trk");
        const auto native = temporary.filePath("saved.motrack");
        put(native, "unrelated companion");
        motrack::MainWindow window;
        window.show();
        ASSERT_TRUE(window.loadProject(fixture()));
        auto* action = window.findChild<QAction*>("saveTrackerProject");
        ASSERT_NE(action, nullptr);
        QTimer poll;
        QTimer watchdog;
        bool choseFile = false;
        bool replaced = false;
        const auto replace = [&]
        {
            window.openProject(replacement);
            EXPECT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
            QCoreApplication::sendPostedEvents();
            replaced = window.windowTitle().contains("replacement.trk");
            EXPECT_TRUE(replaced);
        };
        QObject::connect(
            &poll,
            &QTimer::timeout,
            &window,
            [&]
            {
                if (!choseFile)
                {
                    if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                    {
                        choseFile = true;
                        if (!duringCompanion)
                        {
                            poll.stop();
                            replace();
                        }
                        // Qt leaves a focused filename editor unchanged in selectFile().
                        if (auto* focused = dialog->focusWidget())
                            focused->clearFocus();
                        dialog->selectFile(target);
                        ASSERT_TRUE(QTest::qWaitFor([dialog, &target] { return dialog->selectedFiles() == QStringList{target}; }, 1500))
                            << dialog->selectedFiles().join(", ").toStdString();
                        QMetaObject::invokeMethod(dialog, "accept");
                    }
                }
                else if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                {
                    poll.stop();
                    replace();
                    message->button(QMessageBox::Yes)->click();
                }
            });
        QObject::connect(&watchdog,
                         &QTimer::timeout,
                         &window,
                         []
                         {
                             if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                                 dialog->reject();
                         });
        poll.start(10);
        watchdog.start(7000);
        action->trigger();
        EXPECT_TRUE(choseFile && replaced);
        EXPECT_FALSE(QFile::exists(target));
        EXPECT_EQ(bytes(native), "unrelated companion");
        EXPECT_EQ(bytes(replacement), originalReplacement);
        EXPECT_FALSE(QFile::exists(temporary.filePath("replacement.motrack")));
    }
}

TEST(PairedWorkspace, PendingOpensInvalidateTheNativeSaveChooser)
{
    QTemporaryDir temporary;
    const auto replacement = temporary.filePath("replacement.trk");
    ASSERT_TRUE(QFile::copy(fixture(), replacement));
    const auto target = temporary.filePath("saved.motrack");
    put(target, "keep existing destination");
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixture()));
    auto* action = window.findChild<QAction*>("saveWorkspace");
    ASSERT_NE(action, nullptr);
    QTimer poll;
    QTimer watchdog;
    bool replaced = false;
    QObject::connect(&poll,
                     &QTimer::timeout,
                     &window,
                     [&]
                     {
                         auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                         if (!dialog)
                             return;
                         poll.stop();
                         window.openProject(replacement);
                         ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
                         QCoreApplication::sendPostedEvents();
                         replaced = window.windowTitle().contains("replacement.trk");
                         ASSERT_TRUE(replaced);
                         dialog->setOption(QFileDialog::DontConfirmOverwrite);
                         if (auto* focused = dialog->focusWidget())
                             focused->clearFocus();
                         dialog->selectFile(target);
                         ASSERT_TRUE(QTest::qWaitFor([dialog, &target] { return dialog->selectedFiles() == QStringList{target}; }, 1500));
                         QMetaObject::invokeMethod(dialog, "accept");
                     });
    QObject::connect(&watchdog,
                     &QTimer::timeout,
                     &window,
                     []
                     {
                         if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                             dialog->reject();
                     });
    poll.start(10);
    watchdog.start(7000);
    action->trigger();
    EXPECT_TRUE(replaced);
    EXPECT_EQ(bytes(target), "keep existing destination");
}

TEST(PairedWorkspace, PendingOpenInvalidatesRecoveryConfirmationAndItsSaveDestination)
{
    QTemporaryDir temporary;
    const auto target = temporary.filePath("saved.trk");
    std::atomic_bool cancelled{false};
    const auto partial =
        motrack::writePairedDocuments(target,
                                      motrack::importTrackerDocument(fixture()),
                                      fixture(),
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
    const auto originalStandard = bytes(target);
    const auto originalRecord = bytes(partial.recoveryPath);
    const auto replacement = temporary.filePath("replacement.trk");
    auto other = motrack::importTrackerDocument(fixture());
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(other.project, 0, 7));
    motrack::writeTrackerDocument(replacement, other, fixture());
    const auto originalReplacement = bytes(replacement);
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixture()));
    EXPECT_FALSE(window.saveTrackerProject(target));
    auto* action = window.findChild<QAction*>("saveProject");
    ASSERT_NE(action, nullptr);
    QTimer poll;
    QTimer discard;
    QTimer watchdog;
    bool confirmed = false;
    bool replaced = false;
    QObject::connect(&discard,
                     &QTimer::timeout,
                     &window,
                     []
                     {
                         if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                         {
                             if (auto* button = message->button(QMessageBox::Discard))
                                 button->click();
                         }
                     });
    QObject::connect(&poll,
                     &QTimer::timeout,
                     &window,
                     [&]
                     {
                         // The outer confirmation remains on the stack throughout this callback.
                         auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                         if (!message || !message->button(QMessageBox::Yes))
                             return;
                         poll.stop();
                         discard.start(10);
                         window.openProject(replacement);
                         EXPECT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
                         QCoreApplication::sendPostedEvents();
                         replaced = window.windowTitle().contains("replacement.trk");
                         EXPECT_TRUE(replaced);
                         discard.stop();
                         confirmed = true;
                         message->button(QMessageBox::Yes)->click();
                     });
    QObject::connect(&watchdog,
                     &QTimer::timeout,
                     &window,
                     []
                     {
                         if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                             dialog->reject();
                     });
    poll.start(10);
    watchdog.start(7000);
    action->trigger();
    EXPECT_TRUE(confirmed && replaced);
    EXPECT_EQ(bytes(target), originalStandard);
    EXPECT_EQ(bytes(partial.recoveryPath), originalRecord);
    EXPECT_FALSE(QFile::exists(partial.nativePath));
    EXPECT_EQ(bytes(replacement), originalReplacement);
    EXPECT_FALSE(QFile::exists(temporary.filePath("replacement.motrack")));
}

TEST(PairedWorkspace, DestroyingTheWindowDuringSaveCancelsAndReleasesTheWorker)
{
    for (const auto [native, throughMenu] :
         {std::pair{false, false}, std::pair{false, true}, std::pair{true, false}, std::pair{true, true}})
    {
        SCOPED_TRACE(native);
        SCOPED_TRACE(throughMenu);
        QTemporaryDir temporary;
        const auto target = temporary.filePath(native ? "saved.motrack" : "saved.trk");
        put(target, "previous destination");
        QSemaphore entered;
        QSemaphore release;
        auto window = std::make_unique<motrack::MainWindow>();
        ASSERT_TRUE(window->loadProject(fixture()));
        // MainWindow sets cancellation before QObject emits destroyed, then its child pool joins.
        QObject::connect(window.get(), &QObject::destroyed, [&release] { release.release(); });
        auto* pool = window->findChild<QThreadPool*>("workspaceSavePool");
        ASSERT_NE(pool, nullptr);
        pool->start(
            [&]
            {
                entered.release();
                static_cast<void>(release.tryAcquire(1, 5000));
            });
        ASSERT_TRUE(entered.tryAcquire(1, 5000));
        QTimer poll;
        QTimer watchdog;
        bool choseFile = false;
        QObject::connect(
            &poll,
            &QTimer::timeout,
            [&]
            {
                if (throughMenu && !choseFile)
                {
                    if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                    {
                        choseFile = true;
                        dialog->setOption(QFileDialog::DontConfirmOverwrite, true);
                        // Qt leaves a focused filename editor unchanged in selectFile().
                        if (auto* focused = dialog->focusWidget())
                            focused->clearFocus();
                        dialog->selectFile(target);
                        ASSERT_TRUE(QTest::qWaitFor([dialog, &target] { return dialog->selectedFiles() == QStringList{target}; }, 1500))
                            << dialog->selectedFiles().join(", ").toStdString();
                        QMetaObject::invokeMethod(dialog, "accept");
                    }
                    return;
                }
                if (window && (native ? window->statusBar()->currentMessage() == "Saving project…"
                                      : window->findChild<QDialog*>("pairedSaveProgress") != nullptr))
                {
                    poll.stop();
                    window.reset();
                }
            });
        QObject::connect(&watchdog,
                         &QTimer::timeout,
                         [&]
                         {
                             release.release();
                             if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                                 dialog->reject();
                         });
        poll.start(10);
        watchdog.start(3000);
        if (throughMenu)
        {
            auto* action = window->findChild<QAction*>(native ? "saveWorkspace" : "saveTrackerProject");
            ASSERT_NE(action, nullptr);
            action->trigger();
            EXPECT_TRUE(choseFile);
        }
        else
            EXPECT_FALSE(native ? window->saveWorkspace(target) : window->saveTrackerProject(target));
        EXPECT_EQ(window, nullptr);
        EXPECT_EQ(bytes(target), "previous destination");
    }
}

TEST(PairedWorkspace, WindowDestructionDuringSaveDialogsLeavesNoDanglingWidgets)
{
    // Native/Tracker chooser, companion/recovery confirmation, and discard prompts from close/open.
    for (int phase = 0; phase < 8; ++phase)
    {
        SCOPED_TRACE(phase);
        QTemporaryDir temporary;
        const auto standard = temporary.filePath("saved.trk");
        const auto native = temporary.filePath("saved.motrack");
        const auto replacement = temporary.filePath("replacement.trk");
        ASSERT_TRUE(QFile::copy(fixture(), replacement));
        QString journal;
        QByteArray standardBefore;
        if (phase == 2)
            put(native, "unrelated companion");
        if (phase == 3)
        {
            std::atomic_bool cancelled{false};
            const auto interrupted = motrack::writePairedDocuments(standard,
                                                                   motrack::importTrackerDocument(fixture()),
                                                                   fixture(),
                                                                   &cancelled,
                                                                   [&cancelled](auto state)
                                                                   { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
            ASSERT_EQ(interrupted.status, motrack::PairedSaveStatus::RecoveryRequired);
            journal = interrupted.recoveryPath;
            standardBefore = bytes(standard);
        }
        auto window = std::make_unique<motrack::MainWindow>();
        ASSERT_TRUE(window->loadProject(fixture()));
        if (phase == 3)
            EXPECT_FALSE(window->saveTrackerProject(standard));
        if (phase >= 4)
            ASSERT_TRUE(editMass(*window, 5));
        QTimer poll;
        QTimer watchdog;
        bool destroyed = false;
        bool choseFile = false;
        QObject::connect(&poll,
                         &QTimer::timeout,
                         [&]
                         {
                             if (auto* chooser = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                             {
                                 if (phase == 2 && !choseFile)
                                 {
                                     choseFile = true;
                                     if (auto* focused = chooser->focusWidget())
                                         focused->clearFocus();
                                     chooser->selectFile(standard);
                                     ASSERT_TRUE(QTest::qWaitFor([&] { return chooser->selectedFiles() == QStringList{standard}; }, 1500));
                                     QMetaObject::invokeMethod(chooser, "accept");
                                     return;
                                 }
                                 if (phase > 1)
                                     return;
                             }
                             else if (!qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                                 return;
                             poll.stop();
                             destroyed = true;
                             window.reset();
                         });
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog,
                         &QTimer::timeout,
                         []
                         {
                             if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                                 dialog->reject();
                         });
        poll.start(10);
        watchdog.start(4000);
        if (phase < 4)
        {
            const auto* name = phase == 0 ? "saveWorkspace" : phase == 3 ? "saveProject" : "saveTrackerProject";
            window->findChild<QAction*>(name)->trigger();
        }
        else if (phase == 4)
            static_cast<void>(window->close());
        else if (phase == 5)
            window->openProject(replacement);
        else if (phase == 6)
            window->openMovie(QString::fromUtf8(MOTRACK_FIXTURES) + "/vfr.mp4");
        else
            EXPECT_FALSE(window->openImageSequence({fixture("images/frame0.png")}, 25));
        ASSERT_TRUE(QTest::qWaitFor([&] { return !window; }, 4500));
        EXPECT_TRUE(destroyed);
        if (phase == 2)
            EXPECT_EQ(bytes(native), "unrelated companion");
        if (phase == 3)
        {
            EXPECT_EQ(bytes(standard), standardBefore);
            EXPECT_TRUE(QFileInfo::exists(journal));
        }
        else
            EXPECT_FALSE(QFileInfo::exists(standard));
    }
}

#ifndef Q_OS_WIN
TEST(PairedWorkspace, RetainsRestoredMediaBindingsAfterPairedSaveAndSourceRemoval)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(QDir(temporary.path()).mkpath("import"));
    ASSERT_TRUE(QDir(temporary.path()).mkpath("media"));
    ASSERT_TRUE(QDir(temporary.path()).mkpath("exports"));
    const auto input = temporary.filePath("import/input.trk");
    put(input, bytes(fixture()));
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(input));
    const auto media = temporary.filePath("media");
    const auto alias = temporary.filePath("import/images");
    ASSERT_TRUE(QFile::link(media, alias));
    for (int frame = 0; frame < 4; ++frame)
    {
        const auto name = "frame" + QString::number(frame) + ".png";
        ASSERT_TRUE(QFile::copy(fixture("images/" + name), media + '/' + name));
    }
    ASSERT_TRUE(window.saveTrackerProject(temporary.filePath("exports/first.trk")));
    ASSERT_TRUE(QFile::remove(input));
    ASSERT_TRUE(QFile::remove(alias));
    ASSERT_TRUE(QDir(temporary.path()).rmdir("import"));
    auto* slider = window.findChild<QSlider*>("stepSlider");
    auto* canvas = window.findChild<QWidget*>("videoCanvas");
    ASSERT_TRUE(slider && canvas);
    slider->setValue(2);
    EXPECT_EQ(canvas->accessibleName(), "Frame 2");
    slider->setValue(0);
    EXPECT_EQ(canvas->accessibleName(), "Frame 0");
    ASSERT_TRUE(editMass(window, 7));
    ASSERT_TRUE(window.saveWorkspace(temporary.filePath("exports/native-copy.motrack")));
    const auto output = temporary.filePath("exports/second.trk");
    ASSERT_TRUE(window.saveTrackerProject(output));
    EXPECT_FALSE(window.isWindowModified());
    const auto standard = motrack::importTrackerDocument(output);
    const auto native = motrack::readWorkspaceDocument(temporary.filePath("exports/second.motrack"));
    EXPECT_DOUBLE_EQ(standard.project.pointMasses.at(0).mass, 7);
    EXPECT_EQ(standard.project.clip.mediaReferences, native.project.clip.mediaReferences);
    for (int frame = 0; frame < 4; ++frame)
    {
        const auto name = "frame" + QString::number(frame) + ".png";
        EXPECT_EQ(standard.project.clip.mediaReferences.at(frame), ("../media/" + name).toStdString());
    }
}
#endif

} // namespace
