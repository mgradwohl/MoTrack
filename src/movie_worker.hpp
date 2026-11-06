#pragma once

#include "video_reader.hpp"

#include <QCache>
#include <QObject>

#include <atomic>

namespace motrack
{

/// Owns the decoder and bounded frame cache on one background thread.
class MovieWorker final : public QObject
{
    Q_OBJECT

  public:
    explicit MovieWorker(const std::atomic<quint64>& current);

  public Q_SLOTS:
    void clear();

    void readFrame(const QString& path, int frame, quint64 request, bool useDisplayOrientation, bool hasTimestamp, qint64 timestamp);

  Q_SIGNALS:
    void frameDecoded(int frame, quint64 request, const QImage& image);

  private:
    [[nodiscard]] QImage decodeFrame(
        const QString& path, int frame, quint64 request, VideoOrientation orientation, std::optional<std::chrono::nanoseconds> timestamp);
    [[nodiscard]] QImage seekFrame(std::chrono::nanoseconds timestamp, quint64 request);
    void cacheFrame(int frame, const QImage& image);

    const std::atomic<quint64>& m_CurrentRequest;
    QCache<int, QImage> m_Cache{qsizetype{32} * 1024 * 1024};
    std::unique_ptr<VideoReader> m_Reader;
    std::uint64_t m_NextFrame = 0;
};

} // namespace motrack
