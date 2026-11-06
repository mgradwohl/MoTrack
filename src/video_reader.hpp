#pragma once

#include <QImage>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

namespace motrack
{

struct MovieFrame
{
    std::uint64_t ordinal;
    std::optional<std::chrono::nanoseconds> presentationTime;
    QImage image;
};

class VideoError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

enum class VideoOrientation : std::uint8_t
{
    Encoded,
    Display,
};

/// Decodes a local movie in presentation order. Each reader belongs to a single thread.
class VideoReader
{
  public:
    explicit VideoReader(const QString& path, VideoOrientation orientation = VideoOrientation::Display);
    ~VideoReader();

    VideoReader(const VideoReader&) = delete;
    VideoReader& operator=(const VideoReader&) = delete;

    /// Returns the next frame, or an empty value once the decoder has drained.
    [[nodiscard]] std::optional<MovieFrame> readNext();
    /// Decodes a frame and returns its ordinal and timestamp without converting its pixels.
    [[nodiscard]] std::optional<MovieFrame> skipNext();

  private:
    [[nodiscard]] std::optional<MovieFrame> decodeNext(bool convertPixels);
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace motrack
