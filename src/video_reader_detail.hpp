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
class RgbaFrameConverter
{
  public:
    /// Returns an image that owns its padded pixel buffer, so copies may be shared across threads.
    [[nodiscard]] QImage convert(const AVFrame& frame);
    /// Converts into caller memory laid out by paddedRgbaLayout().
    void convertInto(const AVFrame& frame, std::span<std::uint8_t> destination);

  private:
    struct FreeScale
    {
        void operator()(SwsContext* context) const;
    };
    std::unique_ptr<SwsContext, FreeScale> m_Scale;
};

} // namespace motrack::detail
