#include "sampling_expectations.hpp"
#include "science/sampling/line_profile.hpp"
#include "science/sampling/rgb_region.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Compares the kernels with RGBStep/LineProfileStep results recorded from the pinned Tracker 6.3.5
// JAR by tools/reference/SamplingOracle.java (regenerate and diff with `tools/corpus.py
// sampling-oracle`). Region statistics and horizontal profiles must match bit for bit; tilted
// profiles within 1e-9 (different exact area algorithms and 1-ulp libm differences in atan2/cos/sin).
// Reference defects are registered below and checked against independent expectations instead.

namespace
{

using motrack::science::ArgbImage;
using motrack::science::ImagePoint;
using motrack::science::LINE_PROFILE_MAX_SPREAD;
using motrack::science::LineProfileData;
using motrack::science::LineProfileMode;
using motrack::science::lineProfileMode;
using motrack::science::LineProfileStatus;
using motrack::science::measureRgbRegion;
using motrack::science::PixelRect;
using motrack::science::RGB_DEFAULT_EDGE_LENGTH;
using motrack::science::RGB_DEFAULT_MAX_EDGE_LENGTH;
using motrack::science::RgbPolygon;
using motrack::science::RgbRegionOutline;
using motrack::science::RgbRegionShape;
using motrack::science::RgbRegionStatus;
using motrack::science::RgbShapeKind;
using motrack::science::sampleLineProfile;
using motrack::science::trackerLuma;
using motrack::science::expectations::exactMean;
using motrack::science::expectations::probeCorners;

constexpr double TILTED_TOLERANCE = 1e-9;
/// Tracker's incorrect tilted weights must be visibly wrong, not within rounding.
constexpr double TRACKER_WEIGHT_ERROR = 1e-3;

/// Tracker 6.3.5 defects that MoTrack deliberately does not reproduce (docs/image-sampling.md).
enum class Defect : std::uint8_t
{
    /// Java int sums of squares wrap above 2^31 - 1: wrong (usually NaN) deviations.
    SumOverflow,
    /// An all-black horizontal strip is treated as an unloaded image: no data.
    BlackStrip,
    /// Tilted probes get incorrect area weights: sin * cos < 0, downward vertical lines, and some
    /// first probes that start on a pixel corner.
    TiltedWeights,
    /// Tilted probes meeting grid vertices make getProfileData throw.
    TiltedException,
};

const std::map<std::string, Defect>& defects()
{
    static const std::map<std::string, Defect> registry = {
        {"overflow-white-182", Defect::SumOverflow},
        {"overflow-white-200", Defect::SumOverflow},
        {"overflow-white-ellipse-210", Defect::SumOverflow},
        {"overflow-bright-200", Defect::SumOverflow},
        {"overflow-sparse-1000", Defect::SumOverflow},
        {"horizontal-all-black", Defect::BlackStrip},
        {"tilted-q1-s1", Defect::TiltedWeights},
        {"tilted-q3-s1", Defect::TiltedWeights},
        {"tilted-q1-vedge", Defect::TiltedWeights},
        {"tilted-q1-gradient", Defect::TiltedWeights},
        {"tilted-vertical-down-s0", Defect::TiltedWeights},
        {"tilted-vertical-down-s1", Defect::TiltedWeights},
        {"tilted-integer-start-first-sample", Defect::TiltedWeights},
        {"tilted-vertical-integer", Defect::TiltedException},
        {"tilted-vertical-half", Defect::TiltedException},
        {"tilted-integer-start", Defect::TiltedException},
        {"tilted-horizontal-line", Defect::TiltedException},
    };
    return registry;
}

QJsonObject loadReference()
{
    QFile file(QStringLiteral(MOTRACK_SAMPLING_REFERENCE));
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

const QJsonObject& reference()
{
    static const QJsonObject document = loadReference();
    return document;
}

double number(const QJsonValue& value)
{
    if (value.isString())
    {
        EXPECT_EQ(value.toString(), QStringLiteral("NaN"));
        return std::numeric_limits<double>::quiet_NaN();
    }
    return value.toDouble();
}

ImagePoint point(const QJsonValue& value)
{
    const QJsonArray pair = value.toArray();
    return {.x = number(pair.at(0)), .y = number(pair.at(1))};
}

testing::AssertionResult same(double expected, double actual)
{
    if ((std::isnan(expected) && std::isnan(actual)) || expected == actual)
    {
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure() << "expected " << expected << ", got " << actual;
}

// ---------------------------------------------------------------------------------------------
// Frames: SamplingOracle.pixels repeated operation by operation.

std::uint64_t splitMix(std::uint64_t& state)
{
    std::uint64_t z = state += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

std::uint32_t pixelOf(const std::string& kind, int x, int y, std::uint64_t& state)
{
    int r = 0;
    int g = 0;
    int b = 0;
    if (kind == "noise")
    {
        const auto value = static_cast<std::uint32_t>(splitMix(state) >> 40U);
        r = static_cast<int>((value >> 16U) & 0xffU);
        g = static_cast<int>((value >> 8U) & 0xffU);
        b = static_cast<int>(value & 0xffU);
    }
    else if (kind == "bright")
    {
        const std::uint64_t value = splitMix(state);
        r = 224 + static_cast<int>((value >> 40U) & 31U);
        g = 224 + static_cast<int>((value >> 45U) & 31U);
        b = 224 + static_cast<int>((value >> 50U) & 31U);
    }
    else if (kind == "sparse")
    {
        r = g = b = splitMix(state) % 1000 < 92 ? 255 : 0;
    }
    else if (kind == "gradient")
    {
        r = x & 0xff;
        g = y & 0xff;
        b = (x + y) & 0xff;
    }
    else if (kind == "checker")
    {
        const bool on = ((x / 4 + y / 4) & 1) == 0;
        r = on ? 230 : 10;
        g = on ? 20 : 200;
        b = on ? 120 : 60;
    }
    else if (kind == "edge")
    {
        r = 255 * std::max(0, std::min(10, 203 - (10 * x))) / 10;
        g = 255 * std::max(0, std::min(10, 306 - (10 * y))) / 10;
        b = 90;
    }
    else if (kind == "vedge")
    {
        const bool left = x < 32;
        r = left ? 200 : 20;
        g = left ? 100 : 40;
        b = left ? 50 : 240;
    }
    else if (kind == "band")
    {
        const bool black = y < 24;
        r = black ? 0 : x & 0xff;
        g = black ? 0 : y & 0xff;
        b = black ? 0 : 50;
    }
    else if (kind == "white")
    {
        r = g = b = 255;
    }
    else
    {
        ADD_FAILURE() << "unknown frame kind " << kind;
    }
    // Opaque alpha: the kernels must ignore it, as Tracker's TYPE_INT_RGB raster has none.
    return 0xff000000U | motrack::science::expectations::rgb(r, g, b);
}

void hashInt(std::uint64_t& hash, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
    {
        hash ^= (value >> shift) & 0xffU;
        hash *= 0x100000001b3ULL;
    }
}

constexpr std::uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;

const std::map<std::string, ArgbImage>& frames()
{
    static const std::map<std::string, ArgbImage> images = []
    {
        std::map<std::string, ArgbImage> result;
        for (const auto& value : reference().value(QStringLiteral("frames")).toArray())
        {
            const QJsonObject spec = value.toObject();
            const std::string kind = spec.value(QStringLiteral("kind")).toString().toStdString();
            const int width = spec.value(QStringLiteral("width")).toInt();
            const int height = spec.value(QStringLiteral("height")).toInt();
            std::uint64_t state = spec.value(QStringLiteral("seed")).toString().toULongLong(nullptr, 16);
            auto created = ArgbImage::filled(width, height, 0);
            if (!created)
            {
                ADD_FAILURE() << "invalid frame size " << width << " x " << height;
                continue;
            }
            ArgbImage image = *std::move(created);
            std::uint64_t hash = FNV_OFFSET;
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const std::uint32_t pixel = pixelOf(kind, x, y, state);
                    image.setPixel(x, y, pixel);
                    hashInt(hash, pixel & 0xffffffU);
                }
            }
            EXPECT_EQ(hash, spec.value(QStringLiteral("fnv1a64")).toString().toULongLong(nullptr, 16))
                << "frame " << kind << " differs from the Java oracle";
            result.emplace(spec.value(QStringLiteral("id")).toString().toStdString(), std::move(image));
        }
        return result;
    }();
    return images;
}

const ArgbImage& frameFor(const QJsonObject& item)
{
    return frames().at(item.value(QStringLiteral("frame")).toString().toStdString());
}

std::string idOf(const QJsonObject& item)
{
    return item.value(QStringLiteral("id")).toString().toStdString();
}

TEST(SamplingReference, FixtureMatchesTheKernelConstants)
{
    ASSERT_FALSE(reference().isEmpty()) << MOTRACK_SAMPLING_REFERENCE;
    EXPECT_EQ(reference().value(QStringLiteral("reference_version")).toString(), QStringLiteral("6.3.5"));
    const QJsonObject tracker = reference().value(QStringLiteral("tracker")).toObject();
    EXPECT_EQ(tracker.value(QStringLiteral("rgb_default_edge_length")).toInt(), RGB_DEFAULT_EDGE_LENGTH);
    EXPECT_EQ(tracker.value(QStringLiteral("rgb_default_max_edge_length")).toInt(), RGB_DEFAULT_MAX_EDGE_LENGTH);
    EXPECT_EQ(tracker.value(QStringLiteral("line_profile_max_spread")).toInt(), LINE_PROFILE_MAX_SPREAD);
    const QJsonArray luma = tracker.value(QStringLiteral("luma")).toArray();
    EXPECT_EQ(number(luma.at(0)), trackerLuma(1, 0, 0));
    EXPECT_EQ(number(luma.at(1)), trackerLuma(0, 1, 0));
    EXPECT_EQ(number(luma.at(2)), trackerLuma(0, 0, 1));
    EXPECT_EQ(frames().size(), 10U);
    // Every registered defect names a recorded case.
    std::set<std::string> ids;
    for (const QString& key : {QStringLiteral("regions"), QStringLiteral("profiles")})
    {
        for (const auto& value : reference().value(key).toArray())
        {
            ids.insert(idOf(value.toObject()));
        }
    }
    for (const auto& [id, defect] : defects())
    {
        EXPECT_TRUE(ids.contains(id)) << id;
    }
}

// ---------------------------------------------------------------------------------------------
// RGB regions.

RgbRegionShape shapeOf(const QJsonObject& item)
{
    const QString kind = item.value(QStringLiteral("shape")).toString();
    const QJsonArray size = item.value(QStringLiteral("size")).toArray();
    RgbRegionShape shape;
    shape.kind = kind == QStringLiteral("ellipse")     ? RgbShapeKind::Ellipse
                 : kind == QStringLiteral("rectangle") ? RgbShapeKind::Rectangle
                                                       : RgbShapeKind::Polygon;
    shape.width = size.at(0).toInt();
    shape.height = size.at(1).toInt();
    if (shape.kind == RgbShapeKind::Polygon)
    {
        std::vector<ImagePoint> vertices;
        for (const auto& vertex : item.value(QStringLiteral("vertices")).toArray())
        {
            vertices.push_back(point(vertex));
        }
        shape.polygon = RgbPolygon(std::move(vertices));
    }
    return shape;
}

/// Tracker's arithmetic with Java int sums: what 6.3.5 shows when the sums overflow.
double trackerWrappedDeviation(std::int64_t sum, std::int64_t squareSum, std::int64_t count)
{
    const auto wrap = [](std::int64_t value)
    {
        return static_cast<double>(static_cast<std::int32_t>(static_cast<std::uint32_t>(value)));
    };
    const double mean = wrap(sum) / static_cast<double>(count);
    return count == 1 ? std::numeric_limits<double>::quiet_NaN()
                      : std::sqrt((wrap(squareSum) - (wrap(sum) * mean)) / static_cast<double>(count - 1));
}

/// The exact sample deviation from exact integer moments (independent of the kernel's formula).
double exactDeviation(std::int64_t sum, std::int64_t squareSum, std::int64_t count)
{
    const std::int64_t numerator = (count * squareSum) - (sum * sum);
    return std::sqrt(static_cast<double>(numerator) / (static_cast<double>(count) * static_cast<double>(count - 1)));
}

TEST(SamplingReference, RgbRegionsMatchTracker)
{
    const QJsonArray regions = reference().value(QStringLiteral("regions")).toArray();
    ASSERT_GE(regions.size(), 50);
    for (const auto& value : regions)
    {
        const QJsonObject item = value.toObject();
        const std::string id = idOf(item);
        SCOPED_TRACE(id);
        const RgbRegionShape shape = shapeOf(item);
        const ImagePoint position = point(item.value(QStringLiteral("position")));
        // Inclusion: Tracker's Shape.getBounds() and the exact set of included pixel centres.
        const RgbRegionOutline outline(shape, position);
        const auto bounds = outline.bounds();
        if (!bounds)
        {
            ADD_FAILURE() << "no pixel bounds";
            continue;
        }
        const PixelRect rect = *bounds;
        const QJsonArray expectedBounds = item.value(QStringLiteral("bounds")).toArray();
        EXPECT_EQ(
            rect,
            (PixelRect{
                expectedBounds.at(0).toInt(), expectedBounds.at(1).toInt(), expectedBounds.at(2).toInt(), expectedBounds.at(3).toInt()}));
        std::uint64_t mask = FNV_OFFSET;
        int included = 0;
        for (int y = rect.y; y < rect.y + rect.height; ++y)
        {
            for (int x = rect.x; x < rect.x + rect.width; ++x)
            {
                if (outline.includesPixel(x, y))
                {
                    ++included;
                    hashInt(mask, static_cast<std::uint32_t>(x));
                    hashInt(mask, static_cast<std::uint32_t>(y));
                }
            }
        }
        EXPECT_EQ(included, item.value(QStringLiteral("mask_pixels")).toInt());
        EXPECT_EQ(mask, item.value(QStringLiteral("mask_fnv1a64")).toString().toULongLong(nullptr, 16));

        const auto measured = measureRgbRegion(frameFor(item), shape, position);
        const QJsonValue result = item.value(QStringLiteral("result"));
        EXPECT_TRUE(item.value(QStringLiteral("tracker_error")).isNull());
        if (result.isNull())
        {
            EXPECT_EQ(measured.status, included == 0 ? RgbRegionStatus::NoPixels : RgbRegionStatus::OutsideImage);
            continue;
        }
        ASSERT_EQ(measured.status, RgbRegionStatus::Measured);
        const QJsonObject expected = result.toObject();
        const auto& s = measured.statistics;
        EXPECT_TRUE(same(number(expected.value(QStringLiteral("R"))), s.red));
        EXPECT_TRUE(same(number(expected.value(QStringLiteral("G"))), s.green));
        EXPECT_TRUE(same(number(expected.value(QStringLiteral("B"))), s.blue));
        EXPECT_TRUE(same(number(expected.value(QStringLiteral("luma"))), s.luma));
        EXPECT_TRUE(same(number(expected.value(QStringLiteral("pixels"))), static_cast<double>(s.pixels)));
        const std::array<double, 3> trackerDeviations = {number(expected.value(QStringLiteral("Rsd"))),
                                                         number(expected.value(QStringLiteral("Gsd"))),
                                                         number(expected.value(QStringLiteral("Bsd")))};
        const std::array<double, 3> deviations = {s.redDeviation, s.greenDeviation, s.blueDeviation};
        const bool overflow = defects().contains(id) && defects().at(id) == Defect::SumOverflow;
        EXPECT_EQ(measured.trackerSumOverflow, overflow);
        for (std::size_t c = 0; c < 3; ++c)
        {
            if (overflow)
            {
                // Tracker's value is exactly the int-wrapped arithmetic; MoTrack's is the exact deviation.
                EXPECT_TRUE(same(trackerDeviations[c], trackerWrappedDeviation(s.sums[c], s.squareSums[c], s.pixels))) << c;
                EXPECT_FALSE(same(trackerDeviations[c], deviations[c])) << c;
                EXPECT_NEAR(deviations[c], exactDeviation(s.sums[c], s.squareSums[c], s.pixels), 1e-12 * (1 + deviations[c])) << c;
            }
            else
            {
                EXPECT_TRUE(same(trackerDeviations[c], deviations[c])) << c;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Line profiles.

LineProfileData sampleCase(const QJsonObject& item)
{
    const QJsonArray ends = item.value(QStringLiteral("ends")).toArray();
    const LineProfileMode mode =
        lineProfileMode(item.value(QStringLiteral("always_horizontal")).toBool(), number(item.value(QStringLiteral("axis_angle"))));
    return sampleLineProfile(frameFor(item), point(ends.at(0)), point(ends.at(1)), item.value(QStringLiteral("spread")).toInt(), mode);
}

double angleOf(const QJsonObject& item)
{
    const QJsonArray ends = item.value(QStringLiteral("ends")).toArray();
    const ImagePoint end0 = point(ends.at(0));
    const ImagePoint end1 = point(ends.at(1));
    return std::atan2(end1.y - end0.y, end1.x - end0.x);
}

/// Every tilted sample equals the exact area-weighted mean of its probe; returns the samples' worst
/// difference from `tracker` columns (0 when there is no Tracker result).
double expectExactTilted(const QJsonObject& item, const LineProfileData& data)
{
    const double theta = angleOf(item);
    const int width = (2 * item.value(QStringLiteral("spread")).toInt()) + 1;
    const QJsonValue tracker = item.value(QStringLiteral("result"));
    double worst = 0.0;
    for (const auto& s : data.samples)
    {
        const auto expected = exactMean(frameFor(item), probeCorners(s.position, theta, width));
        EXPECT_NEAR(s.red, expected[0], TILTED_TOLERANCE) << s.index;
        EXPECT_NEAR(s.green, expected[1], TILTED_TOLERANCE) << s.index;
        EXPECT_NEAR(s.blue, expected[2], TILTED_TOLERANCE) << s.index;
        if (tracker.isObject())
        {
            const QJsonObject columns = tracker.toObject();
            const auto at = [&](const char* name)
            {
                return number(columns.value(QLatin1String(name)).toArray().at(s.index));
            };
            worst = std::max({worst, std::abs(at("R") - s.red), std::abs(at("G") - s.green), std::abs(at("B") - s.blue)});
        }
    }
    return worst;
}

TEST(SamplingReference, LineProfilesMatchTracker)
{
    const QJsonArray profiles = reference().value(QStringLiteral("profiles")).toArray();
    ASSERT_GE(profiles.size(), 40);
    for (const auto& value : profiles)
    {
        const QJsonObject item = value.toObject();
        const std::string id = idOf(item);
        SCOPED_TRACE(id);
        const LineProfileData data = sampleCase(item);
        const QJsonValue result = item.value(QStringLiteral("result"));
        const bool tilted = lineProfileMode(item.value(QStringLiteral("always_horizontal")).toBool(),
                                            number(item.value(QStringLiteral("axis_angle")))) == LineProfileMode::Tilted;
        const auto defect = defects().find(id);
        if (defect == defects().end())
        {
            EXPECT_TRUE(item.value(QStringLiteral("tracker_error")).isNull());
            EXPECT_FALSE(data.trackerOmitsBlackStrip);
            if (result.isNull())
            {
                EXPECT_NE(data.status, LineProfileStatus::Measured);
                continue;
            }
            ASSERT_EQ(data.status, LineProfileStatus::Measured);
            const QJsonObject columns = result.toObject();
            ASSERT_EQ(static_cast<qsizetype>(data.samples.size()), columns.value(QStringLiteral("n")).toArray().size());
            for (const auto& s : data.samples)
            {
                const auto at = [&](const char* name)
                {
                    return number(columns.value(QLatin1String(name)).toArray().at(s.index));
                };
                const std::array<std::pair<double, double>, 8> pairs = {std::pair{at("n"), static_cast<double>(s.index)},
                                                                        {at("x"), s.position.x},
                                                                        {at("y"), s.position.y},
                                                                        {at("R"), s.red},
                                                                        {at("G"), s.green},
                                                                        {at("B"), s.blue},
                                                                        {at("luma"), s.luma},
                                                                        {at("pixels"), s.pixels}};
                for (std::size_t c = 0; c < pairs.size(); ++c)
                {
                    if (tilted && c > 0 && c < 7)
                    {
                        EXPECT_NEAR(pairs[c].first, pairs[c].second, TILTED_TOLERANCE) << s.index << " column " << c;
                    }
                    else
                    {
                        EXPECT_TRUE(same(pairs[c].first, pairs[c].second)) << s.index << " column " << c;
                    }
                }
            }
            if (tilted)
            {
                expectExactTilted(item, data);
            }
            continue;
        }
        switch (defect->second)
        {
        case Defect::BlackStrip:
            EXPECT_TRUE(result.isNull());
            EXPECT_EQ(item.value(QStringLiteral("tracker_diagnostic")).toString(), QStringLiteral("image_failed"));
            ASSERT_EQ(data.status, LineProfileStatus::Measured);
            EXPECT_TRUE(data.trackerOmitsBlackStrip);
            EXPECT_FALSE(data.samples.empty());
            for (const auto& s : data.samples)
            {
                EXPECT_EQ(s.red + s.green + s.blue + s.luma, 0.0);
            }
            break;
        case Defect::TiltedWeights:
        {
            ASSERT_TRUE(tilted);
            ASSERT_TRUE(result.isObject());
            ASSERT_EQ(data.status, LineProfileStatus::Measured);
            const QJsonObject columns = result.toObject();
            ASSERT_EQ(static_cast<qsizetype>(data.samples.size()), columns.value(QStringLiteral("n")).toArray().size());
            for (const auto& s : data.samples)
            {
                // Positions do not depend on the weights and still agree.
                EXPECT_NEAR(number(columns.value(QStringLiteral("x")).toArray().at(s.index)), s.position.x, TILTED_TOLERANCE);
                EXPECT_NEAR(number(columns.value(QStringLiteral("y")).toArray().at(s.index)), s.position.y, TILTED_TOLERANCE);
            }
            EXPECT_GT(expectExactTilted(item, data), TRACKER_WEIGHT_ERROR);
            break;
        }
        case Defect::TiltedException:
            ASSERT_TRUE(tilted);
            EXPECT_TRUE(result.isNull());
            EXPECT_FALSE(item.value(QStringLiteral("tracker_error")).isNull());
            ASSERT_EQ(data.status, LineProfileStatus::Measured);
            EXPECT_FALSE(data.samples.empty());
            expectExactTilted(item, data);
            break;
        case Defect::SumOverflow:
            ADD_FAILURE() << "region defect registered for a profile";
            break;
        }
    }
}

} // namespace
