#include "video_reader.hpp"

#include <QFile>
#include <QTransform>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace motrack
{
namespace
{

constexpr int IO_BUFFER_SIZE = 32 * 1024;
constexpr std::int64_t MAX_PIXELS = std::int64_t{32} * 1024 * 1024;

std::optional<QTransform> displayOrientation(const std::uint8_t* data, std::size_t size)
{
    std::array<std::int32_t, 9> matrix{};
    if (!data || size < sizeof(matrix))
    {
        return std::nullopt;
    }
    std::memcpy(matrix.data(), data, sizeof(matrix));
    constexpr std::int32_t UNIT = 1 << 16;
    const auto a = matrix.at(0);
    const auto b = matrix.at(1);
    const auto c = matrix.at(3);
    const auto d = matrix.at(4);
    const auto unitOrZero = [](std::int32_t value)
    {
        return value == 0 || value == UNIT || value == -UNIT;
    };
    // Quarter turns and reflections keep a one-to-one mapping of tracking pixels.
    if (matrix.at(2) != 0 || matrix.at(5) != 0 || matrix.at(8) != (1 << 30) || !unitOrZero(a) || !unitOrZero(b) || !unitOrZero(c) ||
        !unitOrZero(d) || std::abs(a) + std::abs(b) != UNIT || std::abs(c) + std::abs(d) != UNIT || std::abs(a) + std::abs(c) != UNIT)
    {
        return std::nullopt;
    }
    return QTransform(
        static_cast<double>(a) / UNIT, static_cast<double>(b) / UNIT, static_cast<double>(c) / UNIT, static_cast<double>(d) / UNIT, 0, 0);
}

void requireSuccess(int result, std::string_view operation)
{
    if (result < 0)
    {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
        av_strerror(result, message.data(), message.size());
        throw VideoError(std::string(operation) + ": " + message.data());
    }
}

struct CloseIo
{
    void operator()(AVIOContext* context) const
    {
        av_free(context->buffer);
        context->buffer = nullptr;
        avio_context_free(&context);
    }
};

struct CloseFormat
{
    void operator()(AVFormatContext* context) const
    {
        avformat_close_input(&context);
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

struct FreeFrame
{
    void operator()(AVFrame* frame) const
    {
        av_frame_free(&frame);
    }
};

int readBytes(void* opaque, std::uint8_t* buffer, int size)
{
    auto& file = *static_cast<QFile*>(opaque);
    const auto read = file.read(reinterpret_cast<char*>(buffer), size);
    if (read < 0)
    {
        return AVERROR(EIO);
    }
    return read == 0 ? AVERROR_EOF : static_cast<int>(read);
}

std::int64_t seekBytes(void* opaque, std::int64_t offset, int whence)
{
    auto& file = *static_cast<QFile*>(opaque);
    const auto mode = whence & ~AVSEEK_FORCE;
    if (mode == AVSEEK_SIZE)
    {
        return file.size();
    }

    // AVIO translates seeks into absolute positions. Local QFile inputs always report their size.
    if (mode != SEEK_SET || offset < 0)
    {
        return AVERROR(EINVAL);
    }
    return file.seek(offset) ? offset : AVERROR(EIO);
}

} // namespace

struct VideoReader::Impl
{
    explicit Impl(const QString& path, VideoOrientation mode) : file(path)
    {
        if (!file.open(QIODevice::ReadOnly))
        {
            throw VideoError("Could not open the video file.");
        }

        auto* buffer = static_cast<std::uint8_t*>(av_malloc(IO_BUFFER_SIZE));
        if (buffer == nullptr)
        {
            throw VideoError("Could not allocate the video I/O buffer.");
        }
        io.reset(avio_alloc_context(buffer, IO_BUFFER_SIZE, 0, &file, readBytes, nullptr, seekBytes));
        if (!io)
        {
            av_free(buffer);
            throw VideoError("Could not allocate the video I/O context.");
        }

        format.reset(avformat_alloc_context());
        if (!format)
        {
            throw VideoError("Could not allocate the video format context.");
        }
        format->pb = io.get();
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        // Read only from the supplied QFile. Playlist and nested protocol opens are disallowed.
        requireSuccess(av_opt_set(format.get(), "protocol_whitelist", "none", 0), "Could not restrict movie input");
        requireSuccess(av_opt_set(format.get(), "format_whitelist", "mov,matroska,avi,asf,flv,mpeg,mpegts,ogg,rm,mxf,yuv4mpegpipe,wav", 0),
                       "Could not restrict movie formats");
        auto* input = format.release();
        const auto name = file.fileName().toUtf8();
        const auto result = avformat_open_input(&input, name.constData(), nullptr, nullptr);
        format.reset(input);
        requireSuccess(result, "Could not recognize the movie format");
        requireSuccess(avformat_find_stream_info(format.get(), nullptr), "Could not read the movie streams");

        const AVCodec* decoder = nullptr;
        streamIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        requireSuccess(streamIndex, "The file has no supported video stream");
        const auto* stream = format->streams[streamIndex];
        timeBase = stream->time_base;
        const auto* parameters = stream->codecpar;
        if (mode == VideoOrientation::Display)
        {
            orientation.emplace();
            if (const auto* side =
                    av_packet_side_data_get(parameters->coded_side_data, parameters->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX))
            {
                orientation = displayOrientation(side->data, side->size).value_or(QTransform{});
            }
        }
        codec.reset(avcodec_alloc_context3(decoder));
        if (!codec || !packet || !decoded)
        {
            throw VideoError("Could not allocate video decoding state.");
        }
        requireSuccess(avcodec_parameters_to_context(codec.get(), parameters), "Could not read the video codec parameters");
        codec->max_pixels = MAX_PIXELS;
        requireSuccess(avcodec_open2(codec.get(), decoder, nullptr), "Could not open the video decoder");
    }

    void sendNextPacket()
    {
        for (;;)
        {
            const auto result = av_read_frame(format.get(), packet.get());
            if (result == AVERROR_EOF)
            {
                requireSuccess(avcodec_send_packet(codec.get(), nullptr), "Could not drain the video decoder");
                draining = true;
                return;
            }
            requireSuccess(result, "Could not read the next video packet");
            const bool video = packet->stream_index == streamIndex;
            const auto sent = video ? avcodec_send_packet(codec.get(), packet.get()) : 0;
            av_packet_unref(packet.get());
            if (video)
            {
                requireSuccess(sent, "Could not submit the video packet");
                return;
            }
        }
    }

    QImage convertFrame()
    {
        const auto width = decoded->width;
        const auto height = decoded->height;
        if (width <= 0 || height <= 0 || static_cast<std::int64_t>(width) * height > MAX_PIXELS)
        {
            throw VideoError("The video frame dimensions are unsupported.");
        }
        QImage image(width, height, QImage::Format_RGBA8888);
        scale.reset(sws_getCachedContext(scale.release(),
                                         width,
                                         height,
                                         static_cast<AVPixelFormat>(decoded->format),
                                         width,
                                         height,
                                         AV_PIX_FMT_RGBA,
                                         SWS_BILINEAR,
                                         nullptr,
                                         nullptr,
                                         nullptr));
        if (image.isNull() || !scale)
        {
            throw VideoError("Could not allocate the converted video frame.");
        }
        const std::array<std::uint8_t*, 4> destination{image.bits(), nullptr, nullptr, nullptr};
        const std::array<int, 4> strides{static_cast<int>(image.bytesPerLine()), 0, 0, 0};
        requireSuccess(sws_scale(scale.get(), decoded->data, decoded->linesize, 0, height, destination.data(), strides.data()),
                       "Could not convert the video frame");

        if (!orientation)
        {
            return image;
        }
        auto transform = *orientation;
        if (const auto* side = av_frame_get_side_data(decoded.get(), AV_FRAME_DATA_DISPLAYMATRIX))
        {
            transform = displayOrientation(side->data, side->size).value_or(transform);
        }
        return transform.isIdentity() ? image : image.transformed(transform);
    }

    MovieFrame takeFrame(bool convertPixels)
    {
        std::optional<std::chrono::nanoseconds> time;
        if (decoded->best_effort_timestamp != AV_NOPTS_VALUE)
        {
            time.emplace(av_rescale_q(decoded->best_effort_timestamp, timeBase, AVRational{.num = 1, .den = 1'000'000'000}));
        }
        return {.ordinal = nextOrdinal++, .presentationTime = time, .image = convertPixels ? convertFrame() : QImage{}};
    }

    QFile file;
    std::unique_ptr<AVIOContext, CloseIo> io;
    std::unique_ptr<AVFormatContext, CloseFormat> format;
    std::unique_ptr<AVCodecContext, FreeCodec> codec;
    std::unique_ptr<AVPacket, FreePacket> packet{av_packet_alloc()};
    std::unique_ptr<AVFrame, FreeFrame> decoded{av_frame_alloc()};
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scale{nullptr, &sws_freeContext};
    AVRational timeBase{};
    std::optional<QTransform> orientation;
    int streamIndex = -1;
    std::uint64_t nextOrdinal = 0;
    bool draining = false;
};

VideoReader::VideoReader(const QString& path, VideoOrientation orientation) : m_Impl(std::make_unique<Impl>(path, orientation))
{}

VideoReader::~VideoReader() = default;

std::optional<MovieFrame> VideoReader::readNext()
{
    return decodeNext(true);
}

std::optional<MovieFrame> VideoReader::skipNext()
{
    return decodeNext(false);
}

std::optional<MovieFrame> VideoReader::decodeNext(bool convertPixels)
{
    for (;;)
    {
        const auto result = avcodec_receive_frame(m_Impl->codec.get(), m_Impl->decoded.get());
        if (result == AVERROR_EOF)
        {
            return std::nullopt;
        }
        if (result != AVERROR(EAGAIN) || m_Impl->draining)
        {
            requireSuccess(result, "Could not decode the video frame");
            return m_Impl->takeFrame(convertPixels);
        }
        m_Impl->sendNextPacket();
    }
}

MovieIndex buildMovieIndex(const QString& path, VideoOrientation orientation, const std::atomic_bool& cancelled)
{
    constexpr int MAX_INDEXED_FRAMES = 2'000'000;
    if (cancelled)
    {
        return {};
    }
    try
    {
        VideoReader reader(path, orientation);
        MovieIndex index;
        std::optional<double> start;
        bool hasTiming = true;
        while (!cancelled)
        {
            const auto frame = index.frameCount == 0 ? reader.readNext() : reader.skipNext();
            if (!frame)
            {
                return index;
            }
            if (index.frameCount == MAX_INDEXED_FRAMES)
            {
                return {};
            }
            ++index.frameCount;
            if (hasTiming)
            {
                if (frame->presentationTime)
                {
                    const auto seconds = std::chrono::duration<double>(*frame->presentationTime).count();
                    start = start.value_or(seconds);
                    const auto relative = seconds - *start;
                    hasTiming = index.frameTimes.empty() || relative > index.frameTimes.back();
                    index.frameTimes.push_back(relative);
                }
                else
                {
                    hasTiming = false;
                }
                if (!hasTiming)
                {
                    index.frameTimes.clear();
                }
            }
        }
    }
    catch (const VideoError&)
    {
        return {};
    }
    return {};
}

} // namespace motrack
