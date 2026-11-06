#include "image_sequence.hpp"
#include "main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QSlider>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <gtest/gtest.h>

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
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    QCoreApplication::processEvents();
    EXPECT_EQ(canvas->accessibleName(), "Frame 0");
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

} // namespace

int main(int argc, char** argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const QApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
