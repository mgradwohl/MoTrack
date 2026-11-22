// Behavior of motrack_matching through its public API: construction, evolution, searches, line
// searches, classification and the deliberate differences from Tracker, on small synthetic frames.
// tests/science/template_matcher_reference_test.cpp compares the same API with the Java oracle.

#include "science/matching/template_matcher.hpp"

#include "science/matching/matching_internal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{

using motrack::science::ArgbImage;
using motrack::science::ArgbPixel;
using motrack::science::MaskShape;
using motrack::science::MatchMask;
using motrack::science::MatchOptions;
using motrack::science::MatchQuality;
using motrack::science::MatchResult;
using motrack::science::MatchStatus;
using motrack::science::MatchTemplate;
using motrack::science::PixelPoint;
using motrack::science::PixelRect;
using motrack::science::SearchLine;
using motrack::science::TemplateMatcher;
namespace detail = motrack::science::matching_detail;

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();
constexpr double INFINITE = std::numeric_limits<double>::infinity();

ArgbPixel rgb(int red, int green, int blue)
{
    const auto byte = [](int value)
    {
        return static_cast<ArgbPixel>(std::clamp(value, 0, 255));
    };
    return (byte(red) << 16U) | (byte(green) << 8U) | byte(blue);
}

/// A smooth, textured disc of radius 10 centered at (cx, cy) over a gray background.
ArgbImage blobFrame(int width, int height, double cx, double cy)
{
    ArgbImage frame(width, height);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const double u = x + 0.5 - cx;
            const double v = y + 0.5 - cy;
            const double r2 = u * u + v * v;
            const double weight = r2 < 100 ? (1 - r2 / 100) : 0.0;
            const double texture = 0.5 + 0.25 * std::sin(u * 0.45) + 0.25 * std::cos(v * 0.35);
            frame.setPixel(
                x,
                y,
                rgb(60 + static_cast<int>(170 * weight * texture), 90 - static_cast<int>(60 * weight), 70 + static_cast<int>(90 * weight)));
        }
    }
    return frame;
}

constexpr PixelRect rect(int x, int y, int width, int height)
{
    return {.x = x, .y = y, .width = width, .height = height};
}

MatchMask circle(double cx, double cy, double radius)
{
    return MatchMask::fromCenter(MaskShape::Ellipse, cx, cy, cx + radius, cy + radius);
}

TemplateMatcher keyMatcher(const ArgbImage& frame, const MatchMask& mask)
{
    auto matcher = TemplateMatcher::create(frame, mask);
    if (!matcher)
    {
        throw std::runtime_error("no template");
    }
    return *matcher;
}

// ---------------------------------------------------------------------------------------------
// Images, masks and Java primitives.

TEST(TemplateMatcher, ImagesValidateTheirSizeAndExposePixels)
{
    EXPECT_THROW(ArgbImage(-1, 3), std::invalid_argument);
    EXPECT_THROW(ArgbImage(3, -1), std::invalid_argument);
    EXPECT_THROW(ArgbImage(1 << 16, 1 << 16), std::invalid_argument);
    EXPECT_THROW(ArgbImage(2, 2, std::vector<ArgbPixel>(3)), std::invalid_argument);
    EXPECT_TRUE(ArgbImage().empty());
    EXPECT_TRUE(ArgbImage(0, 5).empty());
    ArgbImage image(3, 2, std::vector<ArgbPixel>{1, 2, 3, 4, 5, 6});
    EXPECT_FALSE(image.empty());
    EXPECT_EQ(image.width(), 3);
    EXPECT_EQ(image.height(), 2);
    EXPECT_EQ(image.pixel(2, 1), 6U);
    image.setPixel(0, 1, 9);
    image.pixels()[1] = 7;
    const auto& constant = image;
    EXPECT_EQ(constant.pixels()[3], 9U);
    EXPECT_EQ(constant.pixel(1, 0), 7U);
    EXPECT_EQ(image, ArgbImage(3, 2, std::vector<ArgbPixel>{1, 7, 3, 9, 5, 6}));
}

TEST(TemplateMatcher, BoundsFollowRectangularShapeGetBounds)
{
    EXPECT_EQ(motrack::science::frameBounds(2.5, -1.25, 3.0, 2.0), (PixelRect{2, -2, 4, 3}));
    EXPECT_EQ(motrack::science::frameBounds(2, 3, 4, 5), (PixelRect{2, 3, 4, 5}));
    EXPECT_EQ(motrack::science::frameBounds(2, 3, -1, 5), PixelRect{});
    EXPECT_EQ(motrack::science::frameBounds(2, 3, 0, 0), (PixelRect{2, 3, 0, 0}));
    EXPECT_FALSE(motrack::science::frameBounds(NOT_A_NUMBER, 0, 1, 1));
    EXPECT_FALSE(motrack::science::frameBounds(0, 0, INFINITE, 1));
    EXPECT_FALSE(motrack::science::frameBounds(0, 0, 1, INFINITE));
    EXPECT_FALSE(motrack::science::frameBounds(0, INFINITE, 1, 1));
    EXPECT_FALSE(motrack::science::frameBounds(3e9, 0, 1, 1));
    EXPECT_FALSE(motrack::science::frameBounds(0, -3e9, 1, 1));
    EXPECT_FALSE(motrack::science::frameBounds(-2e9, 0, 3e9, 1));
    EXPECT_FALSE(motrack::science::frameBounds(0, -2e9, 1, 3e9));
    const auto mask = MatchMask::fromCenter(MaskShape::Rectangle, 10.5, 20.25, 6.5, 23.25);
    EXPECT_EQ(mask, (MatchMask{.shape = MaskShape::Rectangle, .x = 6.5, .y = 17.25, .width = 8.0, .height = 6.0}));
    EXPECT_EQ(motrack::science::maskBounds(mask), (PixelRect{6, 17, 9, 7}));
}

TEST(TemplateMatcher, MaskPixelsNeedAllFourCornersInside)
{
    // Path2D containment is half-open: an integer rectangle excludes its last row and column.
    const auto rectangle = motrack::science::maskPixels({.shape = MaskShape::Rectangle, .x = 3, .y = 4, .width = 3, .height = 2});
    EXPECT_EQ(rectangle, (std::vector<std::uint8_t>{1, 1, 0, 0, 0, 0}));
    // A 3 x 3 circle includes only its center pixel; a 2 x 2 circle includes nothing.
    EXPECT_EQ(motrack::science::maskPixels({.shape = MaskShape::Ellipse, .x = 0, .y = 0, .width = 3, .height = 3}),
              (std::vector<std::uint8_t>{0, 0, 0, 0, 1, 0, 0, 0, 0}));
    EXPECT_EQ(motrack::science::maskPixels({.shape = MaskShape::Ellipse, .x = 7, .y = 7, .width = 2, .height = 2}),
              (std::vector<std::uint8_t>(4, 0)));
    EXPECT_TRUE(motrack::science::maskPixels({.shape = MaskShape::Ellipse, .x = NOT_A_NUMBER, .y = 0, .width = 3, .height = 3}).empty());
    EXPECT_TRUE(motrack::science::maskPixels({.shape = MaskShape::Ellipse, .x = 0, .y = 0, .width = 5000, .height = 5000}).empty());
    // Containment of the translated shape itself.
    const MatchMask ellipse{.shape = MaskShape::Ellipse, .x = 10.5, .y = 20.5, .width = 9, .height = 9};
    EXPECT_TRUE(detail::maskContains(ellipse, {10, 20}, 5.0, 5.0));
    EXPECT_FALSE(detail::maskContains(ellipse, {10, 20}, 0.6, 0.6));
    EXPECT_FALSE(detail::maskContains(ellipse, {10, 20}, NOT_A_NUMBER, 5.0));
    EXPECT_FALSE(detail::maskContains(ellipse, {10, 20}, 5.0, INFINITE));
    EXPECT_FALSE(detail::maskContains({.shape = MaskShape::Ellipse, .x = 0, .y = 0, .width = -1, .height = 4}, {}, 0, 1));
    EXPECT_FALSE(detail::maskContains({.shape = MaskShape::Rectangle, .x = 0, .y = 0, .width = 2, .height = -1}, {}, 1, 0));
    // A NaN frame yields no crossings.
    EXPECT_FALSE(detail::maskContains({.shape = MaskShape::Ellipse, .x = NOT_A_NUMBER, .y = 0, .width = 4, .height = 4}, {}, 1, 1));
}

TEST(TemplateMatcher, JavaRoundingAndOrderingSemantics)
{
    EXPECT_EQ(detail::javaRoundToInt(2.5), 3);
    EXPECT_EQ(detail::javaRoundToInt(-2.5), -2);
    EXPECT_EQ(detail::javaRoundToInt(-0.5), 0);
    EXPECT_EQ(detail::javaRoundToInt(0.49999999999999994), 0);
    EXPECT_EQ(detail::javaRoundToInt(-1.5000000000000002), -2);
    EXPECT_EQ(detail::javaRoundToInt(NOT_A_NUMBER), 0);
    EXPECT_EQ(detail::javaRoundToInt(1e300), std::numeric_limits<int>::max());
    EXPECT_EQ(detail::javaRoundToInt(-1e300), std::numeric_limits<int>::min());
    EXPECT_TRUE(detail::javaDoubleLess(-0.0, 0.0));
    EXPECT_FALSE(detail::javaDoubleLess(0.0, -0.0));
    EXPECT_FALSE(detail::javaDoubleLess(0.0, 0.0));
    EXPECT_TRUE(detail::javaDoubleLess(1.0, NOT_A_NUMBER));
    EXPECT_FALSE(detail::javaDoubleLess(NOT_A_NUMBER, NOT_A_NUMBER));
    EXPECT_FALSE(detail::javaDoubleLess(2.0, 1.0));
    MatchResult result;
    result.x = 4.5;
    result.y = -3.5;
    EXPECT_EQ(motrack::science::evolutionOrigin(result), (PixelPoint{5, -3}));
}

TEST(TemplateMatcher, AlphaTablesAndSrcOverFollowLibawt)
{
    EXPECT_EQ(detail::mul8(0, 200), 0);
    EXPECT_EQ(detail::mul8(255, 200), 200);
    EXPECT_EQ(detail::mul8(128, 128), 64);
    EXPECT_EQ(detail::div8(64, 128), 128);
    EXPECT_EQ(detail::div8(200, 100), 255);
    for (int alpha = 0; alpha <= 255; ++alpha)
    {
        EXPECT_EQ(detail::compositeExtraAlpha(alpha), alpha);
    }
    // Fully transparent results keep the destination; opaque sources replace it.
    EXPECT_EQ(detail::srcOver(0x00ffffffU, 0x80123456U, 255), 0x80123456U);
    EXPECT_EQ(detail::srcOver(0xff102030U, 0x80123456U, 255), 0xff102030U);
    EXPECT_EQ(detail::srcOver(0xff102030U, 0xff123456U, 0), 0xff123456U);
    // Half blending onto an opaque pixel stays opaque.
    EXPECT_EQ(detail::srcOver(0xffff0000U, 0xff0000ffU, 128) >> 24U, 0xffU);
    // A translucent result divides by its alpha.
    EXPECT_EQ(detail::srcOver(0xffff0000U, 0x00000000U, 128), 0x80ff0000U);
}

TEST(TemplateMatcher, JavaTrigonometryIsCorrectlyRoundedAndPlatformIndependent)
{
    // Values Java's Math returns (HotSpot x86-64; correctly rounded for these arguments).
    EXPECT_EQ(detail::javaTan(std::numbers::pi / 4), 0.9999999999999999);
    EXPECT_EQ(detail::javaTan(-std::numbers::pi / 4), -0.9999999999999999);
    EXPECT_EQ(detail::javaTan(std::numbers::pi / 2), 1.633123935319537E16);
    EXPECT_EQ(detail::javaTan(std::numbers::pi), -1.2246467991473532E-16);
    EXPECT_EQ(detail::javaTan(0.3), 0.30933624960962325);
    EXPECT_EQ(detail::javaTan(2.0), -2.185039863261519);
    EXPECT_EQ(detail::javaTan(-1.0), -1.5574077246549023);
    EXPECT_EQ(detail::javaSin(std::numbers::pi), 1.2246467991473532E-16);
    EXPECT_EQ(detail::javaCos(std::numbers::pi / 2), 6.123233995736766E-17);
    EXPECT_EQ(detail::javaCos(std::numbers::pi), -1.0);
    EXPECT_EQ(detail::javaSin(-std::numbers::pi / 2), -1.0);
    EXPECT_EQ(detail::javaTan(1e-11), 1e-11);
    // Signed zeros, non-finite and far arguments follow <cmath>.
    EXPECT_TRUE(std::signbit(detail::javaSin(-0.0)));
    EXPECT_TRUE(std::signbit(detail::javaTan(-0.0)));
    EXPECT_EQ(detail::javaCos(0.0), 1.0);
    EXPECT_TRUE(std::isnan(detail::javaSin(NOT_A_NUMBER)));
    EXPECT_TRUE(std::isnan(detail::javaCos(INFINITE)));
    EXPECT_TRUE(std::isnan(detail::javaTan(-INFINITE)));
    EXPECT_EQ(detail::javaSin(1e300), std::sin(1e300));
    EXPECT_EQ(detail::javaCos(-1e300), std::cos(-1e300));
    EXPECT_EQ(detail::javaTan(3e7), std::tan(3e7));
    // Every quadrant: sin^2 + cos^2 = 1 and tan = sin / cos to rounding.
    for (const double angle : {0.7, 2.4, -2.4, -0.7, 4.0, -4.0, 1000.25})
    {
        const double s = detail::javaSin(angle);
        const double c = detail::javaCos(angle);
        EXPECT_NEAR(s * s + c * c, 1.0, 1e-15) << angle;
        EXPECT_NEAR(detail::javaTan(angle), s / c, 4e-16 * std::abs(s / c)) << angle;
        EXPECT_NEAR(s, std::sin(angle), 2e-16) << angle;
    }
}

TEST(TemplateMatcher, ParabolaFitsThreeEquallySpacedPoints)
{
    const auto fit = detail::fitParabola(9.0, 4.0, 3.0); // 4 - 3x + 2x^2
    EXPECT_NEAR(fit.c0, 4.0, 1e-12);
    EXPECT_NEAR(fit.c1, -3.0, 1e-12);
    EXPECT_NEAR(fit.c2, 2.0, 1e-12);
    const auto flat = detail::fitParabola(5.0, 5.0, 5.0);
    EXPECT_EQ(flat.c2, 0.0);
    EXPECT_TRUE(std::isnan(detail::fitParabola(NOT_A_NUMBER, 1.0, 2.0).c1));
}

// ---------------------------------------------------------------------------------------------
// Templates and evolution.

TEST(TemplateMatcher, CreatesTrimmedTemplatesFromTheMaskBounds)
{
    const auto frame = blobFrame(60, 40, 30.3, 20.6);
    const auto mask = circle(30.3, 20.6, 7.5);
    const auto matcher = keyMatcher(frame, mask);
    EXPECT_EQ(matcher.mask(), mask);
    EXPECT_EQ(motrack::science::maskBounds(mask), matcher.bounds());
    EXPECT_EQ(matcher.alphas(), (std::array{255, 0}));
    const auto& matchTemplate = matcher.matchTemplate();
    EXPECT_EQ(matchTemplate.trimLeft, 1);
    EXPECT_EQ(matchTemplate.trimTop, 1);
    EXPECT_LT(matchTemplate.image.width(), matcher.bounds().width);
    // The original is the opaque crop; the working image clears excluded pixels.
    const auto bounds = matcher.bounds();
    EXPECT_EQ(matcher.original().pixel(0, 0), 0xff000000U | frame.pixel(bounds.x, bounds.y));
    EXPECT_EQ(matcher.working().pixel(0, 0), 0U);
    const auto included = motrack::science::maskPixels(mask);
    for (int y = 0; y < matchTemplate.image.height(); ++y)
    {
        for (int x = 0; x < matchTemplate.image.width(); ++x)
        {
            const auto pixel = matchTemplate.image.pixel(x, y);
            const auto index = static_cast<std::size_t>(y + matchTemplate.trimTop) * static_cast<std::size_t>(bounds.width) +
                               static_cast<std::size_t>(x + matchTemplate.trimLeft);
            EXPECT_EQ(pixel >> 24U, included[index] != 0 ? 0xffU : 0U);
        }
    }
}

TEST(TemplateMatcher, RefusesMasksWithoutPixels)
{
    const auto frame = blobFrame(20, 20, 10, 10);
    EXPECT_FALSE(TemplateMatcher::create(frame, {.shape = MaskShape::Ellipse, .x = 5, .y = 5, .width = 2, .height = 2}));
    EXPECT_FALSE(TemplateMatcher::create(frame, {.shape = MaskShape::Ellipse, .x = 5, .y = 5, .width = 0, .height = 4}));
    EXPECT_FALSE(TemplateMatcher::create(frame, {.shape = MaskShape::Ellipse, .x = 5, .y = 5, .width = -3, .height = 4}));
    EXPECT_FALSE(TemplateMatcher::create(frame, {.shape = MaskShape::Ellipse, .x = INFINITE, .y = 5, .width = 3, .height = 4}));
    EXPECT_FALSE(TemplateMatcher::create(frame, {.shape = MaskShape::Rectangle, .x = 0, .y = 0, .width = 4096, .height = 4096}));
    // A mask entirely outside the frame still builds an (all black) template, as in Tracker.
    const auto outside = TemplateMatcher::create(frame, circle(100, 100, 4));
    if (!outside)
    {
        FAIL() << "a mask outside the frame still builds a template";
    }
    EXPECT_EQ(outside->matchTemplate().image.pixel(3, 3), 0xff000000U);
}

TEST(TemplateMatcher, EvolutionBlendsTowardTheNewCropAndTethersBack)
{
    const auto first = blobFrame(60, 40, 30, 20);
    ArgbImage changed(60, 40, rgb(250, 10, 10));
    const auto mask = circle(30, 20, 6);
    auto matcher = keyMatcher(first, mask);
    const auto original = matcher.matchTemplate();
    const auto center = [](const TemplateMatcher& m)
    {
        const auto& image = m.matchTemplate().image;
        return image.pixel(image.width() / 2, image.height() / 2);
    };
    const PixelPoint origin{.x = matcher.bounds().x, .y = matcher.bounds().y};
    // Both alphas zero: Java returns the existing template without recording the alphas.
    EXPECT_FALSE(matcher.evolve(changed, origin, 0, 0));
    EXPECT_EQ(matcher.alphas(), (std::array{255, 0}));
    EXPECT_EQ(matcher.matchTemplate(), original);
    // 20% evolution moves the template a fifth of the way to the new crop.
    ASSERT_TRUE(matcher.evolve(changed, origin, motrack::science::alphaFromPercent(20), 0));
    EXPECT_EQ(matcher.alphas(), (std::array{51, 0}));
    const auto before = center(TemplateMatcher(keyMatcher(first, mask)));
    const auto after = center(matcher);
    const int expectedRed = detail::mul8(204, static_cast<int>((before >> 16U) & 0xffU)) + detail::mul8(51, 250);
    EXPECT_EQ(static_cast<int>((after >> 16U) & 0xffU), expectedRed);
    EXPECT_EQ(after >> 24U, 0xffU);
    // Full tethering restores the original template; clamped and negative alphas are recorded as passed.
    ASSERT_TRUE(matcher.evolve(changed, origin, -5, 300));
    EXPECT_EQ(matcher.alphas(), (std::array{-5, 300}));
    EXPECT_EQ(matcher.matchTemplate(), original);
    // A crop beyond the frame evolves toward black.
    ASSERT_TRUE(matcher.evolve(changed, {500, 500}, 255, 0));
    EXPECT_EQ(center(matcher), 0xff000000U);
}

// ---------------------------------------------------------------------------------------------
// Searches.

TEST(TemplateMatcher, FindsSubpixelTranslationsWithTrackerScores)
{
    const auto key = blobFrame(80, 60, 37.4, 28.7);
    const auto mask = circle(37.4, 28.7, 7);
    const auto matcher = keyMatcher(key, mask);
    const auto bounds = matcher.bounds();
    // The key frame itself is a perfect match: infinite score, no fitted width.
    const auto exact = motrack::science::findMatch(matcher.matchTemplate(), key, rect(27, 19, 20, 20));
    ASSERT_EQ(exact.status, MatchStatus::Matched);
    EXPECT_EQ(exact.pixel, (PixelPoint{bounds.x, bounds.y}));
    EXPECT_EQ(exact.x, bounds.x);
    EXPECT_TRUE(std::isinf(exact.peakHeight));
    EXPECT_TRUE(std::isnan(exact.peakWidth));
    EXPECT_FALSE(exact.atSearchEdge);
    EXPECT_EQ(motrack::science::classifyMatch(exact), MatchQuality::Good);
    for (const auto& shift : {std::array{0.3, -0.2}, std::array{-1.6, 2.45}, std::array{3.25, 0.5}})
    {
        const auto frame = blobFrame(80, 60, 37.4 + shift[0], 28.7 + shift[1]);
        const auto result = motrack::science::findMatch(matcher.matchTemplate(), frame, rect(27, 19, 20, 20));
        ASSERT_EQ(result.status, MatchStatus::Matched);
        EXPECT_NEAR(result.x, bounds.x + shift[0], 0.2);
        EXPECT_NEAR(result.y, bounds.y + shift[1], 0.2);
        EXPECT_GT(result.peakHeight, motrack::science::TRACKER_DEFAULT_GOOD_MATCH);
        EXPECT_EQ(motrack::science::evolutionOrigin(result),
                  (PixelPoint{detail::javaRoundToInt(result.x), detail::javaRoundToInt(result.y)}));
    }
}

TEST(TemplateMatcher, AdjustsTheSearchAreaLikeTracker)
{
    const auto frame = blobFrame(40, 30, 20, 15);
    const auto matcher = keyMatcher(frame, circle(20, 15, 5));
    const auto& matchTemplate = matcher.matchTemplate();
    const int left = matchTemplate.image.width() / 2;
    const int top = matchTemplate.image.height() / 2;
    const int right = left + matchTemplate.image.width() % 2;
    // Moved (not clipped) inside the insets, then cut at the right.
    const auto moved = motrack::science::findMatch(matchTemplate, frame, rect(-10, -10, 15, 12));
    EXPECT_EQ(moved.searchArea, (PixelRect{left, top, 15, 12}));
    const auto cut = motrack::science::findMatch(matchTemplate, frame, rect(30, 5, 50, 5));
    EXPECT_EQ(cut.searchArea.width, 40 - cut.searchArea.x - right);
    // No room left: Tracker returns null with NaN width and height.
    const auto outside = motrack::science::findMatch(matchTemplate, frame, rect(100, 5, 10, 10));
    EXPECT_EQ(outside.status, MatchStatus::OutsideImage);
    EXPECT_TRUE(std::isnan(outside.peakHeight));
    EXPECT_TRUE(std::isnan(outside.peakWidth));
    EXPECT_EQ(motrack::science::classifyMatch(outside), MatchQuality::None);
    EXPECT_EQ(motrack::science::findMatch(matchTemplate, ArgbImage(4, 4), rect(0, 0, 4, 4)).status, MatchStatus::OutsideImage);
    EXPECT_EQ(motrack::science::findMatch(MatchTemplate{}, frame, rect(0, 0, 40, 30)).status, MatchStatus::OutsideImage);
}

TEST(TemplateMatcher, UniformSearchesScoreNaNAndCountAsPossible)
{
    const ArgbImage gray(30, 30, rgb(90, 90, 90));
    const auto matcher = keyMatcher(gray, circle(15, 15, 5));
    const auto result = motrack::science::findMatch(matcher.matchTemplate(), gray, rect(8, 8, 12, 12));
    ASSERT_EQ(result.status, MatchStatus::Matched);
    EXPECT_TRUE(std::isnan(result.peakHeight));
    // NaN compares false against both thresholds, so AutoTracker shows a possible match.
    EXPECT_EQ(motrack::science::classifyMatch(result), MatchQuality::Possible);
    // The first strict minimum in column order wins: the area's first position.
    EXPECT_EQ(result.pixel.x, 8 - matcher.matchTemplate().image.width() / 2 - matcher.matchTemplate().trimLeft);
}

TEST(TemplateMatcher, ClassifiesScoresWithAutoTrackerThresholds)
{
    MatchResult result;
    result.status = MatchStatus::Matched;
    result.peakHeight = 0.5;
    EXPECT_EQ(motrack::science::classifyMatch(result), MatchQuality::None);
    result.peakHeight = 1.0;
    EXPECT_EQ(motrack::science::classifyMatch(result), MatchQuality::Possible);
    result.peakHeight = 4.0;
    EXPECT_EQ(motrack::science::classifyMatch(result), MatchQuality::Good);
    EXPECT_EQ(motrack::science::classifyMatch(result, 5, 1), MatchQuality::Possible);
    result.peakHeight = INFINITE;
    EXPECT_EQ(motrack::science::classifyMatch(result, 10, 1), MatchQuality::Good);
    result.status = MatchStatus::Cancelled;
    EXPECT_EQ(motrack::science::classifyMatch(result), MatchQuality::None);
}

TEST(TemplateMatcher, ConvertsWizardPercentagesToAlphas)
{
    EXPECT_EQ(motrack::science::alphaFromPercent(motrack::science::TRACKER_DEFAULT_EVOLVE_PERCENT), 51);
    EXPECT_EQ(motrack::science::alphaFromPercent(motrack::science::TRACKER_DEFAULT_TETHER_PERCENT), 12);
    EXPECT_EQ(motrack::science::alphaFromPercent(60), 153);
    // (int) (2.55 * 100) truncates 254.99999999999997.
    EXPECT_EQ(motrack::science::alphaFromPercent(100), 254);
    EXPECT_EQ(motrack::science::alphaFromPercent(101), 255);
    EXPECT_EQ(motrack::science::alphaFromPercent(1'000'000'000), 255);
    EXPECT_EQ(motrack::science::alphaFromPercent(0), 0);
    EXPECT_EQ(motrack::science::alphaFromPercent(-7), 0);
}

TEST(TemplateMatcher, BoundaryFitsNeverExtrapolatePastTheNeighbors)
{
    // The target lies 4 px beyond a 3 x 3 search area: the best position is on the boundary.
    const auto key = blobFrame(80, 60, 40, 30);
    const auto matcher = keyMatcher(key, circle(40, 30, 7));
    for (const auto& shift :
         {std::array{4.4, 0.0}, std::array{-4.4, 0.0}, std::array{0.0, 4.4}, std::array{0.0, -4.4}, std::array{-1.6, 0.0}})
    {
        const auto frame = blobFrame(80, 60, 40 + shift[0], 30 + shift[1]);
        const auto result = motrack::science::findMatch(matcher.matchTemplate(), frame, rect(39, 29, 3, 3));
        ASSERT_EQ(result.status, MatchStatus::Matched);
        EXPECT_TRUE(result.atSearchEdge);
        EXPECT_LE(std::abs(result.x - result.pixel.x), 1.0);
        EXPECT_LE(std::abs(result.y - result.pixel.y), 1.0);
    }
    // Neighbors that leave the frame are not used. A template one pixel smaller than the frame has a
    // single search position (Tracker keeps a column and row for the right and bottom neighbors), and
    // its left and upper neighbors leave the frame, so neither axis is refined.
    const auto full = keyMatcher(key, MatchMask{.shape = MaskShape::Rectangle, .x = 0, .y = 0, .width = 80, .height = 60});
    EXPECT_EQ(full.matchTemplate().image.width(), 79);
    const auto whole = motrack::science::findMatch(full.matchTemplate(), blobFrame(80, 60, 40.3, 30.2), rect(0, 0, 80, 60));
    ASSERT_EQ(whole.status, MatchStatus::Matched);
    EXPECT_EQ(whole.searchArea.width, 1);
    EXPECT_EQ(whole.x, 0.0);
    EXPECT_EQ(whole.y, 0.0);
    EXPECT_FALSE(std::isnan(whole.peakHeight));
}

TEST(TemplateMatcher, SearchesCanBeCancelled)
{
    const auto frame = blobFrame(60, 40, 30, 20);
    const auto matcher = keyMatcher(frame, circle(30, 20, 5));
    int polls = 0;
    const MatchOptions options{.cancelled = [&polls]
                               {
                                   return ++polls > 3;
                               }};
    const auto result = motrack::science::findMatch(matcher.matchTemplate(), frame, rect(10, 10, 30, 20), options);
    EXPECT_EQ(result.status, MatchStatus::Cancelled);
    EXPECT_EQ(polls, 4);
    EXPECT_EQ(motrack::science::classifyMatch(result), MatchQuality::None);
    const MatchOptions stop{.cancelled = []
                            {
                                return true;
                            }};
    const SearchLine line{.x0 = 30, .y0 = 20, .angle = 0.0};
    EXPECT_EQ(motrack::science::findMatchAlongLine(matcher.matchTemplate(), frame, rect(10, 10, 30, 20), line, stop).status,
              MatchStatus::Cancelled);
    // Without a callback the search completes.
    EXPECT_EQ(motrack::science::findMatch(matcher.matchTemplate(), frame, rect(10, 10, 30, 20), MatchOptions{}).status,
              MatchStatus::Matched);
}

TEST(TemplateMatcher, ConcurrentSearchesOnOneTemplateAgree)
{
    const auto key = blobFrame(80, 60, 37.4, 28.7);
    const auto frame = blobFrame(80, 60, 38.9, 27.3);
    const auto matcher = keyMatcher(key, circle(37.4, 28.7, 7));
    const auto expected = motrack::science::findMatch(matcher.matchTemplate(), frame, rect(20, 12, 36, 32));
    std::atomic<int> agreeing = 0;
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int index = 0; index < 4; ++index)
    {
        threads.emplace_back(
            [&]
            {
                const auto result = motrack::science::findMatch(matcher.matchTemplate(), frame, rect(20, 12, 36, 32));
                if (result.x == expected.x && result.y == expected.y && result.peakHeight == expected.peakHeight)
                {
                    ++agreeing;
                }
            });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    EXPECT_EQ(agreeing.load(), 4);
}

// ---------------------------------------------------------------------------------------------
// One-dimensional searches.

TEST(TemplateMatcher, LineSearchPointsCoverThePixelsTheLineCrosses)
{
    const PixelRect area = rect(10, 20, 6, 4);
    const auto horizontal = motrack::science::lineSearchPoints(area, {.x0 = 0, .y0 = 21.5, .angle = 0.0});
    EXPECT_EQ(horizontal, (std::vector<PixelPoint>{{0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}}));
    const auto vertical = motrack::science::lineSearchPoints(area, {.x0 = 12.5, .y0 = 0, .angle = std::numbers::pi / 2});
    EXPECT_EQ(vertical, (std::vector<PixelPoint>{{2, 0}, {2, 1}, {2, 2}, {2, 3}}));
    // A tilted line steps through edge-adjacent pixels in order of increasing x.
    const auto maybeTilted = motrack::science::lineSearchPoints(area, {.x0 = 10, .y0 = 23.3, .angle = 0.5});
    if (!maybeTilted)
    {
        FAIL() << "the tilted line crosses the area";
    }
    const auto& tilted = *maybeTilted;
    ASSERT_GT(tilted.size(), 6U);
    for (std::size_t index = 1; index < tilted.size(); ++index)
    {
        const auto step = std::abs(tilted[index].x - tilted[index - 1].x) + std::abs(tilted[index].y - tilted[index - 1].y);
        EXPECT_LE(step, 1) << index;
        EXPECT_GE(tilted[index].x, tilted[index - 1].x);
    }
    EXPECT_FALSE(motrack::science::lineSearchPoints(area, {.x0 = 0, .y0 = 5, .angle = 0.0}));
    EXPECT_FALSE(motrack::science::lineSearchPoints(area, {.x0 = 0, .y0 = 0, .angle = NOT_A_NUMBER}));
    // Java fails (NegativeArraySizeException) on an empty rectangle; MoTrack returns no points.
    const auto empty = motrack::science::lineSearchPoints(rect(4, 4, 0, 6), {.x0 = 4, .y0 = 5.5, .angle = 0.4});
    EXPECT_TRUE(empty && empty->empty());
}

TEST(TemplateMatcher, LineSearchFindsTargetsOnTiltedAxes)
{
    const auto key = blobFrame(90, 70, 45.2, 35.6);
    const auto mask = circle(45.2, 35.6, 7);
    const auto matcher = keyMatcher(key, mask);
    const auto bounds = matcher.bounds();
    for (const double angle : {0.0, std::numbers::pi, 0.35, 1.2, 2.3, -std::numbers::pi / 4})
    {
        const SearchLine line{.x0 = 45.2, .y0 = 35.6, .angle = angle};
        const double distance = 2.7;
        const double cx = 45.2 + distance * std::cos(angle);
        const double cy = 35.6 - distance * std::sin(angle);
        const auto frame = blobFrame(90, 70, cx, cy);
        const auto result = motrack::science::findMatchAlongLine(matcher.matchTemplate(), frame, rect(33, 23, 25, 25), line);
        ASSERT_EQ(result.status, MatchStatus::Matched) << angle;
        // AutoTracker's match center, moved onto the axis.
        const auto center = motrack::science::projectOntoLine(
            line, result.x + (mask.x + mask.width / 2) - bounds.x, result.y + (mask.y + mask.height / 2) - bounds.y);
        EXPECT_NEAR(center[0], cx, 0.15) << angle;
        EXPECT_NEAR(center[1], cy, 0.15) << angle;
    }
}

TEST(TemplateMatcher, LineSearchReportsAreasWithoutLinePixels)
{
    const auto frame = blobFrame(60, 40, 30, 20);
    const auto matcher = keyMatcher(frame, circle(30, 20, 5));
    const auto& matchTemplate = matcher.matchTemplate();
    // Tracker searches the whole area in 2D when the line misses it.
    const auto missed = motrack::science::findMatchAlongLine(matchTemplate, frame, rect(20, 10, 20, 20), {.x0 = 0, .y0 = 2, .angle = 0.0});
    EXPECT_EQ(missed.status, MatchStatus::NoSearchPoints);
    EXPECT_TRUE(std::isnan(missed.peakHeight));
    // A line along the area's closed right edge yields pixels just outside the searched columns;
    // Tracker indexes past its arrays when no line pixel remains.
    const auto none = motrack::science::findMatchAlongLine(
        matchTemplate, frame, rect(20, 10, 10, 20), {.x0 = 30, .y0 = 0, .angle = std::numbers::pi / 2});
    EXPECT_EQ(none.searchArea, (PixelRect{20, 10, 10, 20}));
    EXPECT_EQ(none.status, MatchStatus::NoSearchPoints);
    const auto outside =
        motrack::science::findMatchAlongLine(matchTemplate, frame, rect(200, 10, 20, 20), {.x0 = 0, .y0 = 15, .angle = 0.0});
    EXPECT_EQ(outside.status, MatchStatus::OutsideImage);
    EXPECT_EQ(motrack::science::findMatchAlongLine(MatchTemplate{}, frame, rect(0, 0, 60, 40), {.x0 = 0, .y0 = 15, .angle = 0.0}).status,
              MatchStatus::OutsideImage);
    // A perfect match on the line scores +inf and is not refined.
    const auto exact = motrack::science::findMatchAlongLine(matchTemplate, frame, rect(20, 10, 20, 20), {.x0 = 0, .y0 = 20, .angle = 0.0});
    ASSERT_EQ(exact.status, MatchStatus::Matched);
    EXPECT_TRUE(std::isinf(exact.peakHeight));
    // A best position at the first line pixel uses the 2D fit and is flagged.
    const auto first = motrack::science::findMatchAlongLine(
        matchTemplate, blobFrame(60, 40, 18, 20), rect(25, 15, 10, 10), {.x0 = 0, .y0 = 20, .angle = 0.0});
    ASSERT_EQ(first.status, MatchStatus::Matched);
    EXPECT_TRUE(first.atSearchEdge);
    EXPECT_EQ(first.y, first.pixel.y);
}

TEST(TemplateMatcher, ProjectsMatchCentersOntoTheAxis)
{
    const SearchLine line{.x0 = 10, .y0 = 20, .angle = 0.4};
    const auto projected = motrack::science::projectOntoLine(line, 14.0, 13.0);
    // On the line: perpendicular offset zero.
    const double cross = (projected[0] - 10) * -std::sin(0.4) - (projected[1] - 20) * std::cos(0.4);
    EXPECT_NEAR(cross, 0.0, 1e-12);
    // The move is perpendicular to the axis.
    const double along = (projected[0] - 14.0) * std::cos(0.4) - (projected[1] - 13.0) * std::sin(0.4);
    EXPECT_NEAR(along, 0.0, 1e-12);
    const auto horizontal = motrack::science::projectOntoLine({.x0 = 0, .y0 = 5, .angle = 0.0}, 3.0, 9.0);
    EXPECT_DOUBLE_EQ(horizontal[0], 3.0);
    EXPECT_DOUBLE_EQ(horizontal[1], 5.0);
}

} // namespace
