#include "video_reader.hpp"
#include "video_reader_detail.hpp"

#include <QColor>
#include <QImage>
#include <QSize>
#include <QString>
#include <QTemporaryDir>
#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace
{

#if defined(_WIN32) && defined(_DEBUG)
// Fail the run when the debug CRT detects heap corruption instead of waiting on its dialog (#211).
int endOnCrtReport(int type, char* message, int* /*result*/)
{
    if (type == _CRT_WARN)
    {
        return 0; // Let the CRT handle warnings.
    }
    std::fputs(message, stderr);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

int installCrtReportHook() noexcept
{
    return _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, endOnCrtReport);
}

[[maybe_unused]] const int CRT_REPORT_HOOK = installCrtReportHook();
#endif

constexpr int CHROMA_U = 112;
constexpr int CHROMA_V = 150;

// RGBA rows of these widths are not a multiple of 64 bytes. The H.264 movies in #211 are 1080, 1404, 634, 426 and
// 540 pixels wide. Into tightly packed rows, swscale's x86 SSSE3 converter wrote past each row for widths below 8 or
// 8 to 15 pixels above a multiple of 16. For larger widths 1 to 7 pixels above one, such as 1204, it left that many
// columns unconverted.
constexpr std::array MOVIE_WIDTHS{1080, 1404, 634, 426, 540, 1204};
constexpr std::array FRAME_WIDTHS{1080, 1404, 634, 426, 540, 1204, 635, 31, 1};
constexpr std::array ALIGNED_WIDTHS{16, 32, 1088, 1920};
// Even heights take swscale's unscaled YUV 4:2:0 and 4:2:2 converters; odd heights take its general scaler.
constexpr std::array HEIGHTS{2, 3};

struct Pattern
{
    bool ramp = true;
    bool fullRange = false;

    [[nodiscard]] int luma(int x, int y) const
    {
        return ramp ? 16 + ((x * 7) + (y * 31)) % 220 : 120;
    }

    // ITU-R BT.601, which swscale uses for frames without colorspace details.
    [[nodiscard]] std::array<int, 4> rgba(int x, int y) const
    {
        const double chromaScale = fullRange ? 1.0 : 255.0 / 224.0;
        const double pb = (CHROMA_U - 128) * chromaScale;
        const double pr = (CHROMA_V - 128) * chromaScale;
        const double value = fullRange ? luma(x, y) : (luma(x, y) - 16) * 255.0 / 219.0;
        const auto channel = [](double level)
        {
            return static_cast<int>(std::lround(std::clamp(level, 0.0, 255.0)));
        };
        return {channel(value + (1.402 * pr)), channel(value - (0.344136 * pb) - (0.714136 * pr)), channel(value + (1.772 * pb)), 255};
    }
};

constexpr Pattern RAMP{};
constexpr Pattern FLAT{.ramp = false};

struct Format
{
    AVPixelFormat pixels;
    Pattern pattern;
};

// Tests use one converter per pixel format, as each movie has its own reader. sws_getCachedContext copies the
// options of the context it replaces, including the full range it set for a JPEG-range format (#261).
constexpr std::array FORMATS{Format{.pixels = AV_PIX_FMT_YUV420P, .pattern = RAMP},
                             Format{.pixels = AV_PIX_FMT_YUVJ420P, .pattern = Pattern{.fullRange = true}},
                             Format{.pixels = AV_PIX_FMT_YUV422P, .pattern = RAMP},
                             Format{.pixels = AV_PIX_FMT_YUV444P, .pattern = RAMP}};

void check(int result, const char* operation)
{
    if (result < 0)
    {
        throw std::runtime_error(std::string("Could not ") + operation + '.');
    }
}

struct FreeFrame
{
    void operator()(AVFrame* frame) const
    {
        av_frame_free(&frame);
    }
};

struct FreeMemory
{
    void operator()(std::uint8_t* data) const
    {
        av_free(data);
    }
};

struct FreeCodec
{
    void operator()(AVCodecContext* context) const
    {
        avcodec_free_context(&context);
    }
};

struct FreePacket
{
    void operator()(AVPacket* packet) const
    {
        av_packet_free(&packet);
    }
};

struct CloseOutput
{
    void operator()(AVFormatContext* context) const
    {
        avio_closep(&context->pb);
        avformat_free_context(context);
    }
};

using FramePtr = std::unique_ptr<AVFrame, FreeFrame>;

FramePtr syntheticFrame(AVPixelFormat format, QSize size, const Pattern& pattern)
{
    FramePtr frame(av_frame_alloc());
    if (!frame)
    {
        throw std::bad_alloc();
    }
    frame->format = format;
    frame->width = size.width();
    frame->height = size.height();
    check(av_frame_get_buffer(frame.get(), 0), "allocate a synthetic frame");
    const auto* descriptor = av_pix_fmt_desc_get(format);
    const auto chroma = [](int length, int shift)
    {
        return (length + (1 << shift) - 1) >> shift;
    };
    const auto chromaWidth = static_cast<std::size_t>(chroma(size.width(), descriptor->log2_chroma_w));
    for (int y = 0; y < size.height(); ++y)
    {
        auto* row = frame->data[0] + (static_cast<std::ptrdiff_t>(y) * frame->linesize[0]);
        std::memset(row, 0, static_cast<std::size_t>(frame->linesize[0]));
        for (int x = 0; x < size.width(); ++x)
        {
            row[x] = static_cast<std::uint8_t>(pattern.luma(x, y));
        }
    }
    for (int y = 0; y < chroma(size.height(), descriptor->log2_chroma_h); ++y)
    {
        std::memset(frame->data[1] + (static_cast<std::ptrdiff_t>(y) * frame->linesize[1]), CHROMA_U, chromaWidth);
        std::memset(frame->data[2] + (static_cast<std::ptrdiff_t>(y) * frame->linesize[2]), CHROMA_V, chromaWidth);
    }
    return frame;
}

// Describes the first pixel that differs from the pattern by more than the tolerance in any channel.
std::string firstMismatch(std::span<const std::uint8_t> pixels, qsizetype stride, QSize size, const Pattern& pattern, int tolerance)
{
    for (int y = 0; y < size.height(); ++y)
    {
        for (int x = 0; x < size.width(); ++x)
        {
            const auto expected = pattern.rgba(x, y);
            const auto actual = pixels.subspan(static_cast<std::size_t>((y * stride) + (qsizetype{x} * 4)), expected.size());
            for (std::size_t channel = 0; channel < expected.size(); ++channel)
            {
                if (std::abs(actual[channel] - expected.at(channel)) > tolerance)
                {
                    return "pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") channel " + std::to_string(channel) + " is " +
                           std::to_string(actual[channel]) + ", expected " + std::to_string(expected.at(channel));
                }
            }
        }
    }
    return {};
}

std::string firstMismatch(const QImage& image, const Pattern& pattern, int tolerance)
{
    return firstMismatch(
        {image.constBits(), static_cast<std::size_t>(image.sizeInBytes())}, image.bytesPerLine(), image.size(), pattern, tolerance);
}

// The conversion before #211 was fixed: swscale wrote straight into Qt's tightly packed rows.
QImage previousConversion(const AVFrame& frame)
{
    QImage image(frame.width, frame.height, QImage::Format_RGBA8888);
    const std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scale(sws_getContext(frame.width,
                                                                                       frame.height,
                                                                                       static_cast<AVPixelFormat>(frame.format),
                                                                                       frame.width,
                                                                                       frame.height,
                                                                                       AV_PIX_FMT_RGBA,
                                                                                       SWS_BILINEAR,
                                                                                       nullptr,
                                                                                       nullptr,
                                                                                       nullptr),
                                                                        &sws_freeContext);
    if (image.isNull() || !scale)
    {
        throw std::runtime_error("Could not set up the previous conversion.");
    }
    const std::array<std::uint8_t*, 4> planes{image.bits(), nullptr, nullptr, nullptr};
    const std::array<int, 4> strides{static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    check(sws_scale(scale.get(), frame.data, frame.linesize, 0, frame.height, planes.data(), strides.data()),
          "run the previous conversion");
    return image;
}

void writePackets(AVCodecContext& codec, AVFormatContext& format, const AVStream& stream, AVPacket& packet)
{
    for (;;)
    {
        const auto result = avcodec_receive_packet(&codec, &packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
        {
            return;
        }
        check(result, "encode a synthetic frame");
        av_packet_rescale_ts(&packet, codec.time_base, stream.time_base);
        packet.stream_index = stream.index;
        check(av_interleaved_write_frame(&format, &packet), "write a synthetic packet");
    }
}

// MoTrack's FFmpeg build has no portable H.264 encoder, so the movies use FFmpeg's own encoders. Both decoders
// return YUV 4:2:0 frames, as H.264 does, and MPEG-4 Part 2 crops macroblock-coded frames to the display width.
void writeMovie(const QString& path, AVCodecID codecId, QSize size, const Pattern& pattern, int frames)
{
    const auto name = path.toUtf8();
    AVFormatContext* output = nullptr;
    check(avformat_alloc_output_context2(&output, nullptr, "avi", name.constData()), "allocate the movie muxer");
    const std::unique_ptr<AVFormatContext, CloseOutput> format(output);
    const auto* encoder = avcodec_find_encoder(codecId);
    auto* stream = avformat_new_stream(format.get(), nullptr);
    const std::unique_ptr<AVCodecContext, FreeCodec> codec(avcodec_alloc_context3(encoder));
    const std::unique_ptr<AVPacket, FreePacket> packet(av_packet_alloc());
    if (encoder == nullptr || stream == nullptr || !codec || !packet)
    {
        throw std::runtime_error("Could not set up the synthetic movie encoder.");
    }
    codec->width = size.width();
    codec->height = size.height();
    codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->time_base = AVRational{.num = 1, .den = 25};
    codec->framerate = AVRational{.num = 25, .den = 1};
    codec->flags |= AV_CODEC_FLAG_QSCALE; // Lossy encoders keep a fixed, fine quantizer.
    codec->global_quality = FF_QP2LAMBDA * 2;
    check(avcodec_open2(codec.get(), encoder, nullptr), "open the synthetic movie encoder");
    check(avcodec_parameters_from_context(stream->codecpar, codec.get()), "describe the synthetic movie stream");
    stream->time_base = codec->time_base;
    check(avio_open(&format->pb, name.constData(), AVIO_FLAG_WRITE), "create the synthetic movie");
    check(avformat_write_header(format.get(), nullptr), "write the synthetic movie header");
    for (int number = 0; number < frames; ++number)
    {
        const auto frame = syntheticFrame(AV_PIX_FMT_YUV420P, size, pattern);
        frame->pts = number;
        frame->quality = codec->global_quality;
        check(avcodec_send_frame(codec.get(), frame.get()), "submit a synthetic frame");
        writePackets(*codec, *format, *stream, *packet);
    }
    check(avcodec_send_frame(codec.get(), nullptr), "drain the synthetic movie encoder");
    writePackets(*codec, *format, *stream, *packet);
    check(av_write_trailer(format.get()), "finish the synthetic movie");
}

TEST(RgbaFrameConverter, PadsRowsToSwscaleVectorBlocks)
{
    using motrack::detail::paddedRgbaLayout;
    struct Expected
    {
        int width;
        int height;
        int stride;
    };
    for (const auto& expected : std::array{Expected{.width = 1080, .height = 1920, .stride = 4352},
                                           Expected{.width = 1920, .height = 1080, .stride = 7680},
                                           Expected{.width = 634, .height = 456, .stride = 2560},
                                           Expected{.width = 1, .height = 1, .stride = 64}})
    {
        SCOPED_TRACE(expected.width);
        const auto layout = paddedRgbaLayout(expected.width, expected.height);
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->stride, expected.stride);
        EXPECT_EQ(layout->bytes, (static_cast<std::size_t>(expected.stride) * static_cast<std::size_t>(expected.height)) + 64);
    }
    EXPECT_FALSE(paddedRgbaLayout(0, 1));
    EXPECT_FALSE(paddedRgbaLayout(1, 0));
    EXPECT_FALSE(paddedRgbaLayout(-1, 1));
    EXPECT_FALSE(paddedRgbaLayout(INT_MAX, 1));
}

TEST(RgbaFrameConverter, RejectsFramesItCannotConvert)
{
    motrack::detail::RgbaFrameConverter converter;
    const auto frame = syntheticFrame(AV_PIX_FMT_YUV420P, {1080, 2}, RAMP);
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(1080 * 4 * 2));
    EXPECT_THROW(converter.convertInto(*frame, packed), motrack::VideoError);

    const FramePtr empty(av_frame_alloc());
    ASSERT_TRUE(empty);
    EXPECT_THROW(static_cast<void>(converter.convert(*empty)), motrack::VideoError);
    EXPECT_THROW(converter.convertInto(*empty, packed), motrack::VideoError);

    // swscale cannot convert hardware frames, which MoTrack's decoders never request.
    empty->width = 16;
    empty->height = 2;
    empty->format = AV_PIX_FMT_CUDA;
    EXPECT_THROW(converter.convertInto(*empty, packed), motrack::VideoError);
}

TEST(RgbaFrameConverter, KeepsSwscaleWritesInsideThePaddedLayout)
{
    constexpr std::uint8_t CANARY = 0xa5;
    constexpr std::size_t GUARD_BYTES = 256;
    for (const auto& format : FORMATS)
    {
        motrack::detail::RgbaFrameConverter converter;
        for (const auto width : FRAME_WIDTHS)
        {
            for (const auto height : HEIGHTS)
            {
                SCOPED_TRACE(std::string(av_get_pix_fmt_name(format.pixels)) + " " + std::to_string(width) + "x" + std::to_string(height));
                const auto frame = syntheticFrame(format.pixels, {width, height}, format.pattern);
                const auto layout = motrack::detail::paddedRgbaLayout(width, height);
                ASSERT_TRUE(layout.has_value());
                EXPECT_EQ(layout->stride % 64, 0);
                const auto size = layout->bytes + GUARD_BYTES;
                const std::unique_ptr<std::uint8_t, FreeMemory> memory(static_cast<std::uint8_t*>(av_malloc(size)));
                ASSERT_TRUE(memory);
                const std::span<std::uint8_t> buffer(memory.get(), size);
                std::ranges::fill(buffer, CANARY);
                converter.convertInto(*frame, buffer.first(layout->bytes));
                EXPECT_TRUE(std::ranges::all_of(buffer.subspan(layout->bytes), [](std::uint8_t value) { return value == CANARY; }));
                EXPECT_EQ(firstMismatch(buffer, layout->stride, {width, height}, format.pattern, 3), "");
            }
        }
    }
}

TEST(RgbaFrameConverter, MatchesThePreviousUnpaddedConversionAtAlignedWidths)
{
    for (const auto& format : FORMATS)
    {
        motrack::detail::RgbaFrameConverter converter;
        for (const auto width : ALIGNED_WIDTHS)
        {
            for (const auto height : HEIGHTS)
            {
                SCOPED_TRACE(std::string(av_get_pix_fmt_name(format.pixels)) + " " + std::to_string(width) + "x" + std::to_string(height));
                const auto frame = syntheticFrame(format.pixels, {width, height}, format.pattern);
                const auto previous = previousConversion(*frame);
                const auto image = converter.convert(*frame);
                ASSERT_EQ(image.size(), previous.size());
                EXPECT_EQ(image.format(), QImage::Format_RGBA8888);
                ASSERT_EQ(image.bytesPerLine(), previous.bytesPerLine());
                for (int y = 0; y < height; ++y)
                {
                    EXPECT_EQ(std::memcmp(image.constScanLine(y), previous.constScanLine(y), static_cast<std::size_t>(width) * 4), 0) << y;
                }
            }
        }
    }
}

TEST(RgbaFrameConverter, ImagesOwnTheirPixelsAcrossThreads)
{
    QImage image;
    {
        motrack::detail::RgbaFrameConverter converter;
        const auto frame = syntheticFrame(AV_PIX_FMT_YUV420P, {1080, 4}, RAMP);
        image = converter.convert(*frame);
    }
    const auto layout = motrack::detail::paddedRgbaLayout(1080, 4);
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(image.bytesPerLine(), layout->stride);
    const auto expected = image.copy();
    std::string seen;
    std::thread(
        [copy = image, &seen]() mutable
        {
            seen = firstMismatch(copy, RAMP, 3);
            copy.setPixelColor(0, 0, QColor(Qt::red));
        })
        .join();
    EXPECT_EQ(seen, "");
    EXPECT_EQ(image, expected);
}

TEST(VideoReader, DecodesUnalignedMovieWidthsIntoPaddedImages)
{
    struct Movie
    {
        AVCodecID codec;
        QSize size;
        Pattern pattern;
        int tolerance;
    };
    std::vector<Movie> movies;
    for (const auto width : MOVIE_WIDTHS)
    {
        movies.push_back({.codec = AV_CODEC_ID_FFV1, .size = {width, 4}, .pattern = RAMP, .tolerance = 3});
        movies.push_back({.codec = AV_CODEC_ID_MPEG4, .size = {width, 16}, .pattern = FLAT, .tolerance = 4});
    }
    movies.push_back({.codec = AV_CODEC_ID_FFV1, .size = {635, 5}, .pattern = RAMP, .tolerance = 3});

    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    for (const auto& movie : movies)
    {
        const auto name = QString("%1-%2x%3.avi").arg(avcodec_get_name(movie.codec)).arg(movie.size.width()).arg(movie.size.height());
        SCOPED_TRACE(name.toStdString());
        const auto path = temporary.filePath(name);
        writeMovie(path, movie.codec, movie.size, movie.pattern, 2);
        {
            motrack::VideoReader reader(path, motrack::VideoOrientation::Encoded);
            const auto frame = reader.readNext();
            const auto layout = motrack::detail::paddedRgbaLayout(movie.size.width(), movie.size.height());
            ASSERT_TRUE(frame.has_value() && layout.has_value());
            ASSERT_EQ(frame->image.size(), movie.size);
            EXPECT_EQ(frame->image.format(), QImage::Format_RGBA8888);
            EXPECT_EQ(frame->image.bytesPerLine(), layout->stride);
            EXPECT_EQ(firstMismatch(frame->image, movie.pattern, movie.tolerance), "");
        }
        // Indexing converts and frees the first frame, where the debug CRT detected the overrun.
        const std::atomic_bool cancelled{false};
        EXPECT_EQ(motrack::buildMovieIndex(path, motrack::VideoOrientation::Encoded, cancelled).frameCount, 2);
    }
}

} // namespace
