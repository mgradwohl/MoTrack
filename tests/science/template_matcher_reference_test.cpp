// Compares motrack_matching with the Java oracle fixture (tests/science/match-reference.json, produced
// by tools/reference/MatchOracle.java with the pinned TemplateMatcher) and with the scenes' independent
// true positions. The scene renderer repeats MatchOracle.render operation by operation; this target is
// compiled with -ffp-contract=off so that every frame hash matches the Java frame bit for bit.

#include "science/matching/matching_internal.hpp"
#include "science/matching/template_matcher.hpp"

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
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{

using motrack::science::ArgbImage;
using motrack::science::ArgbPixel;
using motrack::science::MaskShape;
using motrack::science::MatchMask;
using motrack::science::MatchResult;
using motrack::science::MatchStatus;
using motrack::science::PixelPoint;
using motrack::science::PixelRect;
using motrack::science::SearchLine;
using motrack::science::TemplateMatcher;

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();
constexpr int GOOD_MATCH = motrack::science::TRACKER_DEFAULT_GOOD_MATCH;

// ---------------------------------------------------------------------------------------------
// Fixture access.

const QJsonObject& reference()
{
    static const QJsonObject document = []
    {
        QFile file(QStringLiteral(MOTRACK_MATCH_REFERENCE));
        if (!file.open(QIODevice::ReadOnly))
        {
            return QJsonObject{};
        }
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return document;
}

QJsonArray section(const char* name)
{
    return reference()[QString::fromLatin1(name)].toArray();
}

double number(const QJsonValue& value)
{
    if (value.isString())
    {
        const auto text = value.toString();
        if (text == QStringLiteral("NaN"))
        {
            return NOT_A_NUMBER;
        }
        if (text == QStringLiteral("Infinity"))
        {
            return std::numeric_limits<double>::infinity();
        }
        if (text == QStringLiteral("-Infinity"))
        {
            return -std::numeric_limits<double>::infinity();
        }
        ADD_FAILURE() << "Unexpected numeric string " << text.toStdString();
    }
    return value.toDouble();
}

int integer(const QJsonValue& value)
{
    return value.toInt();
}

template <std::size_t Size> std::array<double, Size> numbers(const QJsonValue& value)
{
    std::array<double, Size> result{};
    const auto array = value.toArray();
    EXPECT_EQ(array.size(), static_cast<qsizetype>(Size));
    for (std::size_t index = 0; index < Size && std::cmp_less(index, array.size()); ++index)
    {
        result.at(index) = number(array[static_cast<qsizetype>(index)]);
    }
    return result;
}

template <std::size_t Size> std::array<int, Size> integers(const QJsonValue& value)
{
    std::array<int, Size> result{};
    const auto array = value.toArray();
    EXPECT_EQ(array.size(), static_cast<qsizetype>(Size));
    for (std::size_t index = 0; index < Size && std::cmp_less(index, array.size()); ++index)
    {
        result.at(index) = integer(array[static_cast<qsizetype>(index)]);
    }
    return result;
}

PixelRect rect(const QJsonValue& value)
{
    const auto r = integers<4>(value);
    return {.x = r[0], .y = r[1], .width = r[2], .height = r[3]};
}

std::string text(const QJsonValue& value)
{
    return value.toString().toStdString();
}

bool same(double first, double second)
{
    return (std::isnan(first) && std::isnan(second)) || first == second;
}

MatchMask mask(const QJsonValue& value)
{
    const auto object = value.toObject();
    const auto frame = numbers<4>(object[QStringLiteral("frame")]);
    const auto shape = text(object[QStringLiteral("shape")]) == "ellipse" ? MaskShape::Ellipse : MaskShape::Rectangle;
    return {.shape = shape, .x = frame[0], .y = frame[1], .width = frame[2], .height = frame[3]};
}

// ---------------------------------------------------------------------------------------------
// MatchOracle.Scene and MatchOracle.render.

constexpr int SUBSAMPLES = 4;
constexpr int CHANNELS = 3;

struct Scene
{
    int width = 0;
    int height = 0;
    std::array<int, CHANNELS> background{};
    int texture = 0;
    std::array<double, 2> center{};
    std::array<double, 2> velocity{};
    std::array<double, 2> acceleration{};
    double radius = 0.0;
    double growth = 0.0;
    std::array<double, CHANNELS> amplitude{};
    std::array<double, CHANNELS> drift{};
    double spin = 0.0;
    std::array<double, 4> pattern{};
    int noise = 0;
    std::uint64_t seed = 0;
    std::optional<std::array<int, 6>> occluder;
    std::array<int, CHANNELS> occluderColor{};

    [[nodiscard]] std::array<double, 2> centerAt(int k) const
    {
        const double t = k;
        return {center[0] + velocity[0] * t + 0.5 * acceleration[0] * t * t, center[1] + velocity[1] * t + 0.5 * acceleration[1] * t * t};
    }
};

Scene scene(const QJsonValue& value)
{
    const auto object = value.toObject();
    Scene result;
    result.width = integer(object[QStringLiteral("width")]);
    result.height = integer(object[QStringLiteral("height")]);
    result.background = integers<CHANNELS>(object[QStringLiteral("background")]);
    result.texture = integer(object[QStringLiteral("texture")]);
    result.center = numbers<2>(object[QStringLiteral("center")]);
    result.velocity = numbers<2>(object[QStringLiteral("velocity")]);
    result.acceleration = numbers<2>(object[QStringLiteral("acceleration")]);
    result.radius = number(object[QStringLiteral("radius")]);
    result.growth = number(object[QStringLiteral("growth")]);
    result.amplitude = numbers<CHANNELS>(object[QStringLiteral("amplitude")]);
    result.drift = numbers<CHANNELS>(object[QStringLiteral("drift")]);
    result.spin = number(object[QStringLiteral("spin")]);
    result.pattern = numbers<4>(object[QStringLiteral("pattern")]);
    result.noise = integer(object[QStringLiteral("noise")]);
    result.seed = static_cast<std::uint64_t>(object[QStringLiteral("seed")].toInteger());
    if (!object[QStringLiteral("occluder")].isNull())
    {
        result.occluder = integers<6>(object[QStringLiteral("occluder")]);
        result.occluderColor = integers<CHANNELS>(object[QStringLiteral("occluder_color")]);
    }
    return result;
}

double triangle(double s)
{
    return std::abs(2 * (s - std::floor(s + 0.5)));
}

std::uint64_t splitMix(std::uint64_t& state)
{
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

ArgbImage render(const Scene& s, int k)
{
    ArgbImage image(s.width, s.height);
    const double t = k;
    const auto c = s.centerAt(k);
    const double cx = c[0];
    const double cy = c[1];
    const double radius = s.radius + s.growth * t;
    const double limit = radius * radius;
    const double spin = s.spin * t;
    const double cosine = (1 - spin * spin) / (1 + spin * spin);
    const double sine = 2 * spin / (1 + spin * spin);
    std::array<double, CHANNELS> amplitude{};
    for (int i = 0; i < CHANNELS; ++i)
    {
        amplitude.at(i) = s.amplitude.at(i) + s.drift.at(i) * t;
    }
    std::uint64_t state = s.seed + static_cast<std::uint64_t>(k) * 0x632BE59BD9B4E019ULL;
    const std::array<int, 6>* occluder = s.occluder.has_value() ? &s.occluder.value() : nullptr;
    const bool occluded = occluder != nullptr && k >= (*occluder)[4] && k <= (*occluder)[5];
    for (int y = 0; y < s.height; ++y)
    {
        for (int x = 0; x < s.width; ++x)
        {
            const int tex = ((x * 7 + y * 13) % 11) - 5;
            std::array<int, CHANNELS> base{};
            for (int channel = 0; channel < CHANNELS; ++channel)
            {
                base.at(channel) = s.background.at(channel) + tex * s.texture;
            }
            if (!(x + 1 < cx - radius || x > cx + radius || y + 1 < cy - radius || y > cy + radius))
            {
                std::array<double, CHANNELS> sum{};
                for (int j = 0; j < SUBSAMPLES; ++j)
                {
                    for (int i = 0; i < SUBSAMPLES; ++i)
                    {
                        const double u = x + (i + 0.5) / SUBSAMPLES - cx;
                        const double v = y + (j + 0.5) / SUBSAMPLES - cy;
                        const double r2 = u * u + v * v;
                        if (r2 < limit)
                        {
                            const double q = 1 - r2 / limit;
                            const double ur = cosine * u + sine * v;
                            const double vr = cosine * v - sine * u;
                            const double pattern =
                                triangle(ur * s.pattern[0] + vr * s.pattern[1]) * triangle(ur * s.pattern[2] - vr * s.pattern[3]);
                            const double shade = (0.5 + 0.5 * q) * (0.35 + 0.65 * pattern);
                            for (int channel = 0; channel < CHANNELS; ++channel)
                            {
                                sum.at(channel) += s.background.at(channel) + amplitude.at(channel) * shade;
                            }
                        }
                        else
                        {
                            for (int channel = 0; channel < CHANNELS; ++channel)
                            {
                                sum.at(channel) += base.at(channel);
                            }
                        }
                    }
                }
                for (int channel = 0; channel < CHANNELS; ++channel)
                {
                    base.at(channel) = static_cast<int>(std::floor(sum.at(channel) / (SUBSAMPLES * SUBSAMPLES) + 0.5));
                }
            }
            const bool covered = occluded && x >= (*occluder)[0] && x < (*occluder)[0] + (*occluder)[2] && y >= (*occluder)[1] &&
                                 y < (*occluder)[1] + (*occluder)[3];
            ArgbPixel pixel = 0;
            for (int channel = 0; channel < CHANNELS; ++channel)
            {
                int value = covered ? s.occluderColor.at(channel) : base.at(channel);
                if (s.noise > 0)
                {
                    value += static_cast<int>(splitMix(state) % static_cast<std::uint64_t>(2 * s.noise + 1)) - s.noise;
                }
                value = std::clamp(value, 0, 255);
                pixel = (pixel << 8U) | static_cast<ArgbPixel>(value);
            }
            image.setPixel(x, y, pixel);
        }
    }
    return image;
}

// MatchOracle.hash: FNV-1a 64 over each pixel's four little-endian bytes.
std::string hash(std::span<const ArgbPixel> pixels)
{
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const ArgbPixel pixel : pixels)
    {
        for (unsigned byte = 0; byte < 4; ++byte)
        {
            h ^= (pixel >> (8U * byte)) & 0xffU;
            h *= 0x100000001b3ULL;
        }
    }
    std::ostringstream text;
    text << std::hex << std::setw(16) << std::setfill('0') << h;
    return text.str();
}

std::string hash(std::span<const int> values)
{
    std::vector<ArgbPixel> pixels;
    pixels.reserve(values.size());
    for (const int value : values)
    {
        pixels.push_back(static_cast<ArgbPixel>(value));
    }
    return hash(pixels);
}

/// The scene's true mask-bounds origin at frame k: the key-frame bounds moved with the blob center.
std::array<double, 2> truth(const Scene& s, const MatchMask& m, int k)
{
    const auto bounds = motrack::science::maskBounds(m).value_or(PixelRect{});
    const auto c = s.centerAt(k);
    const auto c0 = s.centerAt(0);
    return {bounds.x + (c[0] - c0[0]), bounds.y + (c[1] - c0[1])};
}

/// A fixed-point rendering for the test log.
std::string fixed(double value, int precision = 3)
{
    std::ostringstream text;
    text << std::fixed << std::setprecision(precision) << value;
    return text.str();
}

/// A left-aligned label padded to `width` characters for the test log.
std::string label(const std::string& value, std::size_t width)
{
    return value.size() >= width ? value : value + std::string(width - value.size(), ' ');
}

double distance(const MatchResult& result, const std::array<double, 2>& point)
{
    return std::hypot(result.x - point[0], result.y - point[1]);
}

/// The error of a 1D match as AutoTracker reports it: the match center (origin + mask center - bounds
/// origin) moved onto the line, compared with the scene's true center (which lies on the line).
double lineError(const std::array<double, 2>& origin, const MatchMask& m, const SearchLine& line, const std::array<double, 2>& center)
{
    const auto bounds = motrack::science::maskBounds(m).value_or(PixelRect{});
    const double x = origin[0] + (m.x + m.width / 2.0) - bounds.x;
    const double y = origin[1] + (m.y + m.height / 2.0) - bounds.y;
    const auto projected = motrack::science::projectOntoLine(line, x, y);
    return std::hypot(projected[0] - center[0], projected[1] - center[1]);
}

void expectTemplate(const TemplateMatcher& matcher, const QJsonObject& expected, const std::string& where)
{
    const auto& matchTemplate = matcher.matchTemplate();
    EXPECT_EQ((std::array{matchTemplate.image.width(), matchTemplate.image.height()}), integers<2>(expected[QStringLiteral("size")]))
        << where;
    EXPECT_EQ((std::array{matchTemplate.trimLeft, matchTemplate.trimTop}), integers<2>(expected[QStringLiteral("trim")])) << where;
    EXPECT_EQ(hash(matchTemplate.image.pixels()), text(expected[QStringLiteral("hash")])) << where;
    EXPECT_EQ(hash(matcher.working().pixels()), text(expected[QStringLiteral("working")])) << where;
    EXPECT_EQ(matcher.alphas(), integers<2>(expected[QStringLiteral("alphas")])) << where;
}

// Java's search result: {rect, after, point | null, width, height} or {rect, exception}.
struct JavaResult
{
    PixelRect after;
    std::optional<std::array<double, 2>> point;
    double width = NOT_A_NUMBER;
    double height = NOT_A_NUMBER;
    std::string exception;
    std::optional<std::vector<PixelPoint>> linePoints;
};

std::optional<std::vector<PixelPoint>> points(const QJsonValue& value)
{
    if (value.isNull())
    {
        return std::nullopt;
    }
    std::vector<PixelPoint> result;
    for (const auto& point : value.toArray())
    {
        const auto xy = integers<2>(point);
        result.push_back({.x = xy[0], .y = xy[1]});
    }
    return result;
}

JavaResult javaResult(const QJsonObject& object)
{
    JavaResult result;
    result.exception = text(object[QStringLiteral("exception")]);
    if (object.contains(QStringLiteral("line_points")))
    {
        result.linePoints = points(object[QStringLiteral("line_points")]);
    }
    if (!result.exception.empty())
    {
        return result;
    }
    result.after = rect(object[QStringLiteral("after")]);
    if (!object[QStringLiteral("point")].isNull())
    {
        result.point = numbers<2>(object[QStringLiteral("point")]);
    }
    result.width = number(object[QStringLiteral("width")]);
    result.height = number(object[QStringLiteral("height")]);
    return result;
}

void expectSameAsJava(const MatchResult& actual, const JavaResult& java, const std::string& where)
{
    ASSERT_TRUE(java.exception.empty()) << where;
    EXPECT_EQ(actual.searchArea, java.after) << where;
    ASSERT_EQ(actual.status == MatchStatus::Matched, java.point.has_value()) << where;
    if (const auto& point = java.point)
    {
        EXPECT_EQ(actual.x, (*point)[0]) << where;
        EXPECT_EQ(actual.y, (*point)[1]) << where;
    }
    EXPECT_TRUE(same(actual.peakWidth, java.width)) << where << ": width " << actual.peakWidth << " vs " << java.width;
    EXPECT_TRUE(same(actual.peakHeight, java.height)) << where << ": height " << actual.peakHeight << " vs " << java.height;
}

// ---------------------------------------------------------------------------------------------

TEST(TemplateMatcherReference, FixtureIsPresentAndPinned)
{
    ASSERT_FALSE(reference().isEmpty()) << "Missing " << MOTRACK_MATCH_REFERENCE;
    EXPECT_EQ(reference()[QStringLiteral("schema_version")].toInt(), 1);
    EXPECT_EQ(text(reference()[QStringLiteral("reference_version")]), "6.3.5");
}

TEST(TemplateMatcherReference, SrcOverCompositingMatchesJava2dForEveryAlpha)
{
    // TemplateMatcher.buildTemplate's AlphaComposite draws, exhaustively over 8-bit channel pairs.
    const auto composite = section("composite");
    ASSERT_EQ(composite.size(), 255);
    for (const auto& entry : composite)
    {
        const auto object = entry.toObject();
        const int alpha = integer(object[QStringLiteral("alpha")]);
        const int extra = motrack::science::matching_detail::compositeExtraAlpha(alpha);
        EXPECT_EQ(extra, alpha);
        for (const bool opaque : {true, false})
        {
            std::vector<ArgbPixel> result(std::size_t{256} * 256);
            for (unsigned y = 0; y < 256; ++y)
            {
                for (unsigned x = 0; x < 256; ++x)
                {
                    const unsigned sourceAlpha = opaque ? 255U : (x * 11U + y * 3U) & 0xffU;
                    const unsigned targetAlpha = opaque ? 255U : (x * 7U + y * 13U) & 0xffU;
                    const ArgbPixel source = (sourceAlpha << 24U) | (x << 16U) | (y << 8U) | ((x ^ y) & 0xffU);
                    const ArgbPixel target = (targetAlpha << 24U) | (y << 16U) | (x << 8U) | ((x + y) & 0xffU);
                    result[y * 256 + x] = motrack::science::matching_detail::srcOver(source, target, extra);
                }
            }
            EXPECT_EQ(hash(result), text(object[opaque ? QStringLiteral("opaque") : QStringLiteral("translucent")]))
                << "alpha " << alpha << (opaque ? " opaque" : " translucent");
        }
    }
}

TEST(TemplateMatcherReference, MaskPixelsAndTemplatesMatchJava)
{
    for (const auto& entry : section("masks"))
    {
        const auto object = entry.toObject();
        const auto id = text(object[QStringLiteral("id")]);
        const auto m = mask(object[QStringLiteral("mask")]);
        const auto maybeBounds = motrack::science::maskBounds(m);
        if (!maybeBounds)
        {
            FAIL() << id;
        }
        const auto bounds = *maybeBounds;
        EXPECT_EQ(bounds, rect(object[QStringLiteral("bounds")])) << id;
        // Path2D containment of the translated mask on the oracle's quarter-pixel grid.
        std::vector<int> grid;
        for (int j = -4; j <= 4 * bounds.height + 4; ++j)
        {
            for (int i = -4; i <= 4 * bounds.width + 4; ++i)
            {
                grid.push_back(motrack::science::matching_detail::maskContains(m, {.x = bounds.x, .y = bounds.y}, i / 4.0, j / 4.0) ? 1
                                                                                                                                    : 0);
            }
        }
        EXPECT_EQ(hash(grid), text(object[QStringLiteral("contains_grid")])) << id;
        const auto frame = render(scene(object[QStringLiteral("scene")]), 0);
        const auto created = TemplateMatcher::create(frame, m);
        if (id == "ellipse-empty")
        {
            // Deliberate difference: Java keeps a 1x1 fully transparent template (trimLeft = width), on
            // which every position scores NaN. MoTrack refuses a mask that includes no pixel.
            const auto javaTemplate = object[QStringLiteral("template")].toObject();
            EXPECT_EQ(integers<2>(javaTemplate[QStringLiteral("size")]), (std::array{1, 1}));
            EXPECT_EQ(integers<2>(javaTemplate[QStringLiteral("trim")]), (std::array{2, 0}));
            EXPECT_FALSE(created) << id;
            EXPECT_TRUE(std::ranges::none_of(motrack::science::maskPixels(m), [](std::uint8_t pixel) { return pixel != 0; }));
            continue;
        }
        if (!created)
        {
            FAIL() << id;
        }
        const auto& matcher = *created;
        expectTemplate(matcher, object[QStringLiteral("template")].toObject(), id);
        // Reference defect (display only, not ported): TemplateMatcher cuts the match preview at
        // (match + 1, match + 1) instead of the trim offset, so masks trimmed by other than one pixel,
        // such as whole-pixel rectangles, preview the wrong pixels.
        if (const auto preview = object[QStringLiteral("preview_mismatches")]; !preview.isNull())
        {
            const bool trimmedByOne = matcher.matchTemplate().trimLeft == 1 && matcher.matchTemplate().trimTop == 1;
            EXPECT_EQ(integer(preview) == 0, trimmedByOne) << id;
        }
        // The included-pixel rows of the trimmed template.
        const auto& image = matcher.matchTemplate().image;
        const auto rows = object[QStringLiteral("rows")].toArray();
        ASSERT_EQ(rows.size(), image.height()) << id;
        for (int y = 0; y < image.height(); ++y)
        {
            std::string row;
            for (int x = 0; x < image.width(); ++x)
            {
                row.push_back((image.pixel(x, y) >> 24U) == 0 ? '0' : '1');
            }
            EXPECT_EQ(row, text(rows[y])) << id << " row " << y;
        }
    }
}

TEST(TemplateMatcherReference, EvolutionAndTetheringMatchBuildTemplate)
{
    const auto evolution = reference()[QStringLiteral("evolution")].toObject();
    const auto s = scene(evolution[QStringLiteral("scene")]);
    const auto m = mask(evolution[QStringLiteral("mask")]);
    auto created = TemplateMatcher::create(render(s, 0), m);
    if (!created)
    {
        FAIL() << "no template";
    }
    auto& matcher = *created;
    expectTemplate(matcher, evolution[QStringLiteral("created")].toObject(), "created");
    int index = 0;
    for (const auto& entry : evolution[QStringLiteral("steps")].toArray())
    {
        const auto step = entry.toObject();
        const auto where = "evolution step " + std::to_string(index++);
        const auto origin = integers<2>(step[QStringLiteral("origin")]);
        const auto alphas = integers<2>(step[QStringLiteral("alphas")]);
        EXPECT_TRUE(step[QStringLiteral("built")].toBool()) << where;
        const bool changed =
            matcher.evolve(render(s, integer(step[QStringLiteral("frame")])), {.x = origin[0], .y = origin[1]}, alphas[0], alphas[1]);
        EXPECT_EQ(changed, alphas[0] != 0 || alphas[1] != 0) << where;
        expectTemplate(matcher, step[QStringLiteral("template")].toObject(), where);
    }
}

TEST(TemplateMatcherReference, LineSearchPointsMatchGetSearchPoints)
{
    for (const auto& entry : section("search_points"))
    {
        const auto object = entry.toObject();
        const auto area = rect(object[QStringLiteral("rect")]);
        const auto line = numbers<3>(object[QStringLiteral("line")]);
        const auto where = "rect " + std::to_string(area.x) + "," + std::to_string(area.y) + " angle " + std::to_string(line[2]);
        const auto actual = motrack::science::lineSearchPoints(area, {.x0 = line[0], .y0 = line[1], .angle = line[2]});
        if (!text(object[QStringLiteral("exception")]).empty())
        {
            // Deliberate difference: Java allocates a negative-size array for an empty rectangle.
            EXPECT_EQ(text(object[QStringLiteral("exception")]), "NegativeArraySizeException") << where;
            EXPECT_TRUE(actual && actual->empty()) << where;
            continue;
        }
        EXPECT_EQ(actual, points(object[QStringLiteral("points")])) << where;
    }
}

// Cases whose MoTrack result deliberately differs from the reference (docs/template-matching.md).
// Each maps to the independent expectation the test checks instead.
enum class Difference : std::uint8_t
{
    None,
    BoundaryFit,    ///< Best position on the search-area boundary: true neighbors, no extrapolation.
    LineDirection,  ///< 1D offset converted along the line pixels instead of (cos, sin).
    LineTrimmed,    ///< 1D points computed for the adjusted search area.
    LineMissesArea, ///< Java falls back to a 2D search.
    LineNoPoints,   ///< Java throws ArrayIndexOutOfBoundsException.
};

Difference difference(const std::string& id)
{
    if (id.starts_with("beyond("))
    {
        return Difference::BoundaryFit;
    }
    if (id == "line(3.141592653589793)" || id == "line(0.3)" || id == "line(2.0)" || id == "line(-0.7853981633974483)")
    {
        return Difference::LineDirection;
    }
    if (id == "line-trimmed(0.2)")
    {
        return Difference::LineTrimmed;
    }
    if (id == "line-misses-area")
    {
        return Difference::LineMissesArea;
    }
    if (id == "line-outside-trimmed-area")
    {
        return Difference::LineNoPoints;
    }
    return Difference::None;
}

TEST(TemplateMatcherReference, SearchesMatchGetMatchLocation)
{
    std::set<std::string> seen;
    double javaWildest = 0; // the reference's largest boundary move away from its whole-pixel match
    for (const auto& entry : section("matches"))
    {
        const auto object = entry.toObject();
        const auto id = text(object[QStringLiteral("id")]);
        seen.insert(id);
        const auto s = scene(object[QStringLiteral("scene")]);
        const auto m = mask(object[QStringLiteral("mask")]);
        const int target = integer(object[QStringLiteral("target")]);
        auto created = TemplateMatcher::create(render(s, 0), m);
        if (!created)
        {
            FAIL() << id;
        }
        auto& matcher = *created;
        if (!object[QStringLiteral("evolve")].isNull())
        {
            // Evolve at the true positions of the frames before the target, as the oracle does.
            const auto evolve = integers<2>(object[QStringLiteral("evolve")]);
            const auto bounds = motrack::science::maskBounds(m).value_or(PixelRect{});
            const auto c0 = s.centerAt(0);
            for (int k = 1; k < target; ++k)
            {
                const auto c = s.centerAt(k);
                MatchResult at;
                at.x = bounds.x + (c[0] - c0[0]);
                at.y = bounds.y + (c[1] - c0[1]);
                matcher.evolve(render(s, k), motrack::science::evolutionOrigin(at), evolve[0], evolve[1]);
            }
        }
        expectTemplate(matcher, object[QStringLiteral("template")].toObject(), id);
        const auto frame = render(s, target);
        const auto java = javaResult(object[QStringLiteral("result")].toObject());
        const auto area = rect(object[QStringLiteral("result")].toObject()[QStringLiteral("rect")]);
        std::optional<SearchLine> line;
        if (!object[QStringLiteral("line")].isNull())
        {
            const auto values = numbers<3>(object[QStringLiteral("line")]);
            line = SearchLine{.x0 = values[0], .y0 = values[1], .angle = values[2]};
            // AutoTracker computes the points for the unadjusted rectangle first.
            EXPECT_EQ(motrack::science::lineSearchPoints(area, line.value()), java.linePoints) << id;
        }
        const auto actual = line ? motrack::science::findMatchAlongLine(matcher.matchTemplate(), frame, area, line.value())
                                 : motrack::science::findMatch(matcher.matchTemplate(), frame, area);
        const auto expected = truth(s, m, target);
        const auto center = s.centerAt(target);
        const auto axis = line.value_or(SearchLine{});
        const auto javaPoint = java.point.value_or(std::array{NOT_A_NUMBER, NOT_A_NUMBER});
        switch (difference(id))
        {
        case Difference::None:
            expectSameAsJava(actual, java, id);
            if (line)
            {
                std::cout << "[ 1D match ] " << label(id, 28) << " error on the line "
                          << fixed(lineError({actual.x, actual.y}, m, axis, center)) << " px\n";
            }
            else if (id.starts_with("shift("))
            {
                // Independent accuracy of the subpixel fit on noise-free shifts with a fixed template.
                EXPECT_LT(distance(actual, expected), 0.15) << id;
            }
            break;
        case Difference::BoundaryFit:
        {
            // Same search, scores and whole-pixel choice; only the fit on the area's boundary differs.
            ASSERT_TRUE(java.point) << id;
            EXPECT_EQ(actual.searchArea, java.after) << id;
            ASSERT_EQ(actual.status, MatchStatus::Matched) << id;
            EXPECT_TRUE(actual.atSearchEdge) << id;
            // MoTrack never moves the match beyond the neighbors of the best searched position.
            EXPECT_LE(std::abs(actual.x - actual.pixel.x), 1.0) << id;
            EXPECT_LE(std::abs(actual.y - actual.pixel.y), 1.0) << id;
            javaWildest = std::max({javaWildest, std::abs(javaPoint[0] - actual.pixel.x), std::abs(javaPoint[1] - actual.pixel.y)});
            std::cout << "[ boundary ] " << label(id, 28) << " MoTrack (" << fixed(actual.x) << ", " << fixed(actual.y) << "), reference ("
                      << fixed(javaPoint[0]) << ", " << fixed(javaPoint[1]) << "), truth (" << fixed(expected[0]) << ", "
                      << fixed(expected[1]) << ")\n";
            break;
        }
        case Difference::LineDirection:
        {
            // Identical search and score; the reference moves the subpixel offset along (cos, sin),
            // MoTrack toward the neighboring line pixel, which lands closer to the truth on the line.
            ASSERT_TRUE(java.point) << id;
            EXPECT_EQ(actual.searchArea, java.after) << id;
            EXPECT_TRUE(same(actual.peakHeight, java.height)) << id;
            EXPECT_TRUE(same(actual.peakWidth, java.width)) << id;
            const double javaError = lineError(javaPoint, m, axis, center);
            const double error = lineError({actual.x, actual.y}, m, axis, center);
            std::cout << "[ 1D match ] " << label(id, 28) << " error on the line " << fixed(error) << " px (reference " << fixed(javaError)
                      << " px)\n";
            EXPECT_LT(error, javaError) << id;
            EXPECT_LT(error, 0.15) << id;
            break;
        }
        case Difference::LineTrimmed:
        {
            // The reference searches a parallel line shifted by the area adjustment.
            ASSERT_TRUE(java.point) << id;
            EXPECT_EQ(actual.searchArea, java.after) << id;
            EXPECT_GT(actual.peakHeight, java.height) << id;
            const double javaError = lineError(javaPoint, m, axis, center);
            const double error = lineError({actual.x, actual.y}, m, axis, center);
            std::cout << "[ 1D match ] " << label(id, 28) << " error on the line " << fixed(error) << " px (reference " << fixed(javaError)
                      << " px)\n";
            EXPECT_LT(error, javaError) << id;
            EXPECT_LT(error, 0.15) << id;
            break;
        }
        case Difference::LineMissesArea:
            // The reference silently searched the whole area in two dimensions.
            ASSERT_TRUE(java.point) << id;
            EXPECT_EQ(actual.status, MatchStatus::NoSearchPoints) << id;
            EXPECT_TRUE(std::isnan(actual.peakHeight)) << id;
            break;
        case Difference::LineNoPoints:
            EXPECT_EQ(java.exception, "ArrayIndexOutOfBoundsException") << id;
            EXPECT_EQ(actual.status, MatchStatus::NoSearchPoints) << id;
            break;
        }
    }
    EXPECT_TRUE(seen.contains("exact") && seen.contains("uniform") && seen.contains("line(0.0)")) << "fixture lacks core cases";
    // Evidence for the boundary difference: the reference extrapolates a concave fit by many pixels.
    EXPECT_GT(javaWildest, 5.0);
}

// The oracle's sequence driver: search around the velocity-predicted center of the latest good matches.
struct Driver
{
    const Scene& scene;
    MatchMask mask;
    double halfSearch;
    std::array<int, 2> evolve;
    std::optional<double> angle;
};

struct DriverStep
{
    PixelRect searched;
    MatchResult result;
    bool good = false;
    std::string frameHash;
    std::string templateHash;
};

std::vector<DriverStep> drive(const Driver& driver, int frames)
{
    std::vector<DriverStep> steps;
    const auto bounds = motrack::science::maskBounds(driver.mask).value_or(PixelRect{});
    auto created = TemplateMatcher::create(render(driver.scene, 0), driver.mask);
    if (!created)
    {
        ADD_FAILURE() << "no template";
        return steps;
    }
    auto& matcher = *created;
    const double maskX = driver.mask.x + driver.mask.width / 2.0;
    const double maskY = driver.mask.y + driver.mask.height / 2.0;
    int lastFrame = -1;
    int previousFrame = -1;
    double lastX = maskX;
    double lastY = maskY;
    double previousX = 0;
    double previousY = 0;
    const auto origin = driver.scene.centerAt(0);
    for (int k = 0; k < frames; ++k)
    {
        double px = lastX;
        double py = lastY;
        if (previousFrame >= 0)
        {
            const double vx = (lastX - previousX) / (lastFrame - previousFrame);
            const double vy = (lastY - previousY) / (lastFrame - previousFrame);
            px = lastX + vx * (k - lastFrame);
            py = lastY + vy * (k - lastFrame);
        }
        DriverStep step;
        step.searched =
            motrack::science::frameBounds(px - driver.halfSearch, py - driver.halfSearch, 2 * driver.halfSearch, 2 * driver.halfSearch)
                .value_or(PixelRect{});
        const auto image = render(driver.scene, k);
        step.frameHash = hash(image.pixels());
        step.result =
            driver.angle
                ? motrack::science::findMatchAlongLine(
                      matcher.matchTemplate(), image, step.searched, {.x0 = origin[0], .y0 = origin[1], .angle = driver.angle.value()})
                : motrack::science::findMatch(matcher.matchTemplate(), image, step.searched);
        step.good = motrack::science::classifyMatch(step.result) == motrack::science::MatchQuality::Good;
        if (step.good)
        {
            previousFrame = lastFrame;
            previousX = lastX;
            previousY = lastY;
            lastFrame = k;
            lastX = step.result.x + maskX - bounds.x;
            lastY = step.result.y + maskY - bounds.y;
            if (driver.evolve[0] != 0 || driver.evolve[1] != 0)
            {
                matcher.evolve(image, motrack::science::evolutionOrigin(step.result), driver.evolve[0], driver.evolve[1]);
                step.templateHash = hash(matcher.matchTemplate().image.pixels());
            }
        }
        steps.push_back(step);
    }
    return steps;
}

TEST(TemplateMatcherReference, TrackingSequencesMatchJavaAndTheTruePath)
{
    for (const auto& entry : section("sequences"))
    {
        const auto object = entry.toObject();
        const auto id = text(object[QStringLiteral("id")]);
        const auto s = scene(object[QStringLiteral("scene")]);
        const auto m = mask(object[QStringLiteral("mask")]);
        std::optional<double> angle;
        if (!object[QStringLiteral("angle")].isNull())
        {
            angle = number(object[QStringLiteral("angle")]);
        }
        const Driver driver{.scene = s,
                            .mask = m,
                            .halfSearch = number(object[QStringLiteral("half_search")]),
                            .evolve = integers<2>(object[QStringLiteral("evolve")]),
                            .angle = angle};
        const auto javaSteps = object[QStringLiteral("steps")].toArray();
        const auto steps = drive(driver, integer(object[QStringLiteral("frames")]));
        ASSERT_EQ(static_cast<qsizetype>(steps.size()), javaSteps.size()) << id;
        // A tilted 1D search differs in every subpixel position (Difference::LineDirection), so its
        // search areas drift apart from the reference's; it is checked against the true path only.
        const bool exact = !angle || *angle == 0.0;
        const auto origin = s.centerAt(0);
        const std::optional<SearchLine> line =
            angle ? std::optional(SearchLine{.x0 = origin[0], .y0 = origin[1], .angle = *angle}) : std::nullopt;
        const auto error = [&](const std::array<double, 2>& point, int k)
        {
            if (line)
            {
                return lineError(point, m, line.value(), s.centerAt(k));
            }
            const auto expected = truth(s, m, k);
            return std::hypot(point[0] - expected[0], point[1] - expected[1]);
        };
        const double tolerance = number(object[QStringLiteral("tolerance")]);
        double javaWorst = 0;
        double worst = 0;
        int good = 0;
        int edges = 0;
        for (std::size_t k = 0; k < steps.size(); ++k)
        {
            const auto javaStep = javaSteps[static_cast<qsizetype>(k)].toObject();
            const auto where = id + " frame " + std::to_string(k);
            const auto& step = steps[k];
            const int frame = static_cast<int>(k);
            EXPECT_EQ(step.frameHash, text(javaStep[QStringLiteral("frame_hash")])) << where;
            if (javaStep[QStringLiteral("good")].toBool())
            {
                javaWorst = std::max(javaWorst, error(numbers<2>(javaStep[QStringLiteral("point")]), frame));
            }
            if (exact)
            {
                EXPECT_EQ(step.searched, rect(javaStep[QStringLiteral("rect")])) << where;
                EXPECT_EQ(step.good, javaStep[QStringLiteral("good")].toBool()) << where;
                EXPECT_EQ(step.templateHash, text(javaStep[QStringLiteral("template")])) << where;
                if (step.result.atSearchEdge && !step.good)
                {
                    // Difference::BoundaryFit: same search, possibly a different fit. Rejected matches do
                    // not steer the driver; accepted ones are compared exactly below.
                    ++edges;
                    EXPECT_EQ(step.result.searchArea, rect(javaStep[QStringLiteral("after")])) << where;
                    continue;
                }
                const JavaResult java{.after = rect(javaStep[QStringLiteral("after")]),
                                      .point = javaStep[QStringLiteral("point")].isNull()
                                                   ? std::nullopt
                                                   : std::optional(numbers<2>(javaStep[QStringLiteral("point")])),
                                      .width = number(javaStep[QStringLiteral("width")]),
                                      .height = number(javaStep[QStringLiteral("height")]),
                                      .exception = {},
                                      .linePoints = std::nullopt};
                expectSameAsJava(step.result, java, where);
            }
            if (step.good)
            {
                ++good;
                worst = std::max(worst, error({step.result.x, step.result.y}, frame));
            }
        }
        // Independent accuracy: every accepted match lies within the sequence's declared tolerance.
        EXPECT_LE(worst, tolerance) << id;
        EXPECT_GT(good, 0) << id;
        if (!exact)
        {
            EXPECT_LT(worst, javaWorst) << id << ": the reference's tilted 1D positions should be worse";
        }
        std::cout << "[ sequence ] " << label(id, 24) << " good " << good << "/" << steps.size() << "  edge " << edges << "  worst error "
                  << fixed(worst) << " px (reference " << fixed(javaWorst) << " px, tolerance " << fixed(tolerance, 2) << ")\n";
    }
}

} // namespace
