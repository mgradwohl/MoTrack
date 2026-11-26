#include "capture_encoder.hpp"

#include "video_reader.hpp"

#include <QFile>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace motrack
{
namespace
{

constexpr int IO_BUFFER_SIZE = 64 * 1024;
constexpr std::int64_t MAX_PIXELS = std::int64_t{32} * 1024 * 1024;
// Motion JPEG quantizer (2 is the finest): close to the source detail at a few MB/s for 720p30.
constexpr int JPEG_QUANTIZER = 4;
constexpr AVRational TRACK_TIME_BASE{.num = 1, .den = CaptureEncoder::TIMESCALE};
constexpr AVRational MICROSECONDS{.num = 1, .den = 1'000'000};
// The last frame's duration when only one frame exists: one 30 Hz interval.
constexpr std::int64_t DEFAULT_FRAME_TICKS = CaptureEncoder::TIMESCALE / 30;

void requireSuccess(int result, std::string_view operation)
{
    if (result < 0)
    {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
        av_strerror(result, message.data(), message.size());
        throw VideoError(std::string(operation) + ": " + message.data());
    }
}

struct FreeFormat
{
    void operator()(AVFormatContext* context) const
    {
        avformat_free_context(context);
    }
};

struct CloseIo
{
    void operator()(AVIOContext* context) const
    {
        av_free(context->buffer);
        context->buffer = nullptr;
        avio_context_free(&context);
    }
};

struct FreeCodec
{
    void operator()(AVCodecContext* context) const
    {
        avcodec_free_context(&context);
    }
};

struct FreeFrame
{
    void operator()(AVFrame* frame) const
    {
        av_frame_free(&frame);
    }
};

struct FreePacket
{
    void operator()(AVPacket* packet) const
    {
        av_packet_free(&packet);
    }
};

using Scaler = std::unique_ptr<SwsContext, decltype(&sws_freeContext)>;

#if LIBAVFORMAT_VERSION_MAJOR >= 61
int writeBytes(void* opaque, const std::uint8_t* buffer, int size)
#else
int writeBytes(void* opaque, std::uint8_t* buffer, int size)
#endif
{
    auto& file = *static_cast<QFile*>(opaque);
    return file.write(reinterpret_cast<const char*>(buffer), size) == size ? size : AVERROR(EIO);
}

std::int64_t seekBytes(void* opaque, std::int64_t offset, int whence)
{
    auto& file = *static_cast<QFile*>(opaque);
    const auto mode = whence & ~AVSEEK_FORCE;
    if (mode == AVSEEK_SIZE)
        return file.size();
    // AVIO translates relative seeks into absolute positions.
    if (mode != SEEK_SET || offset < 0)
        return AVERROR(EINVAL);
    return file.seek(offset) ? offset : AVERROR(EIO);
}

struct Planes
{
    AVPixelFormat format = AV_PIX_FMT_NONE;
    std::array<const std::uint8_t*, 4> data{};
    std::array<int, 4> strides{};
    bool yuv = false;
};

Planes planes(const CaptureImage& frame)
{
    if (frame.width <= 0 || frame.height <= 0 || static_cast<std::int64_t>(frame.width) * frame.height > MAX_PIXELS)
        throw VideoError("The camera frame dimensions are unsupported.");
    Planes result;
    int count = 1;
    switch (frame.format)
    {
    case CapturePixelFormat::Bgra:
        result.format = AV_PIX_FMT_BGRA;
        break;
    case CapturePixelFormat::Bgrx:
        result.format = AV_PIX_FMT_BGR0;
        break;
    case CapturePixelFormat::Rgba:
        result.format = AV_PIX_FMT_RGBA;
        break;
    case CapturePixelFormat::Rgbx:
        result.format = AV_PIX_FMT_RGB0;
        break;
    case CapturePixelFormat::Argb:
        result.format = AV_PIX_FMT_ARGB;
        break;
    case CapturePixelFormat::Xrgb:
        result.format = AV_PIX_FMT_0RGB;
        break;
    case CapturePixelFormat::Abgr:
        result.format = AV_PIX_FMT_ABGR;
        break;
    case CapturePixelFormat::Xbgr:
        result.format = AV_PIX_FMT_0BGR;
        break;
    case CapturePixelFormat::Yuv420p:
    case CapturePixelFormat::Yv12:
        result.format = AV_PIX_FMT_YUV420P;
        count = 3;
        break;
    case CapturePixelFormat::Yuv422p:
        result.format = AV_PIX_FMT_YUV422P;
        count = 3;
        break;
    case CapturePixelFormat::Nv12:
        result.format = AV_PIX_FMT_NV12;
        count = 2;
        break;
    case CapturePixelFormat::Nv21:
        result.format = AV_PIX_FMT_NV21;
        count = 2;
        break;
    case CapturePixelFormat::Yuyv:
        result.format = AV_PIX_FMT_YUYV422;
        break;
    case CapturePixelFormat::Uyvy:
        result.format = AV_PIX_FMT_UYVY422;
        break;
    case CapturePixelFormat::Gray8:
        result.format = AV_PIX_FMT_GRAY8;
        break;
    }
    result.yuv = count > 1 || frame.format == CapturePixelFormat::Yuyv || frame.format == CapturePixelFormat::Uyvy;
    for (int plane = 0; plane < count; ++plane)
    {
        const auto index = static_cast<std::size_t>(plane);
        if (frame.planes.at(index) == nullptr || frame.strides.at(index) <= 0)
            throw VideoError("The camera frame is missing an image plane.");
        result.data.at(index) = frame.planes.at(index);
        result.strides.at(index) = frame.strides.at(index);
    }
    if (frame.format == CapturePixelFormat::Yv12)
    {
        std::swap(result.data[1], result.data[2]);
        std::swap(result.strides[1], result.strides[2]);
    }
    return result;
}

/// Everything a conversion context depends on. RGB and gray sources are full range; YUV sources say which range and
/// matrix they use. RGB targets are full range; YUV targets are JPEG's full-range BT.601.
struct ScaleKey
{
    int sourceWidth = 0;
    int sourceHeight = 0;
    AVPixelFormat sourceFormat = AV_PIX_FMT_NONE;
    bool sourceFullRange = true;
    bool bt709 = false;
    int width = 0;
    int height = 0;
    AVPixelFormat format = AV_PIX_FMT_NONE;
    int flags = 0;

    bool operator==(const ScaleKey&) const = default;
};

ScaleKey scaleKey(const CaptureImage& frame, const Planes& source, int width, int height, AVPixelFormat format, int flags)
{
    return {.sourceWidth = frame.width,
            .sourceHeight = frame.height,
            .sourceFormat = source.format,
            .sourceFullRange = !source.yuv || frame.fullRange,
            .bt709 = frame.bt709,
            .width = width,
            .height = height,
            .format = format,
            .flags = flags};
}

/// One swscale context, rebuilt only when its key changes.
/// - sws_getCachedContext compares the options of the context it is given, and swscale rewrites some of them when it
///   initializes a context (a padded RGB layout becomes the alpha layout, flags are added for some widths), so the
///   comparison failed and a new context was built for every frame (#369).
/// - The ranges are set before initialization. Set afterwards, they do not replace an unscaled copy or repack
///   converter that swscale chose for equal ranges, so a video-range camera frame was recorded unexpanded in the
///   full-range movie: black at 16 and white at 235 (#373).
class FrameScaler
{
  public:
    SwsContext* context(const ScaleKey& key, const char* failure)
    {
        if (m_Scale && key == m_Key)
        {
            return m_Scale.get();
        }
        Scaler scale{sws_alloc_context(), &sws_freeContext};
        if (!scale)
        {
            throw VideoError(failure);
        }
        scale->src_w = key.sourceWidth;
        scale->src_h = key.sourceHeight;
        scale->src_format = key.sourceFormat;
        scale->src_range = static_cast<int>(key.sourceFullRange);
        scale->dst_w = key.width;
        scale->dst_h = key.height;
        scale->dst_format = key.format;
        scale->dst_range = 1;
        scale->flags = static_cast<unsigned>(key.flags);
        requireSuccess(sws_init_context(scale.get(), nullptr, nullptr), failure);
        // Unsupported for some format pairs (it then keeps swscale's defaults); not an error.
        (void)sws_setColorspaceDetails(scale.get(),
                                       sws_getCoefficients(key.bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601),
                                       static_cast<int>(key.sourceFullRange),
                                       sws_getCoefficients(SWS_CS_ITU601),
                                       1,
                                       0,
                                       1 << 16,
                                       1 << 16);
        m_Scale = std::move(scale);
        m_Key = key;
        ++m_Created;
        return m_Scale.get();
    }

    [[nodiscard]] int created() const noexcept
    {
        return m_Created;
    }

  private:
    Scaler m_Scale{nullptr, &sws_freeContext};
    ScaleKey m_Key;
    int m_Created = 0;
};

} // namespace

CaptureImage captureImageFromQImage(const QImage& image)
{
    CaptureImage frame{.width = image.width(), .height = image.height()};
    switch (image.format())
    {
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    case QImage::Format_RGB32:
        frame.format = CapturePixelFormat::Bgrx;
        break;
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
        frame.format = CapturePixelFormat::Bgra;
        break;
#else
    case QImage::Format_RGB32:
        frame.format = CapturePixelFormat::Xrgb;
        break;
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
        frame.format = CapturePixelFormat::Argb;
        break;
#endif
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
        frame.format = CapturePixelFormat::Rgba;
        break;
    case QImage::Format_RGBX8888:
        frame.format = CapturePixelFormat::Rgbx;
        break;
    case QImage::Format_Grayscale8:
        frame.format = CapturePixelFormat::Gray8;
        break;
    default:
        return {};
    }
    frame.planes[0] = image.constBits();
    frame.strides[0] = static_cast<int>(image.bytesPerLine());
    return frame;
}

struct CaptureConverter::Impl
{
    FrameScaler scale;
};

CaptureConverter::CaptureConverter() : m_Impl(std::make_unique<Impl>())
{}

CaptureConverter::~CaptureConverter() = default;

QImage CaptureConverter::toImage(const CaptureImage& frame, QSize bound)
{
    const auto source = planes(frame);
    auto size = QSize(frame.width, frame.height);
    if (bound.isValid() && (size.width() > bound.width() || size.height() > bound.height()))
        size.scale(bound, Qt::KeepAspectRatio);
    size = size.expandedTo({1, 1});
    QImage image(size, QImage::Format_RGBA8888);
    if (image.isNull())
        throw VideoError("Could not allocate the camera preview.");
    auto* scale = m_Impl->scale.context(scaleKey(frame, source, size.width(), size.height(), AV_PIX_FMT_RGBA, SWS_BILINEAR),
                                        "Could not allocate the camera preview.");
    const std::array<std::uint8_t*, 4> destination{image.bits(), nullptr, nullptr, nullptr};
    const std::array<int, 4> strides{static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    requireSuccess(sws_scale(scale, source.data.data(), source.strides.data(), 0, frame.height, destination.data(), strides.data()),
                   "Could not convert the camera frame");
    return image;
}

int CaptureConverter::contextsCreated() const noexcept
{
    return m_Impl->scale.created();
}

struct CaptureEncoder::Impl
{
    explicit Impl(const QString& path) : file(path)
    {
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            throw VideoError("Could not create the recording file.");
        AVFormatContext* output = nullptr;
        requireSuccess(avformat_alloc_output_context2(&output, nullptr, "mov", nullptr), "Could not prepare the QuickTime recording");
        format.reset(output);
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(IO_BUFFER_SIZE));
        if (buffer == nullptr)
            throw VideoError("Could not allocate the recording buffer.");
        io.reset(avio_alloc_context(buffer, IO_BUFFER_SIZE, 1, &file, nullptr, writeBytes, seekBytes));
        if (!io)
        {
            av_free(buffer);
            throw VideoError("Could not allocate the recording output.");
        }
        format->pb = io.get();
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (!packet || !picture)
            throw VideoError("Could not allocate the recording state.");
    }

    void open(int width, int height)
    {
        const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (encoder == nullptr)
            throw VideoError("This FFmpeg build has no Motion JPEG encoder.");
        stream = avformat_new_stream(format.get(), nullptr);
        codec.reset(avcodec_alloc_context3(encoder));
        if (stream == nullptr || !codec)
            throw VideoError("Could not allocate the recording stream.");
        codec->width = width;
        codec->height = height;
        // Full-range 4:2:0 is JPEG's own sampling; MJPEG accepts YUV420P with JPEG range on FFmpeg 6.1+.
        codec->pix_fmt = AV_PIX_FMT_YUV420P;
        codec->color_range = AVCOL_RANGE_JPEG;
        codec->time_base = TRACK_TIME_BASE;
        codec->flags |= AV_CODEC_FLAG_QSCALE;
        codec->global_quality = FF_QP2LAMBDA * JPEG_QUANTIZER;
        codec->thread_count = 0;
        if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0)
            codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        requireSuccess(avcodec_open2(codec.get(), encoder, nullptr), "Could not open the Motion JPEG encoder");
        requireSuccess(avcodec_parameters_from_context(stream->codecpar, codec.get()), "Could not describe the recording stream");
        stream->time_base = TRACK_TIME_BASE;
        picture->format = codec->pix_fmt;
        picture->width = width;
        picture->height = height;
        picture->color_range = AVCOL_RANGE_JPEG;
        requireSuccess(av_frame_get_buffer(picture.get(), 0), "Could not allocate the recording frame");
        requireSuccess(avformat_write_header(format.get(), nullptr), "Could not start the QuickTime recording");
        headerWritten = true;
    }

    void drain()
    {
        for (;;)
        {
            const auto received = avcodec_receive_packet(codec.get(), packet.get());
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
                return;
            requireSuccess(received, "Could not encode the camera frame");
            // The interval since the previous frame stands in for the duration of this one.
            const auto pts = packet->pts;
            packet->duration = lastPacketPts ? pts - *lastPacketPts : DEFAULT_FRAME_TICKS;
            lastPacketPts = pts;
            av_packet_rescale_ts(packet.get(), codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            requireSuccess(av_interleaved_write_frame(format.get(), packet.get()), "Could not write the recording");
        }
    }

    QFile file;
    std::unique_ptr<AVFormatContext, FreeFormat> format;
    std::unique_ptr<AVIOContext, CloseIo> io;
    std::unique_ptr<AVCodecContext, FreeCodec> codec;
    std::unique_ptr<AVFrame, FreeFrame> picture{av_frame_alloc()};
    std::unique_ptr<AVPacket, FreePacket> packet{av_packet_alloc()};
    FrameScaler scale;
    AVStream* stream = nullptr;
    std::optional<std::chrono::microseconds> first;
    std::chrono::microseconds last{0};
    std::optional<std::int64_t> lastPts;
    std::optional<std::int64_t> lastPacketPts;
    std::int64_t frames = 0;
    bool headerWritten = false;
    bool finished = false;
};

CaptureEncoder::CaptureEncoder(const QString& path) : m_Impl(std::make_unique<Impl>(path))
{}

CaptureEncoder::~CaptureEncoder() = default;

bool CaptureEncoder::write(const CaptureImage& frame, std::chrono::microseconds time)
{
    auto& state = *m_Impl;
    if (state.finished)
        throw VideoError("The recording is already finished.");
    const auto source = planes(frame);
    const auto relative = state.first ? time - *state.first : std::chrono::microseconds{0};
    const auto pts = av_rescale_q_rnd(relative.count(), MICROSECONDS, TRACK_TIME_BASE, AV_ROUND_NEAR_INF);
    if (state.lastPts && pts <= *state.lastPts)
        return false;
    if (!state.headerWritten)
        state.open(frame.width, frame.height);
    else if (frame.width != state.codec->width || frame.height != state.codec->height)
        throw VideoError("The camera frame size changed during the recording.");
    auto* scale = state.scale.context(scaleKey(frame, source, state.codec->width, state.codec->height, state.codec->pix_fmt, SWS_BICUBIC),
                                      "Could not convert the camera frame.");
    requireSuccess(av_frame_make_writable(state.picture.get()), "Could not reuse the recording frame");
    requireSuccess(
        sws_scale(scale, source.data.data(), source.strides.data(), 0, frame.height, state.picture->data, state.picture->linesize),
        "Could not convert the camera frame");
    state.picture->pts = pts;
    state.picture->quality = state.codec->global_quality;
    requireSuccess(avcodec_send_frame(state.codec.get(), state.picture.get()), "Could not encode the camera frame");
    state.drain();
    if (!state.first)
        state.first = time;
    state.last = time;
    state.lastPts = pts;
    ++state.frames;
    return true;
}

CaptureSummary CaptureEncoder::finish()
{
    auto& state = *m_Impl;
    if (state.finished)
        throw VideoError("The recording is already finished.");
    state.finished = true;
    if (state.headerWritten)
    {
        requireSuccess(avcodec_send_frame(state.codec.get(), nullptr), "Could not flush the Motion JPEG encoder");
        state.drain();
        requireSuccess(av_write_trailer(state.format.get()), "Could not finish the QuickTime recording");
    }
    avio_flush(state.io.get());
    if (state.io->error < 0 || !state.file.flush())
        throw VideoError("Could not write the recording file.");
    state.file.close();
    return {.frames = state.frames, .duration = state.first ? state.last - *state.first : std::chrono::microseconds{0}};
}

int CaptureEncoder::contextsCreated() const noexcept
{
    return m_Impl->scale.created();
}

} // namespace motrack
