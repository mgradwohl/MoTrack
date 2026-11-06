#include "image_sequence.hpp"
#include "main_window.hpp"

#include <QApplication>
#include <QLabel>
#include <QSlider>
#include <QTableWidget>
#include <QToolButton>
#include <gtest/gtest.h>

namespace
{

QString fixturePath(const QString& name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + '/' + name;
}

TEST(ImageSequence, ReportsMissingImagesAndRejectsOutOfRangeSteps)
{
    motrack::Clip clip;
    clip.mediaReferences = {"https://example.invalid/remote.ppm", "archive.trz!/frame.ppm", "missing.ppm"};
    const auto sequence = motrack::ImageSequence::open(fixturePath("image-sequence.trk"), clip);
    ASSERT_TRUE(sequence.has_value());
    EXPECT_EQ(sequence->stepCount(), 3);
    EXPECT_FALSE(sequence->readStep(-1).has_value());
    EXPECT_FALSE(sequence->readStep(3).has_value());
    for (int step = 0; step < sequence->stepCount(); ++step)
    {
        const auto frame = sequence->readStep(step);
        ASSERT_TRUE(frame.has_value());
        EXPECT_EQ(frame->number, step);
        EXPECT_TRUE(frame->image.isNull());
    }
    clip.firstFrame = 3;
    EXPECT_FALSE(motrack::ImageSequence::open(fixturePath("image-sequence.trk"), clip).has_value());
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

} // namespace

int main(int argc, char** argv)
{
    const QApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
