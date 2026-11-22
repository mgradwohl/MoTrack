#include "science/filters/video_filter.hpp"

#include "science/filters/filter_image.hpp"
#include "science/filters/java_arithmetic.hpp"
#include "science/filters/video_filters.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using motrack::science::BaselineFilter;
using motrack::science::BrightnessFilter;
using motrack::science::DarkGhostFilter;
using motrack::science::DeinterlaceFilter;
using motrack::science::FilterArithmetic;
using motrack::science::FilterImage;
using motrack::science::FilterPoint;
using motrack::science::FilterRotation;
using motrack::science::GhostFilter;
using motrack::science::GrayScaleFilter;
using motrack::science::LogFilter;
using motrack::science::NegativeFilter;
using motrack::science::PerspectiveFilter;
using motrack::science::PerspectivePlane;
using motrack::science::PerspectiveQuad;
using motrack::science::RadialDistortionFilter;
using motrack::science::RadialProjection;
using motrack::science::ResizeFilter;
using motrack::science::RotateFilter;
using motrack::science::StrobeFilter;
using motrack::science::SumFilter;
using motrack::science::VideoFilter;
using motrack::science::VideoFilterKind;
using motrack::science::VideoFilterStack;

// ---------------------------------------------------------------------------------------------
// The synthetic frames of tools/reference/FilterOracle.java, generated with the same integer steps.

struct FrameSpec
{
    std::string pattern;
    int width = 0;
    int height = 0;
    int seed = 0;
    int t = 0;
};

std::uint32_t xorshift(std::uint32_t state)
{
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

std::uint32_t seedState(int seed, int t)
{
    const std::uint32_t state = (static_cast<std::uint32_t>(seed) * 0x9E3779B9U) ^ (static_cast<std::uint32_t>(t + 1) * 0x85EBCA6BU);
    return state == 0 ? 1U : state;
}

std::uint32_t rgb(std::uint32_t r, std::uint32_t g, std::uint32_t b)
{
    return (r << 16U) | (g << 8U) | b;
}

/// Pixels exactly as the Java generator's ints (0x00RRGGBB, or 0xAARRGGBB for "argb").
FilterImage generate(const FrameSpec& frame)
{
    const int w = frame.width;
    const int h = frame.height;
    const int t = frame.t;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    std::uint32_t state = seedState(frame.seed, frame.pattern == "blob" || frame.pattern == "darkblob" ? 0 : t);
    const int cx = 4 + (3 * t);
    const int cy = 3 + (2 * t);
    constexpr std::array<std::uint32_t, 8> PALETTE{0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0x00ffff, 0xff00ff, 0xffffff, 0x000000};
    std::size_t i = 0;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x, ++i)
        {
            state = xorshift(state);
            const std::uint32_t noise = state;
            const bool disk = ((x - cx) * (x - cx)) + ((y - cy) * (y - cy)) <= 16;
            std::uint32_t pixel = 0;
            if (frame.pattern == "gradient")
            {
                pixel = rgb(w > 1 ? static_cast<std::uint32_t>(x * 255 / (w - 1)) : 128U,
                            h > 1 ? static_cast<std::uint32_t>(y * 255 / (h - 1)) : 128U,
                            static_cast<std::uint32_t>((x * 7) + (y * 13) + (t * 29)) & 0xffU);
            }
            else if (frame.pattern == "noise")
            {
                pixel = noise & 0xffffffU;
            }
            else if (frame.pattern == "checker")
            {
                pixel = PALETTE[static_cast<std::size_t>((x / 3) + ((y / 3) * 3) + t) & 7U];
            }
            else if (frame.pattern == "blob")
            {
                pixel = disk ? rgb(235, 215, 130) : rgb(30 + (noise & 15U), 35 + ((noise >> 4U) & 15U), 40 + ((noise >> 8U) & 15U));
            }
            else if (frame.pattern == "darkblob")
            {
                pixel = disk ? rgb(25, 45, 70) : rgb(215 + (noise & 15U), 220 + ((noise >> 4U) & 15U), 225 + ((noise >> 8U) & 15U));
            }
            else if (frame.pattern == "argb")
            {
                const int phase = (x + y) % 7;
                const std::uint32_t alpha = phase == 0   ? 0U
                                            : phase == 1 ? 255U
                                                         : static_cast<std::uint32_t>((x * 37) + (y * 61) + frame.seed) & 0xffU;
                pixel = (alpha << 24U) | (noise & 0xffffffU);
            }
            else
            {
                throw std::invalid_argument("unknown pattern " + frame.pattern);
            }
            pixels[i] = pixel;
        }
    }
    return {w, h, std::move(pixels)};
}

/// SHA-256 of R, G, B bytes per pixel, row by row (FilterOracle.sha256).
std::string sha256(const FilterImage& image)
{
    QByteArray bytes;
    bytes.reserve(static_cast<qsizetype>(image.pixels().size() * 3));
    for (const std::uint32_t pixel : image.pixels())
    {
        bytes.append(static_cast<char>((pixel >> 16U) & 0xffU));
        bytes.append(static_cast<char>((pixel >> 8U) & 0xffU));
        bytes.append(static_cast<char>(pixel & 0xffU));
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
}

std::string rgbHex(const FilterImage& image)
{
    std::string text;
    constexpr std::string_view DIGITS = "0123456789abcdef";
    for (const std::uint32_t pixel : image.pixels())
    {
        for (int shift = 20; shift >= 0; shift -= 4)
        {
            text.push_back(DIGITS[(pixel >> static_cast<unsigned>(shift)) & 0xfU]);
        }
    }
    return text;
}

// ---------------------------------------------------------------------------------------------
// Fixture access

const QJsonObject& reference()
{
    static const QJsonObject document = []
    {
        QFile file(QStringLiteral(MOTRACK_FILTER_REFERENCE));
        if (!file.open(QIODevice::ReadOnly))
        {
            throw std::runtime_error("cannot open " MOTRACK_FILTER_REFERENCE);
        }
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return document;
}

FrameSpec frameSpec(const QJsonObject& json)
{
    return {.pattern = json["pattern"].toString().toStdString(),
            .width = json["width"].toInt(),
            .height = json["height"].toInt(),
            .seed = json["seed"].toInt(),
            .t = json["t"].toInt()};
}

std::string text(const QJsonValue& value)
{
    return value.toString().toStdString();
}

PerspectiveQuad quad(const QJsonValue& value)
{
    const QJsonArray corners = value.toArray();
    PerspectiveQuad result{};
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        const QJsonArray xy = corners[static_cast<qsizetype>(i)].toArray();
        result[i] = {xy[0].toDouble(), xy[1].toDouble()};
    }
    return result;
}

RadialProjection projection(const QJsonValue& value)
{
    const auto parsed = motrack::science::radialProjectionFromName(text(value));
    if (!parsed)
    {
        throw std::invalid_argument("unknown projection " + text(value));
    }
    return *parsed;
}

/// The UI setter FilterOracle.set calls for a changed field.
void setParameter(VideoFilter& filter, const std::string& name, const QJsonValue& value)
{
    switch (filter.kind())
    {
    case VideoFilterKind::Brightness:
        if (name == "brightness")
        {
            dynamic_cast<BrightnessFilter&>(filter).setBrightness(value.toInt());
            return;
        }
        if (name == "contrast")
        {
            dynamic_cast<BrightnessFilter&>(filter).setContrast(value.toDouble());
            return;
        }
        break;
    case VideoFilterKind::GrayScale:
        if (name == "weights")
        {
            const QJsonArray weights = value.toArray();
            dynamic_cast<GrayScaleFilter&>(filter).setWeights(weights[0].toDouble(), weights[1].toDouble(), weights[2].toDouble());
            return;
        }
        break;
    case VideoFilterKind::Log:
        if (name == "level")
        {
            dynamic_cast<LogFilter&>(filter).setLevel(value.toInt());
            return;
        }
        if (name == "grayscale")
        {
            dynamic_cast<LogFilter&>(filter).setGrayscale(value.toBool());
            return;
        }
        break;
    case VideoFilterKind::Deinterlace:
        if (name == "field")
        {
            dynamic_cast<DeinterlaceFilter&>(filter).setOddField(text(value) == "odd");
            return;
        }
        break;
    case VideoFilterKind::Ghost:
    case VideoFilterKind::DarkGhost:
        if (name == "fade")
        {
            dynamic_cast<GhostFilter&>(filter).setFade(value.toDouble());
            return;
        }
        break;
    case VideoFilterKind::Strobe:
        if (name == "fade")
        {
            dynamic_cast<StrobeFilter&>(filter).setFade(value.toDouble());
            return;
        }
        if (name == "bright_trails")
        {
            dynamic_cast<StrobeFilter&>(filter).setBrightTrails(value.toBool());
            return;
        }
        break;
    case VideoFilterKind::Sum:
        if (name == "brightness")
        {
            dynamic_cast<SumFilter&>(filter).setBrightness(value.toDouble());
            return;
        }
        if (name == "mean")
        {
            dynamic_cast<SumFilter&>(filter).setMean(value.toBool());
            return;
        }
        break;
    case VideoFilterKind::Resize:
        if (name == "width_factor")
        {
            dynamic_cast<ResizeFilter&>(filter).setWidthFactor(value.toDouble());
            return;
        }
        if (name == "height_factor")
        {
            dynamic_cast<ResizeFilter&>(filter).setHeightFactor(value.toDouble());
            return;
        }
        break;
    case VideoFilterKind::Rotate:
        if (name == "rotation")
        {
            const auto rotation = motrack::science::filterRotationFromName(text(value));
            if (!rotation)
            {
                throw std::invalid_argument("unknown rotation " + text(value));
            }
            dynamic_cast<RotateFilter&>(filter).setRotation(*rotation);
            return;
        }
        if (name == "reverse")
        {
            dynamic_cast<RotateFilter&>(filter).setReversed(value.toBool());
            return;
        }
        break;
    case VideoFilterKind::RadialDistortion:
    {
        auto& radial = dynamic_cast<RadialDistortionFilter&>(filter);
        if (name == "fixed_radius")
        {
            radial.setFixedRadius(value.toDouble());
            return;
        }
        if (name == "input_fov")
        {
            radial.setSourceFov(value.toDouble());
            return;
        }
        if (name == "input_type")
        {
            radial.setSourceProjection(projection(value));
            return;
        }
        if (name == "output_type")
        {
            radial.setOutputProjection(projection(value));
            return;
        }
        break;
    }
    default:
        break;
    }
    throw std::invalid_argument("no setter for " + name);
}

/// Each OSP Loader.loadObject applied to a new filter, as FilterOracle.create does.
std::unique_ptr<VideoFilter> loadFilter(const QJsonObject& spec)
{
    const auto kind = motrack::science::videoFilterKindFromClassName(text(spec["class"]));
    if (!kind)
    {
        throw std::invalid_argument("unknown filter class " + text(spec["class"]));
    }
    auto filter = motrack::science::makeVideoFilter(*kind);
    const QJsonObject params = spec["params"].toObject();
    if (params.isEmpty())
    {
        return filter;
    }
    switch (*kind)
    {
    case VideoFilterKind::Log:
        if (params.contains("level"))
        {
            setParameter(*filter, "level", params["level"]);
        }
        setParameter(*filter, "grayscale", params["grayscale"]); // getBoolean: missing is false
        return filter;
    case VideoFilterKind::Strobe:
        if (params.contains("fade"))
        {
            setParameter(*filter, "fade", params["fade"]);
        }
        setParameter(*filter, "bright_trails", params["bright_trails"]);
        return filter;
    case VideoFilterKind::Rotate:
        if (params.contains("rotation"))
        {
            setParameter(*filter, "rotation", params["rotation"]);
        }
        setParameter(*filter, "reverse", params["reverse"]);
        return filter;
    case VideoFilterKind::Perspective:
    {
        auto& perspective = dynamic_cast<PerspectiveFilter&>(*filter);
        if (params.contains("fixed_out"))
        {
            perspective.setFixed(PerspectivePlane::Input, params["fixed_in"].toBool(), 0);
            perspective.setFixed(PerspectivePlane::Output, params["fixed_out"].toBool(), 0);
        }
        for (const auto& [key, plane] :
             {std::pair{"in_corners", PerspectivePlane::Input}, std::pair{"out_corners", PerspectivePlane::Output}})
        {
            const QJsonObject keys = params[key].toObject();
            for (auto it = keys.begin(); it != keys.end(); ++it)
            {
                perspective.loadKeyFrame(plane, it.key().toInt(), quad(it.value()));
            }
        }
        return filter;
    }
    default:
        for (auto it = params.begin(); it != params.end(); ++it)
        {
            setParameter(*filter, it.key().toStdString(), it.value());
        }
        return filter;
    }
}

/// Bit-exact Tracker 6.3.5 arithmetic for the filters whose defects MoTrack corrects by default.
void useTrackerArithmetic(VideoFilter& filter)
{
    if (auto* brightness = dynamic_cast<BrightnessFilter*>(&filter))
    {
        brightness->setArithmetic(FilterArithmetic::Tracker635);
    }
    if (auto* perspective = dynamic_cast<PerspectiveFilter*>(&filter))
    {
        perspective->setArithmetic(FilterArithmetic::Tracker635);
    }
}

QJsonValue probe(const VideoFilter& filter, const std::string& name)
{
    if (const auto* resize = dynamic_cast<const ResizeFilter*>(&filter))
    {
        return name == "width_factor" ? resize->widthFactor() : resize->heightFactor();
    }
    const auto& radial = dynamic_cast<const RadialDistortionFilter&>(filter);
    if (name == "fixed_radius")
    {
        return radial.fixedRadius();
    }
    if (name == "output_fov")
    {
        return radial.outputFov();
    }
    return radial.lowerRadiusLimit();
}

struct CaseRun
{
    std::vector<std::string> failures;
    int evaluations = 0;
};

CaseRun runCase(const QJsonObject& json, FilterArithmetic arithmetic)
{
    CaseRun run;
    const std::string id = text(json["id"]);
    VideoFilterStack stack;
    for (const QJsonValue spec : json["filters"].toArray())
    {
        auto& filter = stack.add(loadFilter(spec.toObject()));
        if (arithmetic == FilterArithmetic::Tracker635)
        {
            useTrackerArithmetic(filter);
        }
    }
    std::vector<FilterImage> frames;
    for (const QJsonValue frame : json["frames"].toArray())
    {
        frames.push_back(generate(frameSpec(frame.toObject())));
    }
    const QJsonArray results = json["results"].toArray();
    for (const QJsonValue value : json["ops"].toArray())
    {
        const QJsonObject op = value.toObject();
        const std::string kind = text(op["op"]);
        if (kind == "apply")
        {
            const FilterImage& output = stack.apply(frames.at(static_cast<std::size_t>(op["frame"].toInt())), op["video_frame"].toInt(0));
            const QJsonObject expected = results[run.evaluations].toObject();
            const std::string where = id + " evaluation " + std::to_string(run.evaluations);
            if (output.width() != expected["width"].toInt() || output.height() != expected["height"].toInt())
            {
                run.failures.push_back(where + ": size " + std::to_string(output.width()) + "x" + std::to_string(output.height()));
            }
            else if (sha256(output) != text(expected["sha256"]))
            {
                run.failures.push_back(
                    where + ": pixels differ" +
                    (expected.contains("rgb") ? " (expected " + text(expected["rgb"]) + ", got " + rgbHex(output) + ")" : ""));
            }
            ++run.evaluations;
        }
        else if (kind == "step")
        {
            stack.stepChanged();
        }
        else if (kind == "set")
        {
            setParameter(stack.at(static_cast<std::size_t>(op["filter"].toInt())), text(op["name"]), op["value"]);
        }
        else if (kind == "enable")
        {
            stack.at(static_cast<std::size_t>(op["filter"].toInt())).setEnabled(op["value"].toBool());
        }
        else if (kind == "clear")
        {
            stack.at(static_cast<std::size_t>(op["filter"].toInt())).clear();
        }
        else if (kind == "baseline")
        {
            auto& baseline = dynamic_cast<BaselineFilter&>(stack.at(static_cast<std::size_t>(op["filter"].toInt())));
            FilterImage image = frames.at(static_cast<std::size_t>(op["frame"].toInt()));
            if (!baseline.setBaselineImage(std::move(image)))
            {
                run.failures.push_back(id + ": baseline rejected");
            }
        }
        else if (kind == "replace_source")
        {
            stack.replaceSource();
        }
        else
        {
            throw std::invalid_argument("unknown op " + kind);
        }
    }
    for (const QJsonValue value : json["probes"].toArray())
    {
        const QJsonObject p = value.toObject();
        const QJsonValue actual = probe(stack.at(static_cast<std::size_t>(p["filter"].toInt())), text(p["name"]));
        // The output field of view goes through the platform's atan/asin/tan, which may differ from Java's by
        // an ulp; the pixel hashes stay exact (fixture samples keep >= 5.8e-6 px from every rounding edge).
        const double tolerance =
            text(p["name"]) == "output_fov" ? 4 * std::numeric_limits<double>::epsilon() * std::abs(p["value"].toDouble()) : 0.0;
        if (std::abs(actual.toDouble() - p["value"].toDouble()) > tolerance)
        {
            run.failures.push_back(id + ": probe " + text(p["name"]) + " = " + std::to_string(actual.toDouble()));
        }
    }
    return run;
}

FilterImage solid(int width, int height, std::uint32_t pixel)
{
    return {width, height, pixel};
}

FilterImage pattern(const std::string& name, int width, int height, int seed = 1, int t = 0)
{
    return generate({.pattern = name, .width = width, .height = height, .seed = seed, .t = t});
}

const FilterImage& applyOnce(VideoFilter& filter, const FilterImage& image)
{
    return filter.apply(image, 1);
}

// ---------------------------------------------------------------------------------------------
// Java oracle parity

TEST(VideoFilterReference, FixtureCoversEveryRegisteredFilter)
{
    const QJsonObject& document = reference();
    ASSERT_EQ(document["schema_version"].toInt(), 1);
    EXPECT_EQ(text(document["reference_version"]), "6.3.5");
    const QJsonArray registry = document["registry"].toArray();
    const auto kinds = motrack::science::trackerVideoFilterKinds();
    ASSERT_EQ(static_cast<std::size_t>(registry.size()), kinds.size());
    for (std::size_t i = 0; i < kinds.size(); ++i)
    {
        EXPECT_EQ(text(registry[static_cast<qsizetype>(i)]), motrack::science::videoFilterClassName(kinds[i]));
    }
}

TEST(VideoFilterReference, GeneratedFramesMatchTheJavaGenerator)
{
    for (const QJsonValue value : reference()["frames"].toArray())
    {
        const QJsonObject frame = value.toObject();
        EXPECT_EQ(sha256(generate(frameSpec(frame))), text(frame["sha256"])) << text(frame["pattern"]) << " " << frame["width"].toInt();
    }
}

TEST(VideoFilterReference, EveryCaseMatchesTrackerPixelForPixel)
{
    int evaluations = 0;
    for (const QJsonValue value : reference()["cases"].toArray())
    {
        const CaseRun run = runCase(value.toObject(), FilterArithmetic::Tracker635);
        evaluations += run.evaluations;
        for (const std::string& failure : run.failures)
        {
            ADD_FAILURE() << failure;
        }
    }
    EXPECT_GT(evaluations, 300);
}

TEST(VideoFilterReference, CorrectedArithmeticChangesOnlyBrightnessAndPerspectiveCases)
{
    // The corrections are deliberate differences; every other filter is identical in both modes.
    std::vector<std::string> changed;
    for (const QJsonValue value : reference()["cases"].toArray())
    {
        const QJsonObject json = value.toObject();
        if (!runCase(json, FilterArithmetic::Corrected).failures.empty())
        {
            changed.push_back(text(json["id"]));
        }
    }
    for (const std::string& id : changed)
    {
        EXPECT_TRUE(id.starts_with("brightness-") || id.starts_with("perspective-") || id == "stack-brightness-ghost" ||
                    id == "stack-geometry")
            << id;
    }
    EXPECT_NE(std::ranges::find(changed, "brightness-128-50.0"), changed.end());
    EXPECT_EQ(std::ranges::find(changed, "brightness-0-50.0"), changed.end());
    EXPECT_NE(std::ranges::find(changed, "perspective-trapezoid"), changed.end());
}

TEST(VideoFilterReference, JavaExceptionsBecomeDefinedResults)
{
    std::vector<std::string> ids;
    for (const QJsonValue value : reference()["failures"].toArray())
    {
        ids.push_back(text(value["id"]));
    }
    ASSERT_EQ(ids, (std::vector<std::string>{"resize-zero-width", "perspective-no-key-0"}));
    const QJsonArray failures = reference()["failures"].toArray();
    EXPECT_EQ(text(failures[0]["exception"]), "IllegalArgumentException");
    EXPECT_EQ(text(failures[1]["exception"]), "NullPointerException");
    // MoTrack leaves the image unchanged instead of throwing...
    ResizeFilter resize;
    resize.setWidthFactor(0.01);
    const FilterImage frame = pattern("noise", 50, 40, 20);
    EXPECT_EQ(&applyOnce(resize, frame), &frame);
    // ...and uses the first key frame before it.
    PerspectiveFilter perspective;
    perspective.setFixed(PerspectivePlane::Output, false, 0);
    const PerspectiveQuad in{FilterPoint(8, 4), FilterPoint(30, 6), FilterPoint(34, 20), FilterPoint(2, 18)};
    const PerspectiveQuad out{FilterPoint(3, 2), FilterPoint(33, 2), FilterPoint(33, 20), FilterPoint(3, 20)};
    perspective.loadKeyFrame(PerspectivePlane::Input, 5, in);
    perspective.loadKeyFrame(PerspectivePlane::Output, 5, out);
    const FilterImage noise = pattern("noise", 37, 23, 14);
    const FilterImage& before = perspective.apply(noise, 1, 0);
    const FilterImage first = before;
    EXPECT_TRUE(perspective.apply(noise, 1, 5).sameColors(first));
}

TEST(VideoFilterReference, CompositingMatchesJava2D)
{
    for (const QJsonValue value : reference()["composites"].toArray())
    {
        const QJsonObject json = value.toObject();
        const FilterImage source = generate(frameSpec(json["source"].toObject()));
        FilterImage destination = json["background"].isObject() ? generate(frameSpec(json["background"].toObject()))
                                                                : FilterImage(source.width(), source.height());
        motrack::science::compositeSourceOver(source, destination);
        if (json.contains("filter"))
        {
            NegativeFilter negative;
            destination = applyOnce(negative, destination);
        }
        EXPECT_EQ(sha256(destination), text(json["sha256"]));
    }
}

TEST(VideoFilterReference, TranscendentalsAgreeWithJavaToTheLastBit)
{
    // Brightness truncation makes a one-ulp difference visible, so its sin/cos must equal Java's Math.
    for (const QJsonValue value : reference()["math"]["brightness_trig"].toArray())
    {
        const QJsonObject entry = value.toObject();
        const auto [sin, cos] = motrack::science::java::sinCos(std::numbers::pi * entry["contrast"].toDouble() / 200);
        EXPECT_EQ(std::bit_cast<std::uint64_t>(sin), std::stoull(text(entry["sin"]), nullptr, 16)) << entry["contrast"].toDouble();
        EXPECT_EQ(std::bit_cast<std::uint64_t>(cos), std::stoull(text(entry["cos"]), nullptr, 16)) << entry["contrast"].toDouble();
    }
    const QJsonObject lookups = reference()["math"]["log_lookup_sha256_prefix"].toObject();
    ASSERT_GE(lookups.size(), 201);
    for (auto it = lookups.begin(); it != lookups.end(); ++it)
    {
        LogFilter log;
        log.setLevel(it.key().toInt());
        QByteArray bytes;
        for (const std::int32_t entry : log.lookup())
        {
            const auto bits = static_cast<std::uint32_t>(entry);
            for (int shift = 24; shift >= 0; shift -= 8)
            {
                bytes.append(static_cast<char>((bits >> static_cast<unsigned>(shift)) & 0xffU));
            }
        }
        EXPECT_EQ(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(16).toStdString(), text(it.value()))
            << "level " << it.key().toStdString();
    }
}

TEST(VideoFilterReference, CorrectlyRoundedSinCosStaysWithinAnUlpOfTheLibrary)
{
    const auto ulps = [](double a, double b)
    {
        return std::abs(std::bit_cast<std::int64_t>(a) - std::bit_cast<std::int64_t>(b));
    };
    for (int step = -1620; step <= 1620; ++step)
    {
        const double theta = step * 0.01234;
        const auto [sin, cos] = motrack::science::java::sinCos(theta);
        EXPECT_LE(ulps(sin, std::sin(theta)), 1) << theta;
        EXPECT_LE(ulps(cos, std::cos(theta)), 1) << theta;
    }
    EXPECT_EQ(motrack::science::java::sinCos(0.0), (std::pair{0.0, 1.0}));
    // cos(fl(pi / 2)) is the remainder of pi / 2, which the three-part reduction keeps exactly.
    EXPECT_EQ(motrack::science::java::sinCos(std::numbers::pi / 2).second, 6.123233995736766e-17);
    EXPECT_TRUE(std::isnan(motrack::science::java::sinCos(std::numeric_limits<double>::infinity()).first));
    EXPECT_EQ(motrack::science::java::sinCos(1e7).first, std::sin(1e7));
}

TEST(JavaArithmetic, NarrowingRoundingAndPackingFollowTheJavaLanguage)
{
    namespace java = motrack::science::java;
    constexpr double NAN_VALUE = std::numeric_limits<double>::quiet_NaN();
    constexpr auto INT_MAX_VALUE = std::numeric_limits<std::int32_t>::max();
    constexpr auto INT_MIN_VALUE = std::numeric_limits<std::int32_t>::min();
    EXPECT_EQ(java::toInt(NAN_VALUE), 0);
    EXPECT_EQ(java::toInt(3.99), 3);
    EXPECT_EQ(java::toInt(-3.99), -3);
    EXPECT_EQ(java::toInt(1e10), INT_MAX_VALUE);
    EXPECT_EQ(java::toInt(-1e10), INT_MIN_VALUE);
    EXPECT_EQ(java::toInt(2.75F), 2);
    EXPECT_EQ(java::toLong(NAN_VALUE), 0);
    EXPECT_EQ(java::toLong(1e19), std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(java::toLong(-1e19), std::numeric_limits<std::int64_t>::min());
    // Math.round: ties toward positive infinity, without the floor(x + 0.5) error at 0.49999999999999994.
    EXPECT_EQ(java::round(2.5), 3);
    EXPECT_EQ(java::round(-2.5), -2);
    EXPECT_EQ(java::round(-0.5), 0);
    EXPECT_EQ(java::round(0.49999999999999994), 0);
    EXPECT_EQ(java::round(NAN_VALUE), 0);
    EXPECT_EQ(java::round(1e300), std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(java::lowBits(std::numeric_limits<std::int64_t>::max()), -1);
    EXPECT_EQ(java::lowBits(0x100000005LL), 5);
    EXPECT_TRUE(std::isnan(java::min(NAN_VALUE, 1.0)));
    EXPECT_TRUE(std::isnan(java::min(1.0, NAN_VALUE)));
    EXPECT_TRUE(std::isnan(java::max(NAN_VALUE, 1.0)));
    EXPECT_EQ(java::min(1.0, 2.0), 1.0);
    EXPECT_EQ(java::max(1.0, 2.0), 2.0);
    // (v << 16) | (v << 8) | v on a negative int sets every high bit.
    EXPECT_EQ(java::packGray(-3) & motrack::science::RGB_MASK, 0xFFFFFDU);
    EXPECT_EQ(java::packGray(300) & motrack::science::RGB_MASK, 0x2D2D2CU);
    EXPECT_EQ(java::addWrapping(INT_MAX_VALUE, 1), INT_MIN_VALUE);
    EXPECT_EQ(java::multiplyWrapping(65536, 65536), 0);
}

// ---------------------------------------------------------------------------------------------
// Images and compositing

TEST(FilterImage, ValidatesSizesAndAccess)
{
    EXPECT_THROW(FilterImage(-1, 2), std::invalid_argument);
    EXPECT_THROW(FilterImage(std::numeric_limits<int>::max(), 2), std::invalid_argument);
    EXPECT_THROW(FilterImage(2, 2, std::vector<std::uint32_t>(3)), std::invalid_argument);
    const FilterImage empty(0, 7);
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.width(), 0);
    EXPECT_EQ(empty.height(), 0);
    EXPECT_TRUE(FilterImage(5, 0, std::vector<std::uint32_t>{}).empty());
    const FilterImage image(2, 1, std::vector<std::uint32_t>{0x00123456, 0xFF654321});
    EXPECT_EQ(image.at(1, 0), 0xFF654321U);
    EXPECT_THROW((void)image.at(2, 0), std::out_of_range);
    EXPECT_THROW((void)image.at(0, -1), std::out_of_range);
    EXPECT_TRUE(image.sameColors(FilterImage(2, 1, std::vector<std::uint32_t>{0xFF123456, 0x00654321})));
    EXPECT_FALSE(image.sameColors(FilterImage(2, 1, std::vector<std::uint32_t>{0xFF123457, 0x00654321})));
    EXPECT_FALSE(image.sameColors(FilterImage(1, 2, std::vector<std::uint32_t>{0x00123456, 0xFF654321})));
    EXPECT_EQ(motrack::science::alphaOf(0x80000000U), 0x80);
}

TEST(FilterImage, CompositingRoundsLikeAnEightBitMultiply)
{
    // Over black, each channel is MUL8(a, c), which equals round(a c / 255) for every 8-bit pair.
    for (int alpha = 0; alpha < 256; ++alpha)
    {
        std::vector<std::uint32_t> row;
        row.reserve(256);
        for (std::uint32_t c = 0; c < 256; ++c)
        {
            row.push_back((static_cast<std::uint32_t>(alpha) << 24U) | (c << 16U) | (c << 8U) | c);
        }
        FilterImage destination(256, 1);
        motrack::science::compositeSourceOver(FilterImage(256, 1, std::move(row)), destination);
        for (int c = 0; c < 256; ++c)
        {
            const auto expected = static_cast<int>(std::lround(alpha * c / 255.0));
            ASSERT_EQ(motrack::science::redOf(destination.at(c, 0)), expected) << alpha << " " << c;
            ASSERT_EQ(motrack::science::alphaOf(destination.at(c, 0)), 255);
        }
    }
    // Transparent pixels keep the destination; clipping keeps the larger destination's remainder.
    FilterImage destination(3, 2, 0xFF102030U);
    motrack::science::compositeSourceOver(FilterImage(2, 3, 0x00FFFFFFU), destination);
    EXPECT_EQ(destination, FilterImage(3, 2, 0xFF102030U));
}

// ---------------------------------------------------------------------------------------------
// Registry, factory and stack

TEST(VideoFilterRegistry, NamesRoundTripAndUnknownClassesAreRejected)
{
    for (const VideoFilterKind kind : motrack::science::trackerVideoFilterKinds())
    {
        const auto filter = motrack::science::makeVideoFilter(kind);
        EXPECT_EQ(filter->kind(), kind);
        EXPECT_EQ(motrack::science::videoFilterKindFromClassName(motrack::science::videoFilterClassName(kind)), kind);
        EXPECT_TRUE(filter->isEnabled());
        EXPECT_FALSE(filter->isInitialized());
    }
    EXPECT_EQ(motrack::science::videoFilterKindFromClassName("org.opensourcephysics.media.core.BarrelPincushionFilter"), std::nullopt);
    EXPECT_EQ(motrack::science::videoFilterKindFromClassName("GhostFilter"), std::nullopt);
    // A value outside the enumeration (for example from a corrupt cast) reaches the defensive fallbacks.
    const auto invalid = static_cast<VideoFilterKind>(99); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_TRUE(motrack::science::videoFilterClassName(invalid).empty());
    EXPECT_THROW((void)motrack::science::makeVideoFilter(invalid), std::invalid_argument);
    EXPECT_TRUE(motrack::science::isTemporalVideoFilter(VideoFilterKind::Ghost));
    EXPECT_TRUE(motrack::science::isTemporalVideoFilter(VideoFilterKind::DarkGhost));
    EXPECT_TRUE(motrack::science::isTemporalVideoFilter(VideoFilterKind::Strobe));
    EXPECT_TRUE(motrack::science::isTemporalVideoFilter(VideoFilterKind::Sum));
    EXPECT_FALSE(motrack::science::isTemporalVideoFilter(VideoFilterKind::Baseline));
    EXPECT_NE(motrack::science::newFilterImageIdentity(), motrack::science::newFilterImageIdentity());
}

TEST(VideoFilterStack, ManagesFiltersInOrder)
{
    VideoFilterStack stack;
    EXPECT_TRUE(stack.empty());
    EXPECT_THROW(stack.add(nullptr), std::invalid_argument);
    stack.add(std::make_unique<NegativeFilter>());
    stack.insert(0, std::make_unique<GrayScaleFilter>());
    stack.insert(99, std::make_unique<DeinterlaceFilter>());
    ASSERT_EQ(stack.size(), 3U);
    EXPECT_EQ(stack.at(0).kind(), VideoFilterKind::GrayScale);
    EXPECT_EQ(std::as_const(stack).at(1).kind(), VideoFilterKind::Negative);
    EXPECT_EQ(stack.at(2).kind(), VideoFilterKind::Deinterlace);
    EXPECT_THROW((void)stack.at(3), std::out_of_range);
    EXPECT_EQ(stack.remove(3), nullptr);
    EXPECT_EQ(stack.remove(0)->kind(), VideoFilterKind::GrayScale);
    EXPECT_EQ(stack.size(), 2U);
    stack.clear();
    EXPECT_TRUE(stack.empty());
}

TEST(VideoFilterStack, PassesFramesThroughWithoutEnabledFilters)
{
    VideoFilterStack stack;
    const FilterImage frame = pattern("noise", 5, 4);
    EXPECT_EQ(&stack.apply(frame), &frame);
    stack.add(std::make_unique<NegativeFilter>()).setEnabled(false);
    EXPECT_EQ(&stack.apply(frame), &frame);
    const FilterImage empty;
    stack.at(0).setEnabled(true);
    EXPECT_EQ(&stack.apply(empty), &empty);
    const FilterImage& negative = stack.apply(frame);
    ASSERT_NE(&negative, &frame);
    EXPECT_EQ(negative.at(0, 0), motrack::science::OPAQUE_ALPHA | (~frame.at(0, 0) & motrack::science::RGB_MASK));
}

TEST(VideoFilterStack, RemovingAnUpstreamFilterReinitializesTemporalState)
{
    VideoFilterStack stack;
    stack.add(std::make_unique<NegativeFilter>());
    auto& ghost = dynamic_cast<GhostFilter&>(stack.add(std::make_unique<GhostFilter>()));
    const FilterImage frame = pattern("blob", 20, 12, 3, 0);
    (void)stack.apply(frame);
    const std::uint64_t identity = ghost.outputIdentity();
    (void)stack.apply(frame);
    EXPECT_EQ(ghost.outputIdentity(), identity);
    (void)stack.remove(0);
    (void)stack.apply(frame);
    EXPECT_NE(ghost.outputIdentity(), identity);
}

TEST(VideoFilter, DegenerateOutputLeavesTheImageUnchanged)
{
    ResizeFilter resize;
    resize.setWidthFactor(0.01);
    const FilterImage frame = pattern("noise", 40, 10);
    EXPECT_EQ(&applyOnce(resize, frame), &frame);
    EXPECT_FALSE(resize.isInitialized());
    resize.setWidthFactor(std::numeric_limits<double>::quiet_NaN());
    EXPECT_TRUE(std::isnan(resize.widthFactor()));
    EXPECT_EQ(&applyOnce(resize, frame), &frame);
    resize.setWidthFactor(-3.0);
    EXPECT_EQ(resize.widthFactor(), 3.0);
    resize.setHeightFactor(25.0);
    EXPECT_EQ(resize.heightFactor(), 10.0);
    EXPECT_EQ(applyOnce(resize, frame).width(), 120);
    EXPECT_EQ(applyOnce(resize, frame).height(), 100);
    resize.setEnabled(false);
    EXPECT_EQ(&applyOnce(resize, frame), &frame);
}

// ---------------------------------------------------------------------------------------------
// Corrected arithmetic, with independent expectations

TEST(BrightnessFilter, CorrectedModeIsExactAtTheDefaultContrast)
{
    // At contrast 50 the mapping is c + brightness exactly; Tracker lowers many results by one.
    std::vector<std::uint32_t> ramp;
    ramp.reserve(256);
    for (std::uint32_t c = 0; c < 256; ++c)
    {
        ramp.push_back((c << 16U) | (c << 8U) | c);
    }
    const FilterImage frame(256, 1, std::move(ramp));
    for (int brightness = -128; brightness <= 128; brightness += 8)
    {
        BrightnessFilter corrected;
        corrected.setBrightness(brightness);
        BrightnessFilter tracker;
        tracker.setBrightness(brightness);
        tracker.setArithmetic(FilterArithmetic::Tracker635);
        EXPECT_EQ(tracker.arithmetic(), FilterArithmetic::Tracker635);
        const FilterImage& exact = applyOnce(corrected, frame);
        const FilterImage& reference = applyOnce(tracker, frame);
        int lowered = 0;
        for (int c = 0; c < 256; ++c)
        {
            const int expected = std::clamp(c + brightness, 0, 255);
            EXPECT_EQ(motrack::science::redOf(exact.at(c, 0)), expected) << brightness << " " << c;
            const int java = motrack::science::redOf(reference.at(c, 0));
            EXPECT_TRUE(java == expected || java == expected - 1);
            lowered += java != expected;
        }
        // Java's results fall a few ulps below the exact integer for large brightness magnitudes.
        if (brightness == 0)
        {
            EXPECT_EQ(lowered, 0);
        }
        if (brightness == 128)
        {
            EXPECT_EQ(lowered, 64);
        }
    }
}

TEST(BrightnessFilter, ResetRestoresDefaultsAndExtremeContrastSaturates)
{
    BrightnessFilter filter;
    EXPECT_EQ(filter.arithmetic(), FilterArithmetic::Corrected);
    filter.setBrightness(40);
    filter.setContrast(100.0);
    EXPECT_EQ(filter.brightness(), 40);
    const FilterImage frame(3, 1, std::vector<std::uint32_t>{0x000000, 0x575757, 0xffffff});
    const FilterImage& output = applyOnce(filter, frame);
    // tan(pi/2) is about 1.6e16: everything away from 127 - 40 saturates.
    EXPECT_EQ(output.at(0, 0) & motrack::science::RGB_MASK, 0x000000U);
    EXPECT_EQ(output.at(2, 0) & motrack::science::RGB_MASK, 0xffffffU);
    filter.clear();
    EXPECT_EQ(filter.brightness(), BrightnessFilter::DEFAULT_BRIGHTNESS);
    EXPECT_EQ(filter.contrast(), BrightnessFilter::DEFAULT_CONTRAST);
    EXPECT_TRUE(applyOnce(filter, frame).sameColors(frame));
}

TEST(PerspectiveFilter, CorrectedBilinearWeightsTheRightNeighbours)
{
    // A half-pixel horizontal shift: output (i, j) samples input (i + 0.5, j) exactly.
    const FilterImage frame(4,
                            3,
                            std::vector<std::uint32_t>{0x000000,
                                                       0x646464,
                                                       0xc8c8c8,
                                                       0xffffff, //
                                                       0x0a0a0a,
                                                       0x6e6e6e,
                                                       0xd2d2d2,
                                                       0xf0f0f0, //
                                                       0x141414,
                                                       0x787878,
                                                       0xdcdcdc,
                                                       0xe6e6e6});
    const PerspectiveQuad out{{{0, 0}, {2, 0}, {2, 2}, {0, 2}}};
    const PerspectiveQuad in{{{0.5, 0}, {2.5, 0}, {2.5, 2}, {0.5, 2}}};
    for (const FilterArithmetic arithmetic : {FilterArithmetic::Corrected, FilterArithmetic::Tracker635})
    {
        PerspectiveFilter filter;
        filter.setArithmetic(arithmetic);
        filter.setFixed(PerspectivePlane::Input, true, 0);
        filter.loadKeyFrame(PerspectivePlane::Input, 0, in);
        filter.loadKeyFrame(PerspectivePlane::Output, 0, out);
        const FilterImage& output = applyOnce(filter, frame);
        const auto value = [&](int x, int y)
        {
            return motrack::science::redOf(output.at(x, y));
        };
        if (arithmetic == FilterArithmetic::Corrected)
        {
            EXPECT_EQ(value(0, 0), (0x00 + 0x64) / 2);
            EXPECT_EQ(value(1, 1), (0x6e + 0xd2) / 2);
        }
        else
        {
            // OSP mixes the pixel below instead of the one to the right.
            EXPECT_EQ(value(0, 0), (0x00 + 0x0a) / 2);
            EXPECT_EQ(value(1, 1), (0x6e + 0x78) / 2);
        }
        // The last row and column are copied from the containing pixel.
        EXPECT_EQ(value(3, 0), 0xff);
        EXPECT_EQ(value(2, 2), 0xdc);
    }
}

// ---------------------------------------------------------------------------------------------
// Perspective key frames and transforms

TEST(PerspectiveFilter, KeyFramesFollowTrackerRules)
{
    PerspectiveFilter filter;
    EXPECT_FALSE(filter.isFixed(PerspectivePlane::Input));
    EXPECT_TRUE(filter.isFixed(PerspectivePlane::Output));
    EXPECT_EQ(filter.corners(PerspectivePlane::Input, 0), std::nullopt);
    EXPECT_EQ(filter.outputToInput(0), std::nullopt);
    EXPECT_EQ(filter.inputToOutput(0), std::nullopt);
    EXPECT_FALSE(filter.removeKeyFrame(PerspectivePlane::Input, 0));

    (void)applyOnce(filter, pattern("noise", 10, 7));
    const PerspectiveQuad defaults{{{2, 1}, {7, 1}, {7, 5}, {2, 5}}};
    EXPECT_EQ(filter.corners(PerspectivePlane::Input, 3), defaults);
    EXPECT_EQ(filter.corners(PerspectivePlane::Output, 3), defaults);

    const PerspectiveQuad a{{{1, 1}, {8, 1}, {8, 6}, {1, 6}}};
    const PerspectiveQuad b{{{2, 2}, {9, 1}, {8, 6}, {0, 5}}};
    filter.setCorners(PerspectivePlane::Input, 5, a);
    filter.setCorners(PerspectivePlane::Input, 9, b);
    filter.setCorners(PerspectivePlane::Output, 5, b); // fixed: stored at key 0
    EXPECT_EQ(filter.keyFrames(PerspectivePlane::Input), (std::vector<int>{0, 5, 9}));
    EXPECT_EQ(filter.keyFrames(PerspectivePlane::Output), (std::vector<int>{0}));
    EXPECT_EQ(filter.corners(PerspectivePlane::Input, 4), defaults);
    EXPECT_EQ(filter.corners(PerspectivePlane::Input, 7), a);
    EXPECT_EQ(filter.corners(PerspectivePlane::Input, 100), b);
    EXPECT_EQ(filter.corners(PerspectivePlane::Output, 7), b);

    EXPECT_FALSE(filter.removeKeyFrame(PerspectivePlane::Input, 2));  // key 0 cannot be removed
    EXPECT_FALSE(filter.removeKeyFrame(PerspectivePlane::Output, 7)); // a fixed plane keeps key 0
    EXPECT_TRUE(filter.removeKeyFrame(PerspectivePlane::Input, 6));
    EXPECT_EQ(filter.keyFrames(PerspectivePlane::Input), (std::vector<int>{0, 9}));

    filter.setFixed(PerspectivePlane::Input, true, 12); // keeps the corners shown at frame 12
    filter.setFixed(PerspectivePlane::Input, true, 12);
    EXPECT_EQ(filter.keyFrames(PerspectivePlane::Input), (std::vector<int>{0}));
    EXPECT_EQ(filter.corners(PerspectivePlane::Input, 0), b);
    filter.setFixed(PerspectivePlane::Input, false, 0);
    EXPECT_FALSE(filter.isFixed(PerspectivePlane::Input));

    // Edited XML without key 0 falls back to the first key.
    PerspectiveFilter loaded;
    loaded.loadKeyFrame(PerspectivePlane::Input, 4, a);
    loaded.loadKeyFrame(PerspectivePlane::Output, 4, b);
    EXPECT_EQ(loaded.corners(PerspectivePlane::Input, 1), a);
    EXPECT_EQ(loaded.corners(PerspectivePlane::Output, 1), b);
    EXPECT_FALSE(loaded.removeKeyFrame(PerspectivePlane::Input, 1));
}

TEST(PerspectiveFilter, TransformsAreInverseProjectiveMaps)
{
    PerspectiveFilter filter;
    const PerspectiveQuad in{{{8.5, 4.25}, {30, 6}, {34.75, 20}, {2, 18}}};
    const PerspectiveQuad out{{{3, 2}, {33, 2}, {33, 20}, {3, 20}}};
    filter.loadKeyFrame(PerspectivePlane::Input, 0, in);
    filter.loadKeyFrame(PerspectivePlane::Output, 0, out);
    const auto maybeToInput = filter.outputToInput(0);
    const auto maybeToOutput = filter.inputToOutput(0);
    ASSERT_TRUE(maybeToInput.has_value());
    ASSERT_TRUE(maybeToOutput.has_value());
    // value_or after ASSERT_TRUE keeps clang-tidy 18's optional-access check satisfied.
    const auto toInput = maybeToInput.value_or(motrack::science::ProjectiveTransform{});
    const auto toOutput = maybeToOutput.value_or(motrack::science::ProjectiveTransform{});
    for (std::size_t i = 0; i < 4; ++i)
    {
        const FilterPoint mapped = toInput.map(out[i]);
        EXPECT_NEAR(mapped.x, in[i].x, 1e-9);
        EXPECT_NEAR(mapped.y, in[i].y, 1e-9);
        const FilterPoint back = toOutput.map(in[i]);
        EXPECT_NEAR(back.x, out[i].x, 1e-9);
        EXPECT_NEAR(back.y, out[i].y, 1e-9);
    }
    const FilterPoint p{17.25, 9.5};
    const FilterPoint roundTrip = toOutput.map(toInput.map(p));
    EXPECT_NEAR(roundTrip.x, p.x, 1e-9);
    EXPECT_NEAR(roundTrip.y, p.y, 1e-9);
    // A zero homogeneous coordinate leaves a point where it is, as OSP does.
    const motrack::science::ProjectiveTransform singular{{1, 0, 0, 0, 1, 0, 0, 0, 0}};
    EXPECT_EQ(singular.map(p), p);
}

// ---------------------------------------------------------------------------------------------
// Rotate, resize and radial coordinate maps

TEST(RotateFilter, CoordinateMapsMatchThePixelMapping)
{
    const FilterImage frame = pattern("noise", 5, 3, 13);
    for (const auto rotation :
         {FilterRotation::None, FilterRotation::CounterClockwise90, FilterRotation::Clockwise90, FilterRotation::Rotate180})
    {
        for (const bool reversed : {false, true})
        {
            RotateFilter filter;
            filter.setRotation(rotation);
            filter.setReversed(reversed);
            EXPECT_EQ(filter.rotation(), rotation);
            EXPECT_EQ(filter.isReversed(), reversed);
            const FilterImage& output = applyOnce(filter, frame);
            for (int y = 0; y < frame.height(); ++y)
            {
                for (int x = 0; x < frame.width(); ++x)
                {
                    // The centre of input pixel (x, y) lands at the centre of the pixel it was copied to.
                    const FilterPoint centre = filter.inputToOutput({x + 0.5, y + 0.5}, frame.width(), frame.height());
                    const auto ox = static_cast<int>(std::floor(centre.x));
                    const auto oy = static_cast<int>(std::floor(centre.y));
                    EXPECT_EQ(output.at(ox, oy) & motrack::science::RGB_MASK, frame.at(x, y) & motrack::science::RGB_MASK);
                    const FilterPoint back = filter.outputToInput(centre, frame.width(), frame.height());
                    EXPECT_DOUBLE_EQ(back.x, x + 0.5);
                    EXPECT_DOUBLE_EQ(back.y, y + 0.5);
                }
            }
        }
    }
    const auto invalid = static_cast<FilterRotation>(9); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_EQ(motrack::science::filterRotationName(invalid), "");
    EXPECT_EQ(motrack::science::filterRotationFromName("cw"), std::nullopt);
    EXPECT_EQ(motrack::science::filterRotationFromName("180"), FilterRotation::Rotate180);
}

TEST(ResizeFilter, ScalesCoordinatesAndAdjustsDvFrames)
{
    ResizeFilter filter;
    filter.setWidthFactor(2.5);
    filter.setHeightFactor(0.5);
    EXPECT_EQ(filter.inputToOutput({4, 6}), (FilterPoint{10, 3}));
    EXPECT_EQ(filter.outputToInput({10, 3}), (FilterPoint{4, 6}));
    ResizeFilter dv;
    const FilterImage& output = applyOnce(dv, solid(720, 480, 0xFF336699U));
    EXPECT_EQ(dv.widthFactor(), 0.889);
    EXPECT_EQ(output.width(), 640);
    EXPECT_EQ(output.height(), 480);
    EXPECT_TRUE(output.sameColors(solid(640, 480, 0xFF336699U)));
}

TEST(RadialDistortionFilter, ParametersAreLimitedAndTheCentreIsFixed)
{
    RadialDistortionFilter filter;
    filter.setFixedRadius(std::numeric_limits<double>::quiet_NaN());
    filter.setSourceFov(std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(filter.fixedRadius(), 0.75);
    EXPECT_EQ(filter.sourceFov(), std::numbers::pi / 2);
    filter.setFixedRadius(-5.0);
    EXPECT_EQ(filter.fixedRadius(), 1.0);
    filter.setFixedRadius(0.01);
    EXPECT_EQ(filter.fixedRadius(), 0.2);
    filter.setSourceFov(10.0);
    EXPECT_EQ(filter.sourceFov(), std::numbers::pi - .0005);
    filter.setSourceFov(0.0);
    EXPECT_EQ(filter.sourceFov(), std::numbers::pi / 18);
    filter.setSourceProjection(RadialProjection::Equisolid);
    filter.setOutputProjection(RadialProjection::Stereographic);
    EXPECT_EQ(filter.sourceProjection(), RadialProjection::Equisolid);
    EXPECT_EQ(filter.outputProjection(), RadialProjection::Stereographic);
    EXPECT_EQ(filter.outputFov(), 0.0);
    EXPECT_EQ(filter.outputToInput({8, 6}, 16, 12), (FilterPoint{8, 6}));
    const FilterPoint corner = filter.outputToInput({0, 0}, 16, 12);
    EXPECT_LT(corner.x, 8);
    EXPECT_LT(corner.y, 6);
    for (const auto projection : {RadialProjection::Rectilinear,
                                  RadialProjection::Equisolid,
                                  RadialProjection::Equidistant,
                                  RadialProjection::Stereographic,
                                  RadialProjection::Orthographic})
    {
        EXPECT_EQ(motrack::science::radialProjectionFromName(motrack::science::radialProjectionName(projection)), projection);
    }
    EXPECT_EQ(motrack::science::radialProjectionFromName("Fisheye"), std::nullopt);
    const auto invalid = static_cast<RadialProjection>(9); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_EQ(motrack::science::radialProjectionName(invalid), "");
}

// ---------------------------------------------------------------------------------------------
// Filter state rules

TEST(TemporalFilters, StateRestartsOnlyForTheDocumentedChanges)
{
    const FilterImage first = pattern("blob", 20, 12, 3, 0);
    const FilterImage second = pattern("blob", 20, 12, 3, 2);
    GhostFilter ghost;
    ghost.setFade(-3.0);
    EXPECT_EQ(ghost.fade(), 1.0);
    ghost.setFade(GhostFilter::DEFAULT_FADE);
    (void)ghost.apply(first, 7);
    const std::uint64_t identity = ghost.outputIdentity();
    ghost.setEnabled(true); // unchanged: no restart
    (void)ghost.apply(second, 7);
    EXPECT_EQ(ghost.outputIdentity(), identity);
    (void)ghost.apply(second, 8); // a different buffer
    EXPECT_NE(ghost.outputIdentity(), identity);
    ghost.clear();
    EXPECT_FALSE(ghost.isInitialized());

    StrobeFilter strobe;
    strobe.setFade(0.25);
    EXPECT_EQ(strobe.fade(), 0.25);
    EXPECT_FALSE(strobe.hasBrightTrails());
    (void)strobe.apply(first, 1);
    strobe.setEnabled(true);
    EXPECT_TRUE(strobe.isInitialized());
    strobe.setBrightTrails(true);
    EXPECT_TRUE(strobe.hasBrightTrails());
    EXPECT_FALSE(strobe.isInitialized());

    DarkGhostFilter dark;
    EXPECT_EQ(dark.kind(), VideoFilterKind::DarkGhost);
}

TEST(SumFilter, CountsImagesAndKeepsJavaFractionRules)
{
    SumFilter sum;
    sum.clear(); // before any input: nothing to restart
    EXPECT_EQ(sum.brightness(), 1.0);
    sum.setBrightness(-0.5);
    EXPECT_EQ(sum.brightness(), 0.5);
    sum.setBrightness(std::numeric_limits<double>::quiet_NaN());
    EXPECT_TRUE(std::isnan(sum.brightness()));
    sum.setMean(true);
    EXPECT_TRUE(sum.isMean());
    const FilterImage gray(2, 2, 0xFF505050U);
    const FilterImage white(2, 2, 0xFFFFFFFFU);
    (void)applyOnce(sum, gray);
    EXPECT_EQ(sum.imageCount(), 1);
    sum.addNextImage();
    const FilterImage& mean = applyOnce(sum, white);
    EXPECT_EQ(sum.imageCount(), 2);
    EXPECT_EQ(mean.at(0, 0) & motrack::science::RGB_MASK, 0xA7A7A7U); // (80 + 255) / 2, truncated
    sum.setMean(false);
    // NaN brightness: (int)min(NaN, 255) is 0.
    EXPECT_EQ(applyOnce(sum, white).at(1, 1) & motrack::science::RGB_MASK, 0U);
    sum.clear();
    EXPECT_EQ(sum.brightness(), 1.0);
    EXPECT_EQ(sum.imageCount(), 1);
}

TEST(BaselineFilter, AcceptsOnlyLargeEnoughImages)
{
    BaselineFilter filter;
    EXPECT_FALSE(filter.setBaselineImage(FilterImage{}));
    EXPECT_TRUE(filter.setBaselineImage(solid(3, 3, 0xFF101010U))); // any size before the first frame
    const FilterImage frame = solid(4, 4, 0xFF808080U);
    EXPECT_TRUE(applyOnce(filter, frame).sameColors(frame)); // the 3 x 3 baseline is dropped
    EXPECT_FALSE(filter.baselineImage().has_value());
    EXPECT_FALSE(filter.setBaselineImage(solid(3, 4, 0xFF101010U)));
    EXPECT_TRUE(filter.setBaselineImage(solid(6, 5, 0xFF102030U)));
    EXPECT_EQ(applyOnce(filter, frame).at(3, 3) & motrack::science::RGB_MASK, 0x706050U);
    ASSERT_TRUE(filter.baselineImage().has_value());
    EXPECT_EQ(filter.baselineImage().value_or(FilterImage{}).width(), 6);
    filter.clear();
    EXPECT_TRUE(applyOnce(filter, frame).sameColors(frame));
}

TEST(PointFilters, ExposeTheirParameters)
{
    // Filters without a Clear button ignore clear().
    NegativeFilter negative;
    const FilterImage frame = pattern("noise", 3, 2);
    negative.clear();
    EXPECT_EQ(applyOnce(negative, frame).at(2, 1) & motrack::science::RGB_MASK, ~frame.at(2, 1) & motrack::science::RGB_MASK);
    GrayScaleFilter gray;
    EXPECT_EQ(gray.weights(), GrayScaleFilter::VIDEO_WEIGHTS);
    gray.setWeights(1, 2, 3);
    EXPECT_EQ(gray.weights(), (std::array<double, 3>{1, 2, 3}));
    LogFilter log;
    EXPECT_EQ(log.level(), 0);
    EXPECT_EQ(log.lookup()[200], 200);
    log.setLevel(30);
    log.setLevel(30);
    EXPECT_EQ(log.level(), 30);
    log.setLevel(0);
    EXPECT_EQ(log.lookup()[17], 17);
    log.setGrayscale(true);
    EXPECT_TRUE(log.isGrayscale());
    DeinterlaceFilter deinterlace;
    EXPECT_FALSE(deinterlace.isOddField());
    deinterlace.setOddField(true);
    EXPECT_TRUE(deinterlace.isOddField());
}

} // namespace
