#include "template_tracker.hpp"

#include <gtest/gtest.h>

namespace
{

void drawTarget(QImage& image, QPoint center, int brightness)
{
    for (int y = -5; y <= 5; ++y)
    {
        for (int x = -5; x <= 5; ++x)
        {
            const auto value = ((x + 5) * 67 + (y + 5) * 113 + (x * y + 25) * 37) % 170 + brightness;
            image.scanLine(center.y() + y)[center.x() + x] = static_cast<uchar>(value);
        }
    }
}

TEST(TemplateTracker, FindsTranslationWithChangedBrightness)
{
    QImage first(80, 50, QImage::Format_Grayscale8);
    first.fill(20);
    drawTarget(first, {28, 23}, 20);
    const auto tracker = motrack::TemplateTracker::fromImage(first, {28, 23});
    if (!tracker)
    {
        FAIL() << "Could not sample the textured target.";
    }
    QImage next(80, 50, QImage::Format_Grayscale8);
    next.fill(40);
    drawTarget(next, {37, 19}, 40);
    EXPECT_EQ(tracker->find(next, {28, 23}), QPoint(37, 19));
    EXPECT_FALSE(tracker->find(next, {79, 49}));
    EXPECT_FALSE(tracker->find(next, {-1, 23}));
}

TEST(TemplateTracker, RejectsBlankClippedAndAmbiguousPatterns)
{
    QImage image(80, 50, QImage::Format_Grayscale8);
    image.fill(25);
    EXPECT_FALSE(motrack::TemplateTracker::fromImage(image, {20, 25}));
    EXPECT_FALSE(motrack::TemplateTracker::fromImage(image, {2, 2}));
    EXPECT_FALSE(motrack::TemplateTracker::fromImage({}, {0, 0}));

    drawTarget(image, {25, 25}, 25);
    const auto tracker = motrack::TemplateTracker::fromImage(image, {25, 25});
    if (!tracker)
    {
        FAIL() << "Could not sample the textured target.";
    }
    drawTarget(image, {47, 25}, 25);
    EXPECT_FALSE(tracker->find(image, {36, 25}));
    QImage tiny(4, 4, QImage::Format_Grayscale8);
    EXPECT_FALSE(tracker->find(tiny, {1, 1}));
}

} // namespace
