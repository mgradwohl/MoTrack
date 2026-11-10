#pragma once

#include "video_reader.hpp"
#include "workspace_session.hpp"

#include <QCache>

#include <list>

namespace motrack
{

struct MovieFrameRequest
{
    WorkspaceJobToken token;
    QUuid media;
    QString path;
    int frame = 0;
    quint64 sequence = 0;
    VideoOrientation orientation = VideoOrientation::Encoded;
    std::optional<std::chrono::nanoseconds> timestamp;
    std::shared_ptr<const TrackerArchive> archive;
};

struct MovieFrameResult
{
    MovieFrameRequest request;
    QImage image;
};

/// Confined to one background thread. Two LRU decoders share a single cache, capped at 32 MiB.
/// Requests own their cancellation flags and source payload; no GUI/session objects are borrowed.
class MovieWorker
{
  public:
    explicit MovieWorker(qsizetype cacheBytes = qsizetype{32} * 1024 * 1024);
    [[nodiscard]] MovieFrameResult readFrame(MovieFrameRequest request);
    void clear(QUuid document);
    void clearAll();
    [[nodiscard]] qsizetype cacheBytes() const;
    [[nodiscard]] std::size_t decoderCount() const;

  private:
    struct FrameKey
    {
        QUuid document;
        QUuid media;
        int frame;
        bool hasTimestamp;
        qint64 timestamp;
        bool operator==(const FrameKey&) const = default;
        friend std::size_t qHash(const FrameKey& key, std::size_t seed = 0) noexcept
        {
            return qHashMulti(seed, key.document, key.media, key.frame, key.hasTimestamp, key.timestamp);
        }
    };
    struct Decoder
    {
        QUuid document;
        QUuid media;
        QString path;
        VideoOrientation orientation;
        std::shared_ptr<const TrackerArchive> archive;
        std::unique_ptr<VideoReader> reader;
        std::uint64_t nextFrame = 0;
    };
    [[nodiscard]] Decoder& decoder(const MovieFrameRequest& request);
    [[nodiscard]] QImage decodeFrame(Decoder& decoder, const MovieFrameRequest& request);
    [[nodiscard]] QImage seekFrame(Decoder& decoder, const MovieFrameRequest& request, std::chrono::nanoseconds timestamp);
    void clearCache(QUuid document);

    QCache<FrameKey, QImage> m_Cache;
    std::list<Decoder> m_Decoders;
};

} // namespace motrack
