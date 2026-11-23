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
#include <libavutil/cpu.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
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
#include <cstdarg>
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
#include <string_view>
#include <thread>
#include <utility>
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
// Even widths and heights take swscale's unscaled YUV 4:2:0 and 4:2:2 converters; other sizes take its general scaler.
constexpr std::array HEIGHTS{2, 3};

// The red and blue luma weights of a YCbCr matrix.
struct Matrix
{
    double red;
    double blue;
};

constexpr Matrix BT601{.red = 0.299, .blue = 0.114}; // ITU-R BT.601, swscale's default.
constexpr Matrix BT709{.red = 0.2126, .blue = 0.0722};
constexpr Matrix BT2020{.red = 0.2627, .blue = 0.0593};

struct Pattern
{
    bool ramp = true;
    bool fullRange = false;
    int u = CHROMA_U;
    int v = CHROMA_V;
    Matrix matrix = BT601;

    [[nodiscard]] int luma(int x, int y) const
    {
        return ramp ? 16 + ((x * 7) + (y * 31)) % 220 : 120;
    }

    // Inverts Y = Kr R + Kg G + Kb B, Pb = (B - Y) / (2 (1 - Kb)) and Pr = (R - Y) / (2 (1 - Kr)). Limited range
    // puts black at Y = 16, white at Y = 235 and spans 224 levels of chroma.
    [[nodiscard]] std::array<int, 4> rgba(int x, int y) const
    {
        const double chromaScale = fullRange ? 1.0 : 255.0 / 224.0;
        const double pb = (u - 128) * chromaScale;
        const double pr = (v - 128) * chromaScale;
        const double value = fullRange ? luma(x, y) : (luma(x, y) - 16) * 255.0 / 219.0;
        const double red = value + (2 * (1 - matrix.red) * pr);
        const double blue = value + (2 * (1 - matrix.blue) * pb);
        const double green = (value - (matrix.red * red) - (matrix.blue * blue)) / (1 - matrix.red - matrix.blue);
        const auto channel = [](double level)
        {
            return static_cast<int>(std::lround(std::clamp(level, 0.0, 255.0)));
        };
        return {channel(red), channel(green), channel(blue), 255};
    }
};

constexpr Pattern RAMP{};
constexpr Pattern FLAT{.ramp = false};
constexpr Pattern FULL_RAMP{.fullRange = true};

struct Format
{
    AVPixelFormat pixels;
    Pattern pattern;
};

// One converter sees every format and size in turn, as a reader does when a stream changes format (#261).
constexpr std::array FORMATS{Format{.pixels = AV_PIX_FMT_YUV420P, .pattern = RAMP},
                             Format{.pixels = AV_PIX_FMT_YUVJ420P, .pattern = FULL_RAMP},
                             Format{.pixels = AV_PIX_FMT_YUV422P, .pattern = RAMP},
                             Format{.pixels = AV_PIX_FMT_YUV444P, .pattern = RAMP},
                             Format{.pixels = AV_PIX_FMT_NV12, .pattern = RAMP},
                             Format{.pixels = AV_PIX_FMT_YUV420P10, .pattern = RAMP}};

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
    const QSize chromaSize(chroma(size.width(), descriptor->log2_chroma_w), chroma(size.height(), descriptor->log2_chroma_h));
    for (int plane = 0; plane < 4 && frame->data[plane] != nullptr; ++plane)
    {
        const auto rows = plane == 0 ? size.height() : chromaSize.height();
        std::memset(frame->data[plane], 0, static_cast<std::size_t>(rows) * static_cast<std::size_t>(frame->linesize[plane]));
    }
    // Writes an 8-bit level at the component's bit depth, following the descriptor's planes, steps and offsets.
    const auto write = [&frame](const AVComponentDescriptor& component, int x, int y, int level)
    {
        auto* sample = frame->data[component.plane] + (static_cast<std::ptrdiff_t>(y) * frame->linesize[component.plane]) +
                       (static_cast<std::ptrdiff_t>(x) * component.step) + component.offset;
        const auto value = (level << (component.depth - 8)) << component.shift;
        if (component.depth > 8)
        {
            const auto word = static_cast<std::uint16_t>(value);
            std::memcpy(sample, &word, sizeof(word));
        }
        else
        {
            *sample = static_cast<std::uint8_t>(value);
        }
    };
    for (int y = 0; y < size.height(); ++y)
    {
        for (int x = 0; x < size.width(); ++x)
        {
            write(descriptor->comp[0], x, y, pattern.luma(x, y));
        }
    }
    for (int y = 0; descriptor->nb_components >= 3 && y < chromaSize.height(); ++y)
    {
        for (int x = 0; x < chromaSize.width(); ++x)
        {
            write(descriptor->comp[1], x, y, pattern.u);
            write(descriptor->comp[2], x, y, pattern.v);
        }
    }
    return frame;
}

FramePtr taggedFrame(AVPixelFormat format, QSize size, const Pattern& pattern, AVColorRange range, AVColorSpace space)
{
    auto frame = syntheticFrame(format, size, pattern);
    frame->color_range = range;
    frame->colorspace = space;
    return frame;
}

// Collects the warnings swscale logs while it exists. The FFmpeg log callback is process-wide.
class SwscaleWarnings
{
  public:
    SwscaleWarnings(const SwscaleWarnings&) = delete;
    SwscaleWarnings& operator=(const SwscaleWarnings&) = delete;
    SwscaleWarnings()
    {
        lines().clear();
        av_log_set_callback(collect);
    }
    ~SwscaleWarnings()
    {
        av_log_set_callback(av_log_default_callback);
    }

    [[nodiscard]] static std::vector<std::string> logged()
    {
        return lines();
    }

  private:
    static std::vector<std::string>& lines()
    {
        static std::vector<std::string> collected;
        return collected;
    }

    static void collect(void* context, int level, const char* format, va_list arguments)
    {
        const auto* type = context != nullptr ? *static_cast<const AVClass* const*>(context) : nullptr;
        if (level > AV_LOG_WARNING || type == nullptr || std::string_view(type->class_name) != "SWScaler")
        {
            return;
        }
        std::array<char, 1024> line{};
        int prefix = 1;
        av_log_format_line2(context, level, format, arguments, line.data(), static_cast<int>(line.size()), &prefix);
        lines().emplace_back(line.data());
    }
};

// Disables FFmpeg's SIMD code, so swscale takes its C converters, until the end of the scope.
class WithoutSimd
{
  public:
    WithoutSimd(const WithoutSimd&) = delete;
    WithoutSimd& operator=(const WithoutSimd&) = delete;
    WithoutSimd()
    {
        av_force_cpu_flags(0);
    }
    ~WithoutSimd()
    {
        av_force_cpu_flags(-1);
    }
};

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

// MoTrack's FFmpeg build has no portable H.264 encoder, so the movies use FFmpeg's own encoders. Their decoders
// return YUV 4:2:0 frames, as H.264 does, and MPEG-4 Part 2 crops macroblock-coded frames to the display width.
// A full-range pattern is encoded with JPEG range, which the Motion JPEG decoder returns as yuvj420p.
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
    codec->color_range = pattern.fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
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
        const auto frame = taggedFrame(AV_PIX_FMT_YUV420P, size, pattern, codec->color_range, AVCOL_SPC_UNSPECIFIED);
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
    EXPECT_EQ(converter.contextsCreated(), 0U);

    // A failed context leaves the converter usable.
    EXPECT_EQ(firstMismatch(converter.convert(*frame), RAMP, 3), "");
    EXPECT_THROW(converter.convertInto(*empty, packed), motrack::VideoError);
    EXPECT_EQ(firstMismatch(converter.convert(*frame), RAMP, 3), "");
    EXPECT_EQ(converter.contextsCreated(), 1U);
}

// swscale rewrites the format option of a context for a JPEG-range format and the flags of one for unsubsampled
// input, so sws_getCachedContext never matched such a context and built a new one for every frame (#261).
TEST(RgbaFrameConverter, ReusesOneContextWhileTheFrameFormatIsUnchanged)
{
    const SwscaleWarnings warnings;
    for (const auto pixels : {AV_PIX_FMT_YUVJ420P,
                              AV_PIX_FMT_YUVJ422P,
                              AV_PIX_FMT_YUVJ444P,
                              AV_PIX_FMT_YUV444P,
                              AV_PIX_FMT_YUV420P,
                              AV_PIX_FMT_NV12,
                              AV_PIX_FMT_GRAY8,
                              AV_PIX_FMT_RGB24,
                              AV_PIX_FMT_GBRP})
    {
        for (const auto width : {1080, 635})
        {
            SCOPED_TRACE(std::string(av_get_pix_fmt_name(pixels)) + " " + std::to_string(width));
            motrack::detail::RgbaFrameConverter converter;
            const auto frame = syntheticFrame(pixels, {width, 2}, RAMP);
            for (int number = 0; number < 4; ++number)
            {
                static_cast<void>(converter.convert(*frame));
            }
            EXPECT_EQ(converter.contextsCreated(), 1U);
        }
    }
    // FFmpeg asks callers to give the range of a deprecated JPEG-range format; it warned for every context.
    EXPECT_EQ(SwscaleWarnings::logged(), std::vector<std::string>{});
}

// Gray frames stay full range, as in swscale's frame API.
TEST(RgbaFrameConverter, ConvertsGrayFramesAsFullRange)
{
    constexpr Pattern FULL_GRAY{.fullRange = true, .u = 128, .v = 128};
    motrack::detail::RgbaFrameConverter converter;
    for (const auto width : {1080, 635})
    {
        const auto frame = syntheticFrame(AV_PIX_FMT_GRAY8, {width, 3}, FULL_GRAY);
        EXPECT_EQ(firstMismatch(converter.convert(*frame), FULL_GRAY, 3), "") << width;
    }
}

struct Step
{
    AVPixelFormat pixels;
    QSize size;
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace space = AVCOL_SPC_UNSPECIFIED;
    Pattern pattern;
    bool reusesContext = false; ///< The frame has the swscale inputs of the step before it.
};

// Converts each step's frame in turn through one converter and checks it against the step's own pattern.
void expectEachFrameInItsOwnColours(std::span<const Step> steps)
{
    motrack::detail::RgbaFrameConverter converter;
    std::uint64_t contexts = 0;
    for (const auto& step : steps)
    {
        SCOPED_TRACE(std::string(av_get_pix_fmt_name(step.pixels)) + " " + std::to_string(step.size.width()) + "x" +
                     std::to_string(step.size.height()) + " " + av_color_range_name(step.range) + " " + av_color_space_name(step.space));
        const auto frame = taggedFrame(step.pixels, step.size, step.pattern, step.range, step.space);
        EXPECT_EQ(firstMismatch(converter.convert(*frame), step.pattern, 3), "");
        contexts += step.reusesContext ? 0 : 1;
        EXPECT_EQ(converter.contextsCreated(), contexts);
    }
}

// Each limited-range frame here follows a full-range one. A context that sws_getCachedContext rebuilt for a
// new format copied the full range swscale set for a JPEG-range format, and a yuv420p frame of the same size
// matched the context's rewritten format; neither saw the range in a frame's metadata (#261).
constexpr std::array RANGE_STEPS{
    Step{.pixels = AV_PIX_FMT_YUVJ420P, .size = {1080, 2}, .pattern = FULL_RAMP},
    Step{.pixels = AV_PIX_FMT_YUV420P, .size = {1080, 2}, .pattern = RAMP},
    Step{.pixels = AV_PIX_FMT_YUVJ422P, .size = {634, 3}, .pattern = FULL_RAMP},
    Step{.pixels = AV_PIX_FMT_YUV444P, .size = {31, 2}, .pattern = RAMP},
    Step{.pixels = AV_PIX_FMT_YUVJ444P, .size = {31, 2}, .pattern = FULL_RAMP},
    Step{.pixels = AV_PIX_FMT_YUV422P, .size = {1080, 2}, .pattern = RAMP},
    Step{.pixels = AV_PIX_FMT_YUV420P, .size = {1080, 2}, .range = AVCOL_RANGE_JPEG, .pattern = FULL_RAMP},
    Step{.pixels = AV_PIX_FMT_YUV420P, .size = {1080, 2}, .range = AVCOL_RANGE_MPEG, .pattern = RAMP},
    Step{.pixels = AV_PIX_FMT_YUV420P10, .size = {426, 2}, .range = AVCOL_RANGE_JPEG, .pattern = FULL_RAMP},
    Step{.pixels = AV_PIX_FMT_NV12, .size = {540, 3}, .range = AVCOL_RANGE_JPEG, .pattern = FULL_RAMP},
    Step{.pixels = AV_PIX_FMT_NV12, .size = {540, 3}, .pattern = RAMP},
};

TEST(RgbaFrameConverter, ConvertsEachFrameWithItsOwnRange)
{
    expectEachFrameInItsOwnColours(RANGE_STEPS);
}

TEST(RgbaFrameConverter, ConvertsEachFrameWithItsOwnRangeWithoutSimd)
{
    const WithoutSimd noSimd;
    expectEachFrameInItsOwnColours(RANGE_STEPS);
}

// Strong chroma on mid-gray luma, where BT.601 and BT.709 differ by 12 levels of red and 11 of green.
constexpr Pattern BT601_LIMITED{.ramp = false, .u = 90, .v = 190};
constexpr Pattern BT709_LIMITED{.ramp = false, .u = 90, .v = 190, .matrix = BT709};
constexpr Pattern BT709_FULL{.ramp = false, .fullRange = true, .u = 90, .v = 190, .matrix = BT709};
constexpr Pattern BT2020_LIMITED{.ramp = false, .u = 90, .v = 190, .matrix = BT2020};

constexpr std::array MATRIX_STEPS{
    Step{.pixels = AV_PIX_FMT_YUV420P, .size = {1920, 2}, .range = AVCOL_RANGE_MPEG, .space = AVCOL_SPC_BT709, .pattern = BT709_LIMITED},
    Step{
        .pixels = AV_PIX_FMT_YUV420P, .size = {1920, 2}, .range = AVCOL_RANGE_MPEG, .space = AVCOL_SPC_SMPTE170M, .pattern = BT601_LIMITED},
    // swscale's default matrix is BT.601.
    Step{.pixels = AV_PIX_FMT_YUV420P, .size = {1920, 2}, .range = AVCOL_RANGE_MPEG, .pattern = BT601_LIMITED, .reusesContext = true},
    Step{.pixels = AV_PIX_FMT_YUV420P, .size = {1920, 2}, .range = AVCOL_RANGE_JPEG, .space = AVCOL_SPC_BT709, .pattern = BT709_FULL},
    // A JPEG-range format has the swscale inputs of its limited-range format with a full range.
    Step{.pixels = AV_PIX_FMT_YUVJ420P, .size = {1920, 2}, .space = AVCOL_SPC_BT709, .pattern = BT709_FULL, .reusesContext = true},
    Step{.pixels = AV_PIX_FMT_YUV420P,
         .size = {1920, 2},
         .range = AVCOL_RANGE_MPEG,
         .space = AVCOL_SPC_BT2020_NCL,
         .pattern = BT2020_LIMITED},
    Step{.pixels = AV_PIX_FMT_YUV422P10, .size = {1365, 3}, .range = AVCOL_RANGE_MPEG, .space = AVCOL_SPC_BT709, .pattern = BT709_LIMITED},
    Step{
        .pixels = AV_PIX_FMT_YUV422P10, .size = {1365, 3}, .range = AVCOL_RANGE_MPEG, .space = AVCOL_SPC_BT470BG, .pattern = BT601_LIMITED},
    // swscale has no YCgCo coefficients and uses BT.601's.
    Step{.pixels = AV_PIX_FMT_YUV422P10,
         .size = {1365, 3},
         .range = AVCOL_RANGE_MPEG,
         .space = AVCOL_SPC_YCGCO,
         .pattern = BT601_LIMITED,
         .reusesContext = true},
};

TEST(RgbaFrameConverter, ConvertsEachFrameWithItsOwnMatrix)
{
    // The expectations of different matrices are further apart than the tolerance.
    const auto distance = [](const Pattern& first, const Pattern& second)
    {
        const auto a = first.rgba(0, 0);
        const auto b = second.rgba(0, 0);
        return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
    };
    EXPECT_GE(distance(BT601_LIMITED, BT709_LIMITED), 11);
    EXPECT_GE(distance(BT601_LIMITED, BT2020_LIMITED), 5);
    EXPECT_GE(distance(BT709_LIMITED, BT2020_LIMITED), 5);
    expectEachFrameInItsOwnColours(MATRIX_STEPS);
}

TEST(RgbaFrameConverter, ConvertsEachFrameWithItsOwnMatrixWithoutSimd)
{
    const WithoutSimd noSimd;
    expectEachFrameInItsOwnColours(MATRIX_STEPS);
}

// Converts every format and size into canary-filled memory, then checks each pixel and the canary after the layout.
void expectConversionsInsideThePaddedLayout()
{
    constexpr std::uint8_t CANARY = 0xa5;
    constexpr std::size_t GUARD_BYTES = 256;
    motrack::detail::RgbaFrameConverter converter;
    for (const auto& format : FORMATS)
    {
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

TEST(RgbaFrameConverter, KeepsSwscaleWritesInsideThePaddedLayout)
{
    expectConversionsInsideThePaddedLayout();
}

// swscale's C converters, which AArch64 uses for widths that are not multiples of 16, must also fill every column.
TEST(RgbaFrameConverter, ConvertsEveryColumnWithoutSimd)
{
    const WithoutSimd noSimd;
    EXPECT_EQ(av_get_cpu_flags(), 0);
    expectConversionsInsideThePaddedLayout();
}

TEST(RgbaFrameConverter, MatchesThePreviousUnpaddedConversionAtAlignedWidths)
{
    motrack::detail::RgbaFrameConverter converter;
    for (const auto& format : FORMATS)
    {
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
    bool unchanged = false;
    // The worker takes the last reference, so the converted buffer is freed on that thread.
    std::thread(
        [owned = std::move(image), &expected, &seen, &unchanged]() mutable
        {
            auto copy = owned;
            copy.setPixelColor(0, 0, QColor(Qt::red)); // Detaches the copy from the shared buffer.
            seen = firstMismatch(owned, RAMP, 3);
            unchanged = owned == expected;
            copy = QImage();
            owned = QImage();
        })
        .join();
    EXPECT_EQ(seen, "");
    EXPECT_TRUE(unchanged);
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

// The Motion JPEG decoder returns yuvj420p frames; swscale warned about the deprecated format for each one (#261).
TEST(VideoReader, DecodesJpegRangeMoviesInFullRange)
{
    constexpr Pattern FULL_FLAT{.ramp = false, .fullRange = true};
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("mjpeg-640x16.avi");
    writeMovie(path, AV_CODEC_ID_MJPEG, {640, 16}, FULL_FLAT, 3);
    const SwscaleWarnings warnings;
    motrack::VideoReader reader(path, motrack::VideoOrientation::Encoded);
    int frames = 0;
    while (const auto frame = reader.readNext())
    {
        ++frames;
        ASSERT_EQ(frame->image.size(), QSize(640, 16));
        EXPECT_EQ(firstMismatch(frame->image, FULL_FLAT, 4), "") << frames;
    }
    EXPECT_EQ(frames, 3);
    EXPECT_EQ(SwscaleWarnings::logged(), std::vector<std::string>{});
}

} // namespace
