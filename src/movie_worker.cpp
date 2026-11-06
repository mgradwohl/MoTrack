#include "movie_worker.hpp"

#include <utility>

namespace motrack
{

MovieWorker::MovieWorker(const std::atomic<quint64>& current) : m_CurrentRequest(current)
{}

void MovieWorker::clear()
{
    m_Cache.clear();
    m_Reader.reset();
}

void MovieWorker::readFrame(
    const QString& path, int frame, quint64 request, bool useDisplayOrientation, bool hasTimestamp, qint64 timestamp)
{
    if (m_CurrentRequest == request)
    {
        const auto orientation = useDisplayOrientation ? VideoOrientation::Display : VideoOrientation::Encoded;
        const auto seekTime = hasTimestamp ? std::optional{std::chrono::nanoseconds{timestamp}} : std::nullopt;
        Q_EMIT frameDecoded(frame, request, decodeFrame(path, frame, request, orientation, seekTime));
    }
}

QImage MovieWorker::decodeFrame(
    const QString& path, int frame, quint64 request, VideoOrientation orientation, std::optional<std::chrono::nanoseconds> timestamp)
{
    if (const auto* cached = m_Cache.object(frame))
    {
        return *cached;
    }
    try
    {
        constexpr int SEQUENTIAL_WINDOW = 32;
        if (timestamp && (std::cmp_greater(m_NextFrame, frame) || std::cmp_less(m_NextFrame + SEQUENTIAL_WINDOW, frame)))
        {
            if (!m_Reader)
            {
                m_Reader = std::make_unique<VideoReader>(path, orientation);
            }
            auto image = seekFrame(*timestamp, request);
            if (!image.isNull())
            {
                m_NextFrame = static_cast<std::uint64_t>(frame) + 1;
                cacheFrame(frame, image);
                return image;
            }
            m_Reader.reset();
            if (m_CurrentRequest != request)
            {
                return {};
            }
        }
        if (!m_Reader || std::cmp_greater(m_NextFrame, frame))
        {
            m_Reader.reset();
            m_Reader = std::make_unique<VideoReader>(path, orientation);
            m_NextFrame = 0;
        }
        while (m_CurrentRequest == request)
        {
            const bool atTarget = std::cmp_equal(m_NextFrame, frame);
            const auto decoded = atTarget ? m_Reader->readNext() : m_Reader->skipNext();
            if (!decoded)
            {
                return {};
            }
            ++m_NextFrame;
            if (atTarget)
            {
                cacheFrame(frame, decoded->image);
                return decoded->image;
            }
        }
    }
    catch (const VideoError&)
    {
        m_Reader.reset();
    }
    return {};
}

QImage MovieWorker::seekFrame(std::chrono::nanoseconds timestamp, quint64 request)
{
    try
    {
        if (m_Reader->seekTo(timestamp))
        {
            while (m_CurrentRequest == request)
            {
                const auto decoded = m_Reader->readNext();
                if (!decoded || !decoded->presentationTime || *decoded->presentationTime > timestamp)
                {
                    break;
                }
                if (*decoded->presentationTime == timestamp)
                {
                    return decoded->image;
                }
            }
        }
    }
    catch (const VideoError&)
    {
        // Some demuxers can seek successfully without yielding a usable frame.
        return {};
    }
    return {};
}

void MovieWorker::cacheFrame(int frame, const QImage& image)
{
    const auto bytes = image.sizeInBytes();
    if (bytes <= m_Cache.maxCost())
    {
        m_Cache.insert(frame, new QImage(image), bytes);
    }
}

} // namespace motrack
