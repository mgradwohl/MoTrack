#include "sampling_expectations.hpp"
#include "science/sampling/argb_image.hpp"
#include "science/sampling/line_profile.hpp"
#include "science/sampling/rgb_region.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

// Unit tests with expectations derived independently of Tracker: closed-form pixel counts and
// means, exact overlap areas by slice integration, and Tracker's documented limits.

namespace
{

using motrack::science::ArgbImage;
using motrack::science::clampLineProfileSpread;
using motrack::science::clampRgbEdgeLength;
using motrack::science::ImagePoint;
using motrack::science::LINE_PROFILE_COLUMNS;
using motrack::science::LINE_PROFILE_MAX_LENGTH;
using motrack::science::LINE_PROFILE_MAX_SPREAD;
using motrack::science::LineProfileMode;
using motrack::science::lineProfileMode;
using motrack::science::LineProfileStatus;
using motrack::science::measureRgbRegion;
using motrack::science::PixelRect;
using motrack::science::RGB_DEFAULT_EDGE_LENGTH;
using motrack::science::RGB_DEFAULT_MAX_EDGE_LENGTH;
using motrack::science::RGB_REGION_COLUMNS;
using motrack::science::rgbMaxEdgeLength;
using motrack::science::RgbPolygon;
using motrack::science::RgbRegionOutline;
using motrack::science::RgbRegionShape;
using motrack::science::RgbRegionStatus;
using motrack::science::RgbShapeKind;
using motrack::science::sampleLineProfile;
using motrack::science::trackerLuma;
using motrack::science::expectations::exactMean;
using motrack::science::expectations::latticeMean;
using motrack::science::expectations::makeImage;
using motrack::science::expectations::probeCorners;
using motrack::science::expectations::rgb;

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();

ArgbImage gradient(int width = 128, int height = 96)
{
    return makeImage(width, height, [](int x, int y) { return rgb(x & 0xff, y & 0xff, (x + y) & 0xff); });
}

RgbRegionShape rectangle(int width, int height)
{
    return {.kind = RgbShapeKind::Rectangle, .width = width, .height = height, .polygon = {}};
}

RgbRegionShape ellipse(int width, int height)
{
    return {.kind = RgbShapeKind::Ellipse, .width = width, .height = height, .polygon = {}};
}

RgbRegionShape polygon(std::vector<ImagePoint> vertices)
{
    return {.kind = RgbShapeKind::Polygon,
            .width = RGB_DEFAULT_EDGE_LENGTH,
            .height = RGB_DEFAULT_EDGE_LENGTH,
            .polygon = RgbPolygon(std::move(vertices))};
}

/// Pixel centres k + 0.5 in the half-open interval [low, high).
int centresIn(double low, double high)
{
    int count = 0;
    for (int k = static_cast<int>(std::floor(low)) - 1; k <= static_cast<int>(std::ceil(high)) + 1; ++k)
    {
        count += (k + 0.5 >= low && k + 0.5 < high) ? 1 : 0;
    }
    return count;
}

TEST(ArgbImage, ValidatesSizesAndAddressesWordsByStride)
{
    EXPECT_FALSE(ArgbImage::filled(-1, 4, 0));
    EXPECT_FALSE(ArgbImage::filled(4, ArgbImage::MAX_DIMENSION + 1, 0));
    const auto empty = ArgbImage::filled(0, 0, 0);
    if (!empty)
    {
        FAIL() << "a 0 x 0 image is valid";
    }
    EXPECT_EQ(empty->width(), 0);
    EXPECT_FALSE(empty->contains(0, 0));
    EXPECT_EQ(ArgbImage().height(), 0);

    EXPECT_FALSE(ArgbImage::fromPixels(3, 2, 2, std::vector<std::uint32_t>(6)));  // stride < width
    EXPECT_FALSE(ArgbImage::fromPixels(3, 2, 4, std::vector<std::uint32_t>(6)));  // needs 4 + 3 words
    EXPECT_FALSE(ArgbImage::fromPixels(-3, 2, 4, std::vector<std::uint32_t>(8))); // negative width
    EXPECT_TRUE(ArgbImage::fromPixels(3, 0, 3, {}));
    auto image = ArgbImage::fromPixels(3, 2, 4, {0xff010203U, 2, 3, 0xdead, 0xff112233U, 5, 6});
    if (!image)
    {
        FAIL() << "a strided 3 x 2 image is valid";
    }
    EXPECT_EQ(image->stride(), 4U);
    EXPECT_EQ(image->pixel(0, 1), 0xff112233U);
    EXPECT_EQ(image->row(1).size(), 3U);
    EXPECT_EQ(image->row(1)[2], 6U);
    image->setPixel(2, 1, 0x00abcdefU);
    EXPECT_EQ(image->pixel(2, 1), 0x00abcdefU);
    EXPECT_TRUE(image->contains(2, 1));
    EXPECT_FALSE(image->contains(3, 1));
    EXPECT_FALSE(image->contains(0, -1));
    EXPECT_EQ(motrack::science::redOf(0xff112233U), 0x11);
    EXPECT_EQ(motrack::science::greenOf(0xff112233U), 0x22);
    EXPECT_EQ(motrack::science::blueOf(0xff112233U), 0x33);
}

TEST(TrackerLuma, UsesCcir601WeightsInJavaOrder)
{
    EXPECT_EQ(trackerLuma(1, 0, 0), 0.299);
    EXPECT_EQ(trackerLuma(0, 1, 0), 0.587);
    EXPECT_EQ(trackerLuma(0, 0, 1), 0.114);
    EXPECT_EQ(trackerLuma(10, 20, 30), (0.299 * 10 + 0.587 * 20) + 0.114 * 30);
    EXPECT_EQ(RGB_REGION_COLUMNS[3], "luma");
    EXPECT_EQ(LINE_PROFILE_COLUMNS[7], "pixels");
}

TEST(RgbShapeLimits, FollowTrackerEditLimits)
{
    EXPECT_EQ(motrack::science::rgbShapeKindFromSaved(std::nullopt), RgbShapeKind::Ellipse);
    EXPECT_EQ(motrack::science::rgbShapeKindFromSaved(0), RgbShapeKind::Ellipse);
    EXPECT_EQ(motrack::science::rgbShapeKindFromSaved(1), RgbShapeKind::Rectangle);
    EXPECT_EQ(motrack::science::rgbShapeKindFromSaved(2), RgbShapeKind::Polygon);
    EXPECT_EQ(motrack::science::rgbShapeKindFromSaved(7), RgbShapeKind::Rectangle);
    EXPECT_EQ(motrack::science::rgbShapeKindFromSaved(-1), RgbShapeKind::Rectangle);
    EXPECT_EQ(RGB_DEFAULT_EDGE_LENGTH, 20);
    EXPECT_EQ(RGB_DEFAULT_MAX_EDGE_LENGTH, 200);
    EXPECT_EQ(rgbMaxEdgeLength(1920, 1080), 1079);
    EXPECT_EQ(rgbMaxEdgeLength(64, 48), 47);
    EXPECT_EQ(clampRgbEdgeLength(-3, 200), 0);
    EXPECT_EQ(clampRgbEdgeLength(250, 200), 200);
    EXPECT_EQ(clampRgbEdgeLength(37, 200), 37);
    EXPECT_EQ(clampLineProfileSpread(-1), 0);
    EXPECT_EQ(clampLineProfileSpread(150), LINE_PROFILE_MAX_SPREAD);
    EXPECT_EQ(clampLineProfileSpread(7), 7);
}

TEST(RgbPolygon, ClosesReopensAndEditsLikeTracker)
{
    RgbPolygon polygon;
    EXPECT_TRUE(polygon.empty());
    EXPECT_EQ(polygon.handleCount(), 0U);
    polygon.append({.x = 0, .y = 0});
    polygon.append({.x = 10, .y = 0});
    EXPECT_FALSE(polygon.close()); // fewer than three vertices
    polygon.append({.x = 0, .y = 10});
    EXPECT_FALSE(polygon.isClosed());
    EXPECT_EQ(polygon.handleCount(), 2U);
    EXPECT_FALSE(polygon.reopen());
    EXPECT_TRUE(polygon.close());
    EXPECT_TRUE(polygon.isClosed());
    EXPECT_FALSE(polygon.close());
    EXPECT_EQ(polygon.vertices().size(), 4U);
    EXPECT_EQ(polygon.handleCount(), 2U); // the closing vertex is not a handle
    EXPECT_TRUE(polygon.reopen());
    EXPECT_EQ(polygon.vertices().size(), 3U);

    EXPECT_TRUE(polygon.moveHandleVertex(1, {2.5, 9.5}));
    EXPECT_EQ(polygon.vertices()[2].x, 2.5);
    EXPECT_FALSE(polygon.moveHandleVertex(2, {1, 1}));
    EXPECT_FALSE(polygon.moveHandleVertex(-1, {1, 1}));

    // U12: handle i removes vertex i + 1; the tip rejects handle == size - 1, which 6.3.5 accepted.
    EXPECT_FALSE(polygon.removeHandleVertex(2));
    EXPECT_FALSE(polygon.removeHandleVertex(-1));
    EXPECT_TRUE(polygon.removeHandleVertex(0));
    ASSERT_EQ(polygon.vertices().size(), 2U);
    EXPECT_EQ(polygon.vertices()[1].x, 2.5);
    EXPECT_TRUE(polygon.removeHandleVertex(0));
    EXPECT_FALSE(polygon.removeHandleVertex(0)); // a single vertex stays
    EXPECT_EQ(polygon.vertices().size(), 1U);

    // A last vertex other than exactly (0, 0) leaves the polygon open.
    const RgbPolygon nearlyClosed({{.x = 0, .y = 0}, {.x = 5, .y = 0}, {.x = 5, .y = 5}, {.x = 1e-12, .y = 0}});
    EXPECT_FALSE(nearlyClosed.isClosed());
}

TEST(RgbRegionOutline, RectanglesIncludeHalfOpenPixelCentres)
{
    // Rectangle [x - w/2, x - w/2 + w) with integer division; centres on the left/top edge count.
    for (const double x : {10.0, 10.25, 10.5, 10.6, 11.0, 11.999})
    {
        for (const double y : {7.0, 7.5, 7.75})
        {
            for (const auto& [w, h] : {std::pair{1, 1}, std::pair{2, 3}, std::pair{5, 4}, std::pair{7, 13}})
            {
                const RgbRegionOutline outline(rectangle(w, h), {.x = x, .y = y});
                const int offsetX = -w / 2; // Java int division
                const int offsetY = -h / 2;
                const double left = x + offsetX;
                const double top = y + offsetY;
                const int expected = centresIn(left, left + w) * centresIn(top, top + h);
                int included = 0;
                for (int py = 0; py < 30; ++py)
                {
                    for (int px = 0; px < 30; ++px)
                    {
                        included += outline.includesPixel(px, py) ? 1 : 0;
                    }
                }
                EXPECT_EQ(included, expected) << x << "," << y << " " << w << "x" << h;
            }
        }
    }
    const RgbRegionOutline odd(rectangle(5, 5), {.x = 20, .y = 20});
    EXPECT_EQ(odd.bounds(), (PixelRect{18, 18, 5, 5})); // [-2, 3): half a pixel right of centre
    EXPECT_TRUE(odd.contains(18.0, 18.0));
    EXPECT_FALSE(odd.contains(23.0, 20.0));
    EXPECT_EQ(RgbRegionOutline(rectangle(4, 4), {20.5, 20.5}).bounds(), (PixelRect{18, 18, 5, 5}));
}

TEST(RgbRegionOutline, EllipsesFollowTheConicAwayFromTheBezierBand)
{
    // Java2D's Bezier circle deviates from the true circle by < 2.8e-4 of the radius; pixel centres
    // farther than that from the boundary (in normalized radius) must agree with the exact conic.
    for (const auto& [w, h, x, y] : {std::tuple{20, 20, 48.0, 36.0},
                                     std::tuple{21, 21, 48.3, 36.7},
                                     std::tuple{7, 13, 30.25, 40.75},
                                     std::tuple{60, 50, 48.0, 36.0},
                                     std::tuple{3, 8, 70.75, 8.5}})
    {
        const RgbRegionOutline outline(ellipse(w, h), {.x = x, .y = y});
        const int offsetX = -w / 2;
        const int offsetY = -h / 2;
        const double cx = x + offsetX + (w / 2.0);
        const double cy = y + offsetY + (h / 2.0);
        // An affine image of the circle approximation: the same relative error in normalized radius.
        const double band = 5e-4;
        int checked = 0;
        for (int py = 0; py < 100; ++py)
        {
            for (int px = 0; px < 100; ++px)
            {
                const double u = (px + 0.5 - cx) / (w / 2.0);
                const double v = (py + 0.5 - cy) / (h / 2.0);
                const double radius = std::sqrt(u * u + v * v);
                if (std::abs(radius - 1) > band)
                {
                    EXPECT_EQ(outline.includesPixel(px, py), radius < 1) << w << "x" << h << " pixel " << px << "," << py;
                    ++checked;
                }
            }
        }
        EXPECT_GT(checked, 9000);
    }
    EXPECT_FALSE(RgbRegionOutline(ellipse(0, 20), {40, 30}).includesPixel(39, 30));
    EXPECT_EQ(RgbRegionOutline(ellipse(-4, 6), {40, 30}).bounds(), (PixelRect{0, 0, 0, 0})); // empty path
}

TEST(RgbRegionOutline, PolygonsUseNonZeroWindingAndImplicitClosure)
{
    // Bow tie: both lobes (winding +1 and -1) are inside; the crossing point region is the waist.
    const RgbRegionOutline bowtie(polygon({{.x = 0, .y = 0}, {.x = 20, .y = 12}, {.x = 20, .y = 0}, {.x = 0, .y = 12}}),
                                  {.x = 40, .y = 30});
    EXPECT_TRUE(bowtie.contains(43, 36));  // left lobe
    EXPECT_TRUE(bowtie.contains(57, 36));  // right lobe
    EXPECT_FALSE(bowtie.contains(50, 31)); // above the crossing, between the lobes
    // A square wound twice is inside (winding 2), unlike the even-odd rule.
    const RgbRegionOutline twice(polygon({{.x = 0, .y = 0},
                                          {.x = 10, .y = 0},
                                          {.x = 10, .y = 10},
                                          {.x = 0, .y = 10},
                                          {.x = 0, .y = 0},
                                          {.x = 10, .y = 0},
                                          {.x = 10, .y = 10},
                                          {.x = 0, .y = 10}}),
                                 {.x = 40, .y = 30});
    EXPECT_TRUE(twice.contains(45, 35));
    // Pentagram centre has winding number 2 and is inside.
    const RgbRegionOutline star(polygon({{.x = 0, .y = 0},
                                         {.x = 8.8168, .y = 27.1353},
                                         {.x = -14.2658, .y = 10.3647},
                                         {.x = 14.2658, .y = 10.3647},
                                         {.x = -8.8168, .y = 27.1353}}),
                                {.x = 48, .y = 30});
    EXPECT_TRUE(star.contains(48, 45));
    // An open triangle is closed implicitly, exactly like its explicitly closed form.
    const RgbRegionOutline open(polygon({{.x = 0, .y = 0}, {.x = 15, .y = 0}, {.x = 0, .y = 12}}), {.x = 48, .y = 36});
    const RgbRegionOutline closed(polygon({{.x = 0, .y = 0}, {.x = 15, .y = 0}, {.x = 0, .y = 12}, {.x = 0, .y = 0}}), {.x = 48, .y = 36});
    for (int py = 30; py < 50; ++py)
    {
        for (int px = 45; px < 65; ++px)
        {
            EXPECT_EQ(open.includesPixel(px, py), closed.includesPixel(px, py));
        }
    }
    // Degenerate polygons contain nothing.
    EXPECT_FALSE(RgbRegionOutline(polygon({{0, 0}}), {48, 36}).contains(48.2, 36.2));
    EXPECT_FALSE(RgbRegionOutline(polygon({}), {48, 36}).contains(48, 36));
    EXPECT_EQ(RgbRegionOutline(polygon({}), {48, 36}).bounds(), (PixelRect{0, 0, 0, 0}));
    // Non-finite geometry is rejected rather than cast.
    const RgbRegionOutline invalid(polygon({{.x = 0, .y = 0}, {.x = NOT_A_NUMBER, .y = 1}, {.x = 4, .y = 4}}), {.x = 10, .y = 10});
    EXPECT_FALSE(invalid.isFinite());
    EXPECT_FALSE(invalid.bounds());
    EXPECT_FALSE(invalid.contains(11, 11));
    EXPECT_FALSE(RgbRegionOutline(rectangle(4, 4), {std::numeric_limits<double>::infinity(), 0}).bounds());
    EXPECT_FALSE(RgbRegionOutline(rectangle(4, 4), {10, 10}).contains(NOT_A_NUMBER, 10));
    // Bounds beyond the int range cannot be measured.
    EXPECT_FALSE(RgbRegionOutline(rectangle(4, 4), {.x = 3e9, .y = 10}).bounds());
    // Finite inputs whose translation overflows to infinity give NaN extents (inf - inf).
    const RgbRegionOutline overflowing(polygon({{.x = 1e308, .y = 1e308}, {.x = 1e308, .y = 1e308}, {.x = 1e308, .y = 1e308}}),
                                       {.x = 1e308, .y = 1e308});
    EXPECT_TRUE(overflowing.isFinite());
    EXPECT_FALSE(overflowing.bounds());
    EXPECT_EQ(
        measureRgbRegion(gradient(), polygon({{.x = 1e308, .y = 0}, {.x = 1e308, .y = 1}, {.x = 1e308, .y = 2}}), {.x = 1e308, .y = 0})
            .status,
        RgbRegionStatus::OutsideImage);
}

TEST(MeasureRgbRegion, ReportsAnalyticMeansAndDeviations)
{
    // Rectangle [30.3, 50.3) x [25.6, 35.6) on R = x, G = y, B = x + y: columns 30..49, rows 26..35.
    const auto image = gradient();
    const auto result = measureRgbRegion(image, rectangle(20, 10), {.x = 40.3, .y = 30.6});
    ASSERT_EQ(result.status, RgbRegionStatus::Measured);
    EXPECT_EQ(result.bounds, (PixelRect{30, 25, 21, 11}));
    EXPECT_EQ(result.statistics.pixels, 200);
    EXPECT_EQ(result.statistics.red, 39.5);
    EXPECT_EQ(result.statistics.green, 30.5);
    EXPECT_EQ(result.statistics.blue, 70.0);
    EXPECT_EQ(result.statistics.luma, trackerLuma(39.5, 30.5, 70.0));
    // Columns 30..49 repeat for each of 10 rows: the variance of 20 consecutive integers is (20^2 - 1) / 12 * n / (n - 1).
    EXPECT_NEAR(result.statistics.redDeviation, std::sqrt((400.0 - 1) / 12 * 200 / 199), 1e-12);
    EXPECT_NEAR(result.statistics.greenDeviation, std::sqrt((100.0 - 1) / 12 * 200 / 199), 1e-12);
    EXPECT_EQ(result.statistics.sums[0], 7900);
    EXPECT_FALSE(result.trackerSumOverflow);

    // Two-valued checker: var = na nb (a - b)^2 / (n (n - 1)).
    const auto checker = makeImage(80, 60, [](int x, int y) { return ((x / 4 + y / 4) & 1) == 0 ? rgb(230, 20, 120) : rgb(10, 200, 60); });
    const auto cells = measureRgbRegion(checker, rectangle(16, 16), {.x = 40, .y = 30});
    ASSERT_EQ(cells.status, RgbRegionStatus::Measured);
    std::int64_t on = 0;
    for (int y = 22; y < 38; ++y)
    {
        for (int x = 32; x < 48; ++x)
        {
            on += ((x / 4 + y / 4) & 1) == 0 ? 1 : 0;
        }
    }
    const double n = 256;
    EXPECT_EQ(cells.statistics.pixels, 256);
    EXPECT_NEAR(cells.statistics.red, (on * 230.0 + (256 - on) * 10.0) / n, 1e-12);
    EXPECT_NEAR(cells.statistics.redDeviation, std::sqrt(on * (n - on) * 220.0 * 220.0 / (n * (n - 1))), 1e-10);

    // Alpha is ignored; one pixel has no deviation.
    const auto translucent = ArgbImage::fromPixels(2, 2, 2, {0x00090807U, 0x80090807U, 0x7f090807U, 0x01090807U});
    if (!translucent)
    {
        FAIL() << "a 2 x 2 image is valid";
    }
    auto single = measureRgbRegion(*translucent, rectangle(1, 1), {.x = 1, .y = 1});
    ASSERT_EQ(single.status, RgbRegionStatus::Measured);
    EXPECT_EQ(single.statistics.pixels, 1);
    EXPECT_EQ(single.statistics.red, 9.0);
    EXPECT_TRUE(std::isnan(single.statistics.redDeviation));
    EXPECT_TRUE(std::isnan(single.statistics.blueDeviation));
}

TEST(MeasureRgbRegion, DistinguishesEmptyOutsideAndInvalidRegions)
{
    const auto image = gradient(96, 72);
    EXPECT_EQ(measureRgbRegion(image, rectangle(0, 0), {40, 30}).status, RgbRegionStatus::NoPixels);
    EXPECT_EQ(measureRgbRegion(image, ellipse(20, 0), {40, 30}).status, RgbRegionStatus::NoPixels);
    EXPECT_EQ(measureRgbRegion(image, ellipse(-4, 6), {40, 30}).status, RgbRegionStatus::NoPixels);
    EXPECT_EQ(measureRgbRegion(image, rectangle(5, -1), {40, 30}).status, RgbRegionStatus::NoPixels);
    // At the origin the outline is not translated (Java's identity transform) and is still measured.
    const auto origin = measureRgbRegion(image, polygon({{.x = 0, .y = 0}, {.x = 4, .y = 0}, {.x = 0, .y = 4}}), {.x = 0, .y = 0});
    ASSERT_EQ(origin.status, RgbRegionStatus::Measured);
    EXPECT_EQ(origin.bounds, (PixelRect{0, 0, 4, 4}));
    EXPECT_EQ(origin.statistics.pixels, 6); // centres strictly left of the hypotenuse x + y = 4
    EXPECT_EQ(measureRgbRegion(image, polygon({{0, 0}, {10, 10}, {20, 20}}), {48, 36}).status, RgbRegionStatus::NoPixels);
    // Bounds reaching x = -1 are rejected although every included centre is inside the image.
    const auto left = measureRgbRegion(image, ellipse(5, 5), {.x = 1.9, .y = 30});
    EXPECT_EQ(left.status, RgbRegionStatus::OutsideImage);
    EXPECT_EQ(left.bounds, (PixelRect{-1, 28, 6, 5}));
    EXPECT_EQ(measureRgbRegion(image, ellipse(5, 5), {2.0, 30}).status, RgbRegionStatus::Measured);
    EXPECT_EQ(measureRgbRegion(image, rectangle(96, 72), {48, 36}).statistics.pixels, 96 * 72);
    EXPECT_EQ(measureRgbRegion(image, rectangle(96, 72), {48, 36.5}).status, RgbRegionStatus::OutsideImage);
    EXPECT_EQ(measureRgbRegion(image, rectangle(10, 10), {48, -3}).status, RgbRegionStatus::OutsideImage);
    EXPECT_EQ(measureRgbRegion(image, rectangle(10, 10), {3e9, 30}).status, RgbRegionStatus::OutsideImage);
    const auto invalid = measureRgbRegion(image, rectangle(10, 10), {.x = NOT_A_NUMBER, .y = 30});
    EXPECT_EQ(invalid.status, RgbRegionStatus::InvalidGeometry);
    EXPECT_EQ(invalid.bounds, (PixelRect{}));
}

TEST(MeasureRgbRegion, KeepsExactDeviationsWhereTrackerIntSumsOverflow)
{
    // 181^2 = 32761 white pixels fit Tracker's int sum of squares; 182^2 = 33124 do not.
    const auto white = makeImage(260, 260, [](int, int) { return rgb(255, 255, 255); });
    const auto fits = measureRgbRegion(white, rectangle(181, 181), {.x = 130.5, .y = 130.5});
    ASSERT_EQ(fits.status, RgbRegionStatus::Measured);
    EXPECT_EQ(fits.statistics.pixels, 32761);
    EXPECT_FALSE(fits.trackerSumOverflow);
    EXPECT_EQ(fits.statistics.redDeviation, 0.0);
    const auto overflows = measureRgbRegion(white, rectangle(182, 182), {.x = 130, .y = 130});
    ASSERT_EQ(overflows.status, RgbRegionStatus::Measured);
    EXPECT_TRUE(overflows.trackerSumOverflow);
    EXPECT_EQ(overflows.statistics.squareSums[1], 33124LL * 255 * 255);
    EXPECT_EQ(overflows.statistics.red, 255.0);
    EXPECT_EQ(overflows.statistics.redDeviation, 0.0);
    EXPECT_EQ(overflows.statistics.greenDeviation, 0.0);
    EXPECT_EQ(overflows.statistics.blueDeviation, 0.0);
}

TEST(MeasureRgbRegion, KeepsExactMeansWhereTrackerChannelSumsOverflow)
{
    // 2902^2 = 8,421,604 white pixels: 255 n exceeds 2^31 - 1, so Tracker's int sums wrap the means.
    const auto white = makeImage(2904, 2904, [](int, int) { return rgb(255, 255, 255); });
    const auto huge = measureRgbRegion(white, rectangle(2902, 2902), {.x = 1452, .y = 1452});
    ASSERT_EQ(huge.status, RgbRegionStatus::Measured);
    EXPECT_EQ(huge.statistics.pixels, 2902LL * 2902);
    EXPECT_GT(huge.statistics.sums[0], std::numeric_limits<std::int32_t>::max());
    EXPECT_TRUE(huge.trackerSumOverflow);
    EXPECT_EQ(huge.statistics.red, 255.0);
    EXPECT_EQ(huge.statistics.luma, trackerLuma(255, 255, 255));
    EXPECT_EQ(huge.statistics.greenDeviation, 0.0);
    // What Tracker's 32-bit sum gives instead: a negative "mean".
    const auto wrapped = static_cast<std::int32_t>(static_cast<std::uint32_t>(huge.statistics.sums[0]));
    EXPECT_LT(static_cast<double>(wrapped) / static_cast<double>(huge.statistics.pixels), 0.0);
}

TEST(LineProfileMode, MatchesTrackersAxisTest)
{
    EXPECT_EQ(lineProfileMode(true, 0.7), LineProfileMode::Horizontal);
    EXPECT_EQ(lineProfileMode(false, 0.0), LineProfileMode::Horizontal);
    EXPECT_EQ(lineProfileMode(false, 5e-6), LineProfileMode::Horizontal);
    EXPECT_EQ(lineProfileMode(false, std::numbers::pi), LineProfileMode::Horizontal);
    EXPECT_EQ(lineProfileMode(false, 2e-5), LineProfileMode::Tilted);
    EXPECT_EQ(lineProfileMode(false, -0.5), LineProfileMode::Tilted);
}

TEST(HorizontalProfile, SamplesTruncatedColumnsOnOneRow)
{
    const auto image = gradient();
    // Ends 10.7 and 30.2 truncate to columns 10 and 30; column 30 is excluded.
    const auto data = sampleLineProfile(image, {.x = 10.7, .y = 20.4}, {.x = 30.2, .y = 20.4}, 1, LineProfileMode::Horizontal);
    ASSERT_EQ(data.status, LineProfileStatus::Measured);
    ASSERT_EQ(data.samples.size(), 20U);
    for (int i = 0; i < 20; ++i)
    {
        const auto& s = data.samples[static_cast<std::size_t>(i)];
        EXPECT_EQ(s.index, i);
        EXPECT_EQ(s.position.x, 10 + i + 0.5);
        EXPECT_EQ(s.position.y, 20.5);
        EXPECT_EQ(s.red, 10.0 + i);
        EXPECT_EQ(s.green, 20.0); // mean of rows 19, 20, 21
        EXPECT_EQ(s.blue, 30.0 + i);
        EXPECT_EQ(s.pixels, 3.0);
        EXPECT_EQ(s.luma, trackerLuma(s.red, s.green, s.blue));
    }
    EXPECT_FALSE(data.trackerOmitsBlackStrip);
    // Reversed ends give the same samples; end1.y is ignored.
    const auto reversed = sampleLineProfile(image, {.x = 30.2, .y = 20.4}, {.x = 10.7, .y = 90}, 1, LineProfileMode::Horizontal);
    ASSERT_EQ(reversed.samples.size(), 20U);
    EXPECT_EQ(reversed.samples[19].red, 29.0);
    // Truncation toward zero: x = -0.9 and y = -0.5 address column 0 and row 0.
    const auto truncated = sampleLineProfile(image, {.x = -0.9, .y = -0.5}, {.x = 8, .y = -0.5}, 0, LineProfileMode::Horizontal);
    ASSERT_EQ(truncated.samples.size(), 8U);
    EXPECT_EQ(truncated.samples[0].position.y, 0.5);
    // Huge coordinates saturate like Java's (int) cast before clamping.
    EXPECT_EQ(sampleLineProfile(image, {-1e12, 5}, {10, 5}, 0, LineProfileMode::Horizontal).samples.size(), 10U);
    // The far end is clamped to the image width.
    const auto clamped = sampleLineProfile(image, {.x = 110.5, .y = 50.2}, {.x = 400, .y = 50.2}, 3, LineProfileMode::Horizontal);
    ASSERT_EQ(clamped.samples.size(), 18U);
    EXPECT_EQ(clamped.samples.back().red, 127.0);
    EXPECT_EQ(clamped.samples.back().pixels, 7.0);
}

TEST(HorizontalProfile, ReportsEmptyOutsideAndBlackStrips)
{
    const auto image = gradient();
    EXPECT_EQ(sampleLineProfile(image, {10.2, 5}, {10.9, 5}, 0, LineProfileMode::Horizontal).status, LineProfileStatus::TooShort);
    EXPECT_EQ(sampleLineProfile(image, {-20, 5}, {-5, 5}, 0, LineProfileMode::Horizontal).status, LineProfileStatus::OutsideImage);
    EXPECT_EQ(sampleLineProfile(image, {130, 5}, {200, 5}, 0, LineProfileMode::Horizontal).status, LineProfileStatus::OutsideImage);
    EXPECT_EQ(sampleLineProfile(image, {5.5, 1.5}, {25.5, 1.5}, 3, LineProfileMode::Horizontal).status, LineProfileStatus::OutsideImage);
    EXPECT_EQ(sampleLineProfile(image, {5.5, 95.5}, {25.5, 95.5}, 0, LineProfileMode::Horizontal).status, LineProfileStatus::Measured);
    EXPECT_EQ(sampleLineProfile(image, {5.5, 95.5}, {25.5, 95.5}, 1, LineProfileMode::Horizontal).status, LineProfileStatus::OutsideImage);
    EXPECT_EQ(sampleLineProfile(image, {5, 1e12}, {25, 1e12}, 0, LineProfileMode::Horizontal).status, LineProfileStatus::OutsideImage);
    EXPECT_EQ(sampleLineProfile(image, {5, 5}, {25, NOT_A_NUMBER}, 0, LineProfileMode::Horizontal).status,
              LineProfileStatus::InvalidGeometry);
    EXPECT_EQ(sampleLineProfile(image, {5, 5}, {25, 5}, -1, LineProfileMode::Horizontal).status, LineProfileStatus::InvalidGeometry);
    EXPECT_EQ(sampleLineProfile(image, {5, 50}, {25, 50}, LINE_PROFILE_MAX_SPREAD + 1, LineProfileMode::Horizontal).status,
              LineProfileStatus::InvalidGeometry);
    // An all-black strip has real zero values (Tracker shows no data for it).
    const auto band = makeImage(64, 48, [](int x, int y) { return y < 24 ? 0U : rgb(x, y, 50); });
    const auto black = sampleLineProfile(band, {.x = 5, .y = 10}, {.x = 40, .y = 10}, 2, LineProfileMode::Horizontal);
    ASSERT_EQ(black.status, LineProfileStatus::Measured);
    EXPECT_TRUE(black.trackerOmitsBlackStrip);
    ASSERT_EQ(black.samples.size(), 35U);
    EXPECT_EQ(black.samples[0].red, 0.0);
    EXPECT_EQ(black.samples[0].luma, 0.0);
    const auto edge = sampleLineProfile(band, {.x = 5, .y = 22}, {.x = 40, .y = 22}, 2, LineProfileMode::Horizontal);
    EXPECT_FALSE(edge.trackerOmitsBlackStrip);
    EXPECT_EQ(edge.samples[0].red, 5.0 / 5); // only row 24 of rows 20..24 is lit
}

/// Samples of a tilted profile must equal the exact area-weighted means of their probes.
void expectExactProbes(const ArgbImage& image, ImagePoint end0, ImagePoint end1, int spread, std::size_t expectedSamples)
{
    const auto data = sampleLineProfile(image, end0, end1, spread, LineProfileMode::Tilted);
    ASSERT_EQ(data.status, LineProfileStatus::Measured);
    ASSERT_EQ(data.samples.size(), expectedSamples);
    const double theta = std::atan2(end1.y - end0.y, end1.x - end0.x);
    for (const auto& s : data.samples)
    {
        const auto expected = exactMean(image, probeCorners(s.position, theta, 2 * spread + 1));
        EXPECT_NEAR(s.red, expected[0], 1e-9) << "sample " << s.index;
        EXPECT_NEAR(s.green, expected[1], 1e-9) << "sample " << s.index;
        EXPECT_NEAR(s.blue, expected[2], 1e-9) << "sample " << s.index;
        EXPECT_EQ(s.pixels, 2.0 * spread + 1);
    }
}

TEST(TiltedProfile, WeightsPixelsByExactProbeOverlap)
{
    const auto image = gradient();
    // Every direction quadrant, including the ones Tracker gets wrong (sin * cos < 0).
    expectExactProbes(image, {.x = 15.37, .y = 30.52}, {.x = 55.81, .y = 60.07}, 1, 50);
    expectExactProbes(image, {.x = 15.37, .y = 60.07}, {.x = 55.81, .y = 30.52}, 1, 50);
    expectExactProbes(image, {.x = 55.81, .y = 60.07}, {.x = 15.37, .y = 30.52}, 2, 50);
    expectExactProbes(image, {.x = 55.81, .y = 30.52}, {.x = 15.37, .y = 60.07}, 0, 50);
    // Grid-aligned probes, on which Tracker throws.
    expectExactProbes(image, {.x = 40, .y = 10}, {.x = 40, .y = 40}, 1, 30);
    expectExactProbes(image, {.x = 40.5, .y = 10.25}, {.x = 40.5, .y = 40.25}, 0, 30);
    expectExactProbes(image, {.x = 10, .y = 10}, {.x = 40, .y = 40}, 1, 42);
    expectExactProbes(image, {.x = 10.25, .y = 30.5}, {.x = 50.75, .y = 30.5}, 1, 40);
    // A hard vertical edge (independent of the slice integration: a midpoint lattice).
    const auto edge = makeImage(64, 48, [](int x, int) { return x < 32 ? rgb(200, 100, 50) : rgb(20, 40, 240); });
    const auto crossing = sampleLineProfile(edge, {.x = 20.13, .y = 30.18}, {.x = 44.07, .y = 10.29}, 2, LineProfileMode::Tilted);
    ASSERT_EQ(crossing.status, LineProfileStatus::Measured);
    const double theta = std::atan2(10.29 - 30.18, 44.07 - 20.13);
    for (const auto& s : crossing.samples)
    {
        const auto lattice = latticeMean(edge, probeCorners(s.position, theta, 5), 512);
        EXPECT_NEAR(s.red, lattice[0], 0.05) << "sample " << s.index;
        EXPECT_NEAR(s.blue, lattice[2], 0.05) << "sample " << s.index;
    }
}

TEST(TiltedProfile, StepsTheProbeLikeTracker)
{
    const auto uniform = makeImage(96, 72, [](int, int) { return rgb(17, 130, 255); });
    // Sample centres advance by one pixel along the line from end0.
    const ImagePoint end0{.x = 20.37, .y = 20.61};
    const ImagePoint end1{.x = 47.91, .y = 29.12};
    const auto data = sampleLineProfile(uniform, end0, end1, 3, LineProfileMode::Tilted);
    ASSERT_EQ(data.status, LineProfileStatus::Measured);
    const double length = std::hypot(end1.x - end0.x, end1.y - end0.y);
    ASSERT_EQ(data.samples.size(), static_cast<std::size_t>(std::floor(length)));
    const double ux = (end1.x - end0.x) / length;
    const double uy = (end1.y - end0.y) / length;
    for (const auto& s : data.samples)
    {
        EXPECT_NEAR(s.position.x, end0.x + (s.index + 0.5) * ux, 1e-9);
        EXPECT_NEAR(s.position.y, end0.y + (s.index + 0.5) * uy, 1e-9);
        EXPECT_NEAR(s.red, 17.0, 1e-12);
        EXPECT_NEAR(s.green, 130.0, 1e-12);
        EXPECT_NEAR(s.blue, 255.0, 1e-12);
        EXPECT_EQ(s.pixels, 7.0);
    }
    // A start outside the image advances by whole pixels; a far end outside shortens the profile.
    const auto clipped = sampleLineProfile(uniform, {.x = -5.37, .y = 20.25}, {.x = 30.75, .y = 35.5}, 1, LineProfileMode::Tilted);
    ASSERT_EQ(clipped.status, LineProfileStatus::Measured);
    const double clippedLength = std::hypot(36.12, 15.25);
    EXPECT_LT(clipped.samples.size(), static_cast<std::size_t>(std::floor(clippedLength)));
    for (const auto& s : clipped.samples)
    {
        EXPECT_GE(s.position.x, 0.0);
        EXPECT_NEAR(s.red, 17.0, 1e-12);
    }
    EXPECT_EQ(sampleLineProfile(uniform, {70.31, 50.27}, {110.25, 75}, 1, LineProfileMode::Tilted).samples.size(), 28U);
    EXPECT_EQ(sampleLineProfile(uniform, {10.3, 10.2}, {10.9, 10.7}, 0, LineProfileMode::Tilted).status, LineProfileStatus::TooShort);
    EXPECT_EQ(sampleLineProfile(uniform, {-10.3, -10.6}, {-2.2, -3.1}, 0, LineProfileMode::Tilted).status, LineProfileStatus::OutsideImage);
    EXPECT_EQ(sampleLineProfile(uniform, {0, 0}, {LINE_PROFILE_MAX_LENGTH * 2, 0}, 0, LineProfileMode::Tilted).status,
              LineProfileStatus::InvalidGeometry);
    EXPECT_EQ(sampleLineProfile(uniform, {0, 0}, {1e308, -1e308}, 0, LineProfileMode::Tilted).status, LineProfileStatus::InvalidGeometry);
    EXPECT_EQ(sampleLineProfile(ArgbImage(), {1, 1}, {5, 5}, 0, LineProfileMode::Tilted).status, LineProfileStatus::OutsideImage);
    // Probes along the left and top edges, with corners exactly on the image boundary.
    const auto left = sampleLineProfile(uniform, {.x = 0.5, .y = 10}, {.x = 0.5, .y = 40}, 0, LineProfileMode::Tilted);
    ASSERT_EQ(left.status, LineProfileStatus::Measured);
    EXPECT_EQ(left.samples.size(), 30U);
    EXPECT_NEAR(left.samples.back().red, 17.0, 1e-12);
    const auto top = sampleLineProfile(uniform, {.x = 40, .y = 0.5}, {.x = 10, .y = 0.5}, 0, LineProfileMode::Tilted);
    ASSERT_EQ(top.status, LineProfileStatus::Measured);
    EXPECT_EQ(top.samples.size(), 30U);
    EXPECT_NEAR(top.samples.back().blue, 255.0, 1e-12);
    EXPECT_EQ(sampleLineProfile(uniform, {std::numeric_limits<double>::infinity(), 1}, {5, 5}, 0, LineProfileMode::Tilted).status,
              LineProfileStatus::InvalidGeometry);
}

} // namespace
