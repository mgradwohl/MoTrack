#include "capture_encoder.hpp"

#include "video_reader.hpp"

#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace
{

using std::chrono::microseconds;

// Independent expectations: each frame time is the camera time relative to the first frame,
// rounded to the 1/90000 s QuickTime track timescale.
constexpr std::array<std::int64_t, 5> CAMERA_TIMES{1'000'000, 1'033'333, 1'070'000, 1'099'000, 1'140'500};
constexpr std::array<double, 5> EXPECTED_SECONDS{0.0, 3000.0 / 90000, 6300.0 / 90000, 8910.0 / 90000, 12645.0 / 90000};

QImage solid(QColor color, int width = 64, int height = 48)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

void expectColor(QRgb actual, int red, int green, int blue, int tolerance)
{
    EXPECT_NEAR(qRed(actual), red, tolerance);
    EXPECT_NEAR(qGreen(actual), green, tolerance);
    EXPECT_NEAR(qBlue(actual), blue, tolerance);
}

// A 4:2:0 or 4:2:2 frame of one colour in the given plane layout.
struct PlanarFrame
{
    std::vector<std::uint8_t> luma;
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    motrack::CaptureImage image;
};

PlanarFrame planar(motrack::CapturePixelFormat format, std::uint8_t y, std::uint8_t u, std::uint8_t v, int width = 16, int height = 8)
{
    using motrack::CapturePixelFormat;
    PlanarFrame frame;
    frame.image = {.width = width, .height = height, .format = format};
    const auto lumaSize = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (format == CapturePixelFormat::Nv12 || format == CapturePixelFormat::Nv21)
    {
        frame.luma.assign(lumaSize, y);
        for (int i = 0; i < width * height / 4; ++i)
        {
            frame.first.push_back(format == CapturePixelFormat::Nv12 ? u : v);
            frame.first.push_back(format == CapturePixelFormat::Nv12 ? v : u);
        }
        frame.image.planes = {frame.luma.data(), frame.first.data(), nullptr};
        frame.image.strides = {width, width, 0};
    }
    else if (format == CapturePixelFormat::Yuyv || format == CapturePixelFormat::Uyvy)
    {
        for (int i = 0; i < width * height / 2; ++i)
        {
            const std::array<std::uint8_t, 4> yuyv{y, u, y, v};
            const std::array<std::uint8_t, 4> uyvy{u, y, v, y};
            const auto& pair = format == CapturePixelFormat::Yuyv ? yuyv : uyvy;
            frame.luma.insert(frame.luma.end(), pair.begin(), pair.end());
        }
        frame.image.planes = {frame.luma.data(), nullptr, nullptr};
        frame.image.strides = {width * 2, 0, 0};
    }
    else
    {
        const auto rows = format == CapturePixelFormat::Yuv422p ? height : height / 2;
        const auto chroma = static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(rows);
        frame.luma.assign(lumaSize, y);
        // YV12 stores V before U.
        frame.first.assign(chroma, format == CapturePixelFormat::Yv12 ? v : u);
        frame.second.assign(chroma, format == CapturePixelFormat::Yv12 ? u : v);
        frame.image.planes = {frame.luma.data(), frame.first.data(), frame.second.data()};
        frame.image.strides = {width, width / 2, width / 2};
    }
    return frame;
}

TEST(CaptureEncoder, RecordsTheCameraFrameTimesThatTheMovieIndexReports)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QString::fromUtf8("capture-測定.mov"));
    const std::array colors{QColor(200, 60, 30), QColor(30, 200, 60), QColor(60, 30, 200), QColor(220, 220, 40), QColor(40, 40, 40)};
    motrack::CaptureSummary summary;
    {
        motrack::CaptureEncoder encoder(path);
        for (std::size_t frame = 0; frame < CAMERA_TIMES.size(); ++frame)
        {
            const auto image = solid(colors.at(frame));
            ASSERT_TRUE(encoder.write(motrack::captureImageFromQImage(image), microseconds{CAMERA_TIMES.at(frame)}));
        }
        summary = encoder.finish();
    }
    EXPECT_EQ(summary.frames, 5);
    EXPECT_EQ(summary.duration, microseconds{140'500});
    const std::atomic_bool cancelled{false};
    const auto index = motrack::buildMovieIndex(path, motrack::VideoOrientation::Display, cancelled);
    ASSERT_EQ(index.frameCount, 5);
    ASSERT_EQ(index.frameTimes.size(), 5u);
    for (std::size_t frame = 0; frame < EXPECTED_SECONDS.size(); ++frame)
        EXPECT_NEAR(index.frameTimes.at(frame), EXPECTED_SECONDS.at(frame), 1e-9) << frame;
    // Camera times use the 1/90000 s ticks (33.333 ms is not a whole millisecond), so they keep it as their precision.
    EXPECT_EQ(index.timeBase,
              (motrack::FrameTimeBase{.numerator = 1,
                                      .denominator = motrack::CaptureEncoder::TIMESCALE,
                                      .precisionNumerator = 1,
                                      .precisionDenominator = motrack::CaptureEncoder::TIMESCALE}));
    motrack::VideoReader reader(path);
    for (const auto& color : colors)
    {
        const auto decoded = reader.readNext();
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->image.size(), QSize(64, 48));
        // Motion JPEG at the fixed quantizer keeps flat colours within a few levels.
        expectColor(decoded->image.pixel(32, 24), color.red(), color.green(), color.blue(), 6);
    }
}

TEST(CaptureEncoder, WholeMillisecondCameraTimesIndexWithMillisecondPrecision)
{
    // A camera clock of whole milliseconds at 30 fps (0, 33, 67, 100 ms...) written in 1/90000 s ticks, as a
    // Matroska capture remuxed into a finer time base would be: FFmpeg reports the 1/90000 s time base, and the index
    // infers from the timestamps that they are only precise to 1 ms (#335).
    QTemporaryDir temporary;
    const auto path = temporary.filePath("milliseconds.mov");
    constexpr int FRAMES = 8;
    {
        motrack::CaptureEncoder encoder(path);
        for (int frame = 0; frame < FRAMES; ++frame)
        {
            const auto milliseconds = std::llround(frame * 1000.0 / 30.0);
            ASSERT_TRUE(encoder.write(motrack::captureImageFromQImage(solid(QColor(10 * frame, 80, 160))),
                                      microseconds{5'000'000 + milliseconds * 1000}));
        }
        EXPECT_EQ(encoder.finish().frames, FRAMES);
    }
    const std::atomic_bool cancelled{false};
    const auto index = motrack::buildMovieIndex(path, motrack::VideoOrientation::Encoded, cancelled);
    ASSERT_EQ(index.frameCount, FRAMES);
    ASSERT_EQ(index.frameTimes.size(), static_cast<std::size_t>(FRAMES));
    for (int frame = 0; frame < FRAMES; ++frame)
        EXPECT_NEAR(
            index.frameTimes.at(static_cast<std::size_t>(frame)), static_cast<double>(std::llround(frame * 1000.0 / 30.0)) / 1000.0, 1e-9)
            << frame;
    EXPECT_EQ(
        index.timeBase,
        (motrack::FrameTimeBase{
            .numerator = 1, .denominator = motrack::CaptureEncoder::TIMESCALE, .precisionNumerator = 1, .precisionDenominator = 1000}));
    EXPECT_EQ(index.timeBase.resolutionSeconds(), 0.001);
}

TEST(CaptureEncoder, SkipsFramesWhoseTimeDoesNotAdvanceAndRefusesANewFrameSize)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("repeats.mov");
    motrack::CaptureEncoder encoder(path);
    const auto image = solid(Qt::gray);
    const auto larger = solid(Qt::gray, 128, 96);
    EXPECT_TRUE(encoder.write(motrack::captureImageFromQImage(image), microseconds{500}));
    EXPECT_FALSE(encoder.write(motrack::captureImageFromQImage(image), microseconds{505})); // Same 1/90000 s tick.
    EXPECT_FALSE(encoder.write(motrack::captureImageFromQImage(image), microseconds{100}));
    // Scaling a resized frame would distort the recorded geometry; the encoder refuses it.
    EXPECT_THROW((void)encoder.write(motrack::captureImageFromQImage(larger), microseconds{20'500}), motrack::VideoError);
    EXPECT_TRUE(encoder.write(motrack::captureImageFromQImage(image), microseconds{20'500}));
    const auto summary = encoder.finish();
    EXPECT_EQ(summary.frames, 2);
    EXPECT_EQ(summary.duration, microseconds{20'000});
    EXPECT_THROW((void)encoder.finish(), motrack::VideoError);
    EXPECT_THROW((void)encoder.write(motrack::captureImageFromQImage(image), microseconds{90'000}), motrack::VideoError);
    const std::atomic_bool cancelled{false};
    const auto index = motrack::buildMovieIndex(path, motrack::VideoOrientation::Display, cancelled);
    ASSERT_EQ(index.frameTimes.size(), 2u);
    EXPECT_NEAR(index.frameTimes.back(), 1800.0 / 90000, 1e-9);
    motrack::VideoReader reader(path);
    (void)reader.readNext();
    const auto second = reader.readNext();
    ASSERT_TRUE(second);
    EXPECT_EQ(second->image.size(), QSize(64, 48));
}

TEST(CaptureEncoder, ReportsUnwritableFilesAndUnusableFrames)
{
    QTemporaryDir temporary;
    EXPECT_THROW((void)motrack::CaptureEncoder(temporary.filePath("missing/clip.mov")), motrack::VideoError);
    {
        motrack::CaptureEncoder empty(temporary.filePath("empty.mov"));
        EXPECT_EQ(empty.finish().frames, 0);
    }
    motrack::CaptureEncoder encoder(temporary.filePath("frames.mov"));
    auto missing = planar(motrack::CapturePixelFormat::Nv12, 81, 90, 240);
    missing.image.planes[1] = nullptr;
    EXPECT_THROW((void)encoder.write(missing.image, microseconds{0}), motrack::VideoError);
    EXPECT_THROW((void)encoder.write(motrack::CaptureImage{}, microseconds{0}), motrack::VideoError);
    EXPECT_EQ(motrack::captureImageFromQImage(QImage(8, 8, QImage::Format_Mono)).width, 0);
}

TEST(CaptureConverter, ConvertsCameraLayoutsAndRangesToPreviewImages)
{
    using motrack::CapturePixelFormat;
    motrack::CaptureConverter converter;
    const auto convert = [&converter](const motrack::CaptureImage& image)
    {
        return converter.toImage(image, {}).pixel(4, 4);
    };
    // BT.601 video-range red, its full-range equivalent, blue in planar layouts (YV12 swaps the chroma
    // planes), and mid grey in the packed layouts.
    expectColor(convert(planar(CapturePixelFormat::Nv12, 81, 90, 240).image), 255, 0, 0, 12);
    auto full = planar(CapturePixelFormat::Nv21, 76, 85, 255);
    full.image.fullRange = true;
    expectColor(convert(full.image), 255, 0, 0, 12);
    expectColor(convert(planar(CapturePixelFormat::Yuv420p, 41, 240, 110).image), 0, 0, 255, 12);
    expectColor(convert(planar(CapturePixelFormat::Yv12, 41, 240, 110).image), 0, 0, 255, 12);
    expectColor(convert(planar(CapturePixelFormat::Yuv422p, 41, 240, 110).image), 0, 0, 255, 12);
    expectColor(convert(planar(CapturePixelFormat::Yuyv, 126, 128, 128).image), 128, 128, 128, 4);
    expectColor(convert(planar(CapturePixelFormat::Uyvy, 126, 128, 128).image), 128, 128, 128, 4);
    QImage gray(8, 8, QImage::Format_Grayscale8);
    gray.fill(200);
    expectColor(convert(motrack::captureImageFromQImage(gray)), 200, 200, 200, 1);
    auto rgba = solid(QColor(10, 120, 250)).convertToFormat(QImage::Format_RGBA8888);
    expectColor(convert(motrack::captureImageFromQImage(rgba)), 10, 120, 250, 1);
    for (const auto format : {CapturePixelFormat::Argb,
                              CapturePixelFormat::Xrgb,
                              CapturePixelFormat::Abgr,
                              CapturePixelFormat::Xbgr,
                              CapturePixelFormat::Rgbx,
                              CapturePixelFormat::Bgrx,
                              CapturePixelFormat::Bgra})
    {
        // Byte order: the alpha or padding byte is opaque, the others are 10, 120 and 250 in name order.
        std::vector<std::uint8_t> pixels;
        for (int pixel = 0; pixel < 8 * 8; ++pixel)
            switch (format)
            {
            case CapturePixelFormat::Argb:
            case CapturePixelFormat::Xrgb:
                pixels.insert(pixels.end(), {255, 10, 120, 250});
                break;
            case CapturePixelFormat::Abgr:
            case CapturePixelFormat::Xbgr:
                pixels.insert(pixels.end(), {255, 250, 120, 10});
                break;
            case CapturePixelFormat::Rgbx:
                pixels.insert(pixels.end(), {10, 120, 250, 255});
                break;
            default:
                pixels.insert(pixels.end(), {250, 120, 10, 255});
                break;
            }
        const motrack::CaptureImage image{.width = 8, .height = 8, .format = format, .planes = {pixels.data()}, .strides = {32}};
        SCOPED_TRACE(static_cast<int>(format));
        expectColor(convert(image), 10, 120, 250, 1);
    }
    const auto large = solid(Qt::white, 64, 48);
    EXPECT_EQ(converter.toImage(motrack::captureImageFromQImage(large), {32, 32}).size(), QSize(32, 24));
    EXPECT_EQ(converter.toImage(motrack::captureImageFromQImage(large), {640, 480}).size(), QSize(64, 48));
}

} // namespace
