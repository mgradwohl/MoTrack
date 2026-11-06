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

void MovieWorker::readFrame(const QString& path, int frame, quint64 request, bool useDisplayOrientation)
{
    if (m_CurrentRequest == request)
    {
        const auto orientation = useDisplayOrientation ? VideoOrientation::Display : VideoOrientation::Encoded;
        Q_EMIT frameDecoded(frame, request, decodeFrame(path, frame, request, orientation));
    }
}

QImage MovieWorker::decodeFrame(const QString& path, int frame, quint64 request, VideoOrientation orientation)
{
    if (const auto* cached = m_Cache.object(frame))
    {
        return *cached;
    }
    try
    {
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
                const auto bytes = decoded->image.sizeInBytes();
                if (bytes <= m_Cache.maxCost())
                {
                    m_Cache.insert(frame, new QImage(decoded->image), bytes);
                }
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

} // namespace motrack
