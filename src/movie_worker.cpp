#include "movie_worker.hpp"

#include <algorithm>
#include <utility>

namespace motrack
{
namespace
{

bool cancelled(const MovieFrameRequest& request)
{
    return !request.token.cancelled || request.token.cancelled->load();
}

} // namespace

MovieWorker::MovieWorker(qsizetype cacheBytes) : m_Cache(std::clamp(cacheBytes, qsizetype{0}, qsizetype{32} * 1024 * 1024))
{}

void MovieWorker::clearCache(QUuid document)
{
    for (const auto& key : m_Cache.keys())
        if (key.document == document)
            m_Cache.remove(key);
}

void MovieWorker::clear(QUuid document)
{
    clearCache(document);
    std::erase_if(m_Decoders, [document](const auto& decoder) { return decoder.document == document; });
}

void MovieWorker::clearAll()
{
    m_Cache.clear();
    m_Decoders.clear();
}

qsizetype MovieWorker::cacheBytes() const
{
    return m_Cache.totalCost();
}

std::size_t MovieWorker::decoderCount() const
{
    return static_cast<std::size_t>(std::ranges::count_if(m_Decoders, [](const auto& decoder) { return decoder.reader != nullptr; }));
}

MovieWorker::Decoder& MovieWorker::decoder(const MovieFrameRequest& request)
{
    auto found = std::ranges::find(m_Decoders, request.token.document, &Decoder::document);
    if (found != m_Decoders.end())
    {
        if (found->media == request.media && found->path == request.path && found->orientation == request.orientation &&
            found->archive == request.archive)
        {
            m_Decoders.splice(m_Decoders.begin(), m_Decoders, found);
            return m_Decoders.front();
        }
        clear(request.token.document);
    }
    constexpr std::size_t MAX_DECODERS = 2;
    if (m_Decoders.size() == MAX_DECODERS)
        clear(m_Decoders.back().document);
    m_Decoders.push_front({.document = request.token.document,
                           .media = request.media,
                           .path = request.path,
                           .orientation = request.orientation,
                           .archive = request.archive,
                           .reader = {},
                           .nextFrame = 0});
    return m_Decoders.front();
}

MovieFrameResult MovieWorker::readFrame(MovieFrameRequest request)
{
    QImage image;
    if (!cancelled(request) && request.token.kind == WorkspaceJob::Frame && !request.token.document.isNull() && !request.media.isNull() &&
        request.frame >= 0 && !request.path.isEmpty())
    {
        auto& current = decoder(request); // Validate path/orientation/source identity before considering a cached frame.
        const FrameKey key{.document = request.token.document,
                           .media = request.media,
                           .frame = request.frame,
                           .hasTimestamp = request.timestamp.has_value(),
                           .timestamp = request.timestamp ? request.timestamp->count() : 0};
        if (const auto* cached = m_Cache.object(key))
            image = *cached;
        else
        {
            image = decodeFrame(current, request);
            if (!image.isNull() && !cancelled(request) && image.sizeInBytes() <= m_Cache.maxCost())
                m_Cache.insert(key, new QImage(image), image.sizeInBytes());
        }
    }
    if (cancelled(request))
        image = {};
    return {.request = std::move(request), .image = std::move(image)};
}

QImage MovieWorker::decodeFrame(Decoder& decoder, const MovieFrameRequest& request)
{
    try
    {
        constexpr int SEQUENTIAL_WINDOW = 32;
        if (request.timestamp &&
            (std::cmp_greater(decoder.nextFrame, request.frame) || std::cmp_less(decoder.nextFrame + SEQUENTIAL_WINDOW, request.frame)))
        {
            if (!decoder.reader)
                decoder.reader = std::make_unique<VideoReader>(request.path, request.orientation);
            auto image = seekFrame(decoder, request, *request.timestamp);
            if (!image.isNull())
            {
                decoder.nextFrame = static_cast<std::uint64_t>(request.frame) + 1;
                return image;
            }
            decoder.reader.reset();
            if (cancelled(request))
                return {};
        }
        if (!decoder.reader || std::cmp_greater(decoder.nextFrame, request.frame))
        {
            decoder.reader.reset();
            decoder.reader = std::make_unique<VideoReader>(request.path, request.orientation);
            decoder.nextFrame = 0;
        }
        while (!cancelled(request))
        {
            const bool atTarget = std::cmp_equal(decoder.nextFrame, request.frame);
            const auto decoded = atTarget ? decoder.reader->readNext() : decoder.reader->skipNext();
            if (!decoded)
                return {};
            ++decoder.nextFrame;
            if (atTarget)
                return decoded->image;
        }
    }
    catch (const VideoError&)
    {
        decoder.reader.reset();
        decoder.nextFrame = 0;
    }
    return {};
}

QImage MovieWorker::seekFrame(Decoder& decoder, const MovieFrameRequest& request, std::chrono::nanoseconds timestamp)
{
    try
    {
        if (decoder.reader->seekTo(timestamp))
            while (!cancelled(request))
            {
                const auto decoded = decoder.reader->readNext();
                if (!decoded || !decoded->presentationTime || *decoded->presentationTime > timestamp)
                    break;
                if (*decoded->presentationTime == timestamp)
                    return decoded->image;
            }
    }
    catch (const VideoError&)
    {
        // Some demuxers can seek successfully without yielding a usable frame.
        return {};
    }
    return {};
}

} // namespace motrack
