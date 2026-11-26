#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>

namespace motrack
{

/// Uncompressed capture pixel layouts, named by byte or plane order in memory (as Qt names video formats).
enum class CapturePixelFormat : std::uint8_t
{
    Bgra,
    Bgrx,
    Rgba,
    Rgbx,
    Argb,
    Xrgb,
    Abgr,
    Xbgr,
    Yuv420p,
    Yv12,
    Yuv422p,
    Nv12,
    Nv21,
    Yuyv,
    Uyvy,
    Gray8,
};

/// A borrowed view of one uncompressed frame, valid only for the call it is passed to. YUV samples use
/// video range (16–235) unless fullRange is set, and BT.601 coefficients unless bt709 is set.
struct CaptureImage
{
    int width = 0;
    int height = 0;
    CapturePixelFormat format = CapturePixelFormat::Bgra;
    std::array<const std::uint8_t*, 3> planes{};
    std::array<int, 3> strides{};
    bool fullRange = false;
    bool bt709 = false;
};

/// Borrow an RGB32, ARGB32, RGBA8888, RGBX8888 or Grayscale8 image's pixels; another format is null.
[[nodiscard]] CaptureImage captureImageFromQImage(const QImage& image);

/// Converts capture frames to RGBA images for preview. Reuses its scaler; one thread at a time.
class CaptureConverter
{
  public:
    CaptureConverter();
    ~CaptureConverter();
    CaptureConverter(const CaptureConverter&) = delete;
    CaptureConverter& operator=(const CaptureConverter&) = delete;
    /// The frame scaled to fit bound (never enlarged), keeping its aspect ratio. Throws VideoError.
    [[nodiscard]] QImage toImage(const CaptureImage& frame, QSize bound);
    /// How many scaler contexts were built: one per distinct frame layout, size, range and matrix, not one per frame.
    [[nodiscard]] int contextsCreated() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

/// Counts for a finished capture: written frames and the time from the first to the last of them.
struct CaptureSummary
{
    std::int64_t frames = 0;
    std::chrono::microseconds duration{0};
};

/// Writes captured frames into a QuickTime movie of Motion JPEG frames with MoTrack's own FFmpeg.
/// Every frame is intra-coded at a fixed high quality, and each keeps the presentation time the
/// caller supplies: the first written frame is time zero and later times are relative to it, stored
/// in a 1/90000 s track timescale. Variable frame intervals therefore remain in the file, and the
/// movie index reports them as the frame times. The first frame fixes the movie size; a frame of another
/// size is refused (scaling it would distort the recorded geometry and any calibration).
/// One thread at a time; throws VideoError for unsupported frames and write failures.
class CaptureEncoder
{
  public:
    static constexpr int TIMESCALE = 90'000;

    /// Creates or truncates path. Nothing is written until the first frame.
    explicit CaptureEncoder(const QString& path);
    /// Without finish(), closes the incomplete movie without a trailer; the owner deletes the file.
    ~CaptureEncoder();
    CaptureEncoder(const CaptureEncoder&) = delete;
    CaptureEncoder& operator=(const CaptureEncoder&) = delete;

    /// Encodes one frame at time on the caller's clock. Returns false, writing nothing, when the time
    /// does not advance past the previous frame at the track timescale.
    bool write(const CaptureImage& frame, std::chrono::microseconds time);
    /// Flushes the encoder and writes the movie index. Further writes are invalid.
    CaptureSummary finish();
    /// How many scaler contexts were built: one while the frame layout, range and matrix stay the same.
    [[nodiscard]] int contextsCreated() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace motrack
