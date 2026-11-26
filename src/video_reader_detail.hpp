#pragma once

// FFmpeg-facing pieces of video_reader.cpp, exposed so tests can drive swscale with synthetic frames.

#include <QImage>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

struct AVFrame;
struct SwsContext;

namespace motrack::detail
{

/// Row stride and allocation size, in bytes, of an RGBA destination for swscale.
struct RgbaLayout
{
    int stride = 0;
    std::size_t bytes = 0;
};

/// Rounds rows up to 64 bytes and keeps 64 spare bytes after the last row, like av_image_alloc with
/// align 64. In the pinned FFmpeg n9.0.2, swscale's x86 SSSE3 YUV-to-RGB converter stores 16 pixels at
/// a time and can write up to 15 pixels past the requested width (#211). FFmpeg master stops at the
/// width since c6af16b; re-evaluate this padding when the pin includes it. Returns nothing for
/// unsupported dimensions.
[[nodiscard]] std::optional<RgbaLayout> paddedRgbaLayout(int width, int height);

/// Converts decoded frames to RGBA without resizing them. Each converter belongs to a single thread.
///
/// Each frame is converted with its own range and YCbCr matrix (#261). FFmpeg's deprecated JPEG-range formats
/// (yuvj420p and the like) are full range; other YUV frames are full range only when their color_range says
/// so. RGB and integer gray frames are full range, as in swscale's own frame API. BT.709, FCC, SMPTE 240M and BT.2020
/// frames use their matrix; every other YUV frame, including one with no colorspace, uses BT.601, swscale's
/// default. The chroma siting is swscale's default. One swscale context is kept for as long as these inputs
/// are unchanged.
class RgbaFrameConverter
{
  public:
    /// Returns an image that owns its padded pixel buffer, so copies may be shared across threads.
    [[nodiscard]] QImage convert(const AVFrame& frame);
    /// Converts into caller memory laid out by paddedRgbaLayout().
    void convertInto(const AVFrame& frame, std::span<std::uint8_t> destination);
    /// The number of swscale contexts this converter has created, for tests of context reuse.
    [[nodiscard]] std::uint64_t contextsCreated() const noexcept
    {
        return m_ContextsCreated;
    }

    /// Every input of a swscale context. swscale rewrites the format and flag options of a context it
    /// initialises, so sws_getCachedContext cannot tell whether a context still fits a frame (#261).
    struct Key
    {
        int width = 0;
        int height = 0;
        int sourceFormat = -1; ///< An AVPixelFormat; a JPEG-range format is replaced by its limited-range format.
        bool fullRange = false;
        int matrix = 0; ///< SWS_CS_* coefficients of the YCbCr source.
        int destinationFormat = -1;
        int flags = 0;

        friend bool operator==(const Key&, const Key&) = default;
    };

  private:
    struct FreeScale
    {
        void operator()(SwsContext* context) const;
    };
    std::unique_ptr<SwsContext, FreeScale> m_Scale;
    Key m_Key;
    std::uint64_t m_ContextsCreated = 0;
};

} // namespace motrack::detail
