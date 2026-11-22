#pragma once

#include "frame_time_base.hpp"

#include <QImage>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace motrack
{

struct MovieTimeline;

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

struct MovieIndex
{
    int frameCount = 0;
    std::vector<double> frameTimes;
    std::vector<std::chrono::nanoseconds> presentationTimes;
    /// The video stream's time base, to which frameTimes were quantized before rescaling to nanoseconds.
    FrameTimeBase timeBase = {};
};

/// Decodes a local movie in presentation order. Each reader belongs to a single thread.
class VideoReader
{
  public:
    explicit VideoReader(const QString& path, VideoOrientation orientation = VideoOrientation::Display);
    ~VideoReader();

    VideoReader(const VideoReader&) = delete;
    VideoReader& operator=(const VideoReader&) = delete;

    /// Seeks to a keyframe at or before the timestamp and resets decode ordinals.
    [[nodiscard]] bool seekTo(std::chrono::nanoseconds time);
    /// Returns the next frame, or an empty value once the decoder has drained.
    [[nodiscard]] std::optional<MovieFrame> readNext();
    /// Decodes a frame and returns its ordinal and timestamp without converting its pixels.
    [[nodiscard]] std::optional<MovieFrame> skipNext();
    /// The video stream's time base (seconds per timestamp tick), or unknown when the container reports none.
    [[nodiscard]] FrameTimeBase timeBase() const noexcept;

  private:
    [[nodiscard]] std::optional<MovieFrame> decodeNext(bool convertPixels);
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

/// Indexes presentation-order frames and monotonic timestamps in seconds from the movie's first frame.
[[nodiscard]] MovieIndex buildMovieIndex(const QString& path, VideoOrientation orientation, const std::atomic_bool& cancelled);

/// Retain the completed index's absolute timestamps without copying them into each view.
[[nodiscard]] std::shared_ptr<const MovieTimeline>
retainMovieTimeline(const QString& path, VideoOrientation orientation, MovieIndex& index);
[[nodiscard]] bool matchesMovieTimeline(const MovieTimeline& timeline, const QString& path, VideoOrientation orientation);

} // namespace motrack
