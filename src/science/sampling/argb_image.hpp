#pragma once

#include "science/argb_pixel.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace motrack::science
{

/// A position in image pixels: origin at the top-left corner of the frame, x to the right and y down.
///
/// Pixel (x, y) covers [x, x + 1) x [y, y + 1), so its centre is (x + 0.5, y + 0.5). These are the
/// coordinates Tracker saves for RGB-region and line-profile steps; calibration to world units is
/// applied by the coordinate system (slices C1/C6), not by the sampling kernels.
struct ImagePoint
{
    double x = 0.0;
    double y = 0.0;
};

/// An integer pixel rectangle, laid out like `java.awt.Rectangle`.
struct PixelRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    friend bool operator==(const PixelRect&, const PixelRect&) = default;
};

/// An owned frame of 32-bit 0xAARRGGBB words, rows from the top, `stride` words apart.
///
/// The sampling kernels read only the red, green and blue bytes, as Tracker reads its
/// `TYPE_INT_RGB` rasters, so QImage `Format_RGB32`/`Format_ARGB32` scan lines can be copied in
/// unchanged. Tracker measures `Video.getImage()`, which is the filtered frame when the filter
/// stack is enabled, at the video's own resolution (see docs/image-sampling.md). This is a
/// deliberately minimal frame type: slice C6 unifies it with the image type of the K6 filter kernels.
class ArgbImage
{
  public:
    /// Largest supported width or height (8K video is 7680 x 4320).
    static constexpr int MAX_DIMENSION = 1 << 15;

    /// An empty 0 x 0 image.
    ArgbImage() = default;

    /// A `width` x `height` image filled with `argb`; nothing when a dimension is negative or above MAX_DIMENSION.
    [[nodiscard]] static std::optional<ArgbImage> filled(int width, int height, std::uint32_t argb);

    /// Adopts rows of `stride` words. Returns nothing when a dimension is out of range, `stride` is
    /// less than `width`, or `pixels` holds fewer than `stride * (height - 1) + width` words.
    [[nodiscard]] static std::optional<ArgbImage> fromPixels(int width, int height, std::size_t stride, std::vector<std::uint32_t> pixels);

    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    /// Words between the starts of consecutive rows.
    [[nodiscard]] std::size_t stride() const noexcept;

    [[nodiscard]] bool contains(int x, int y) const noexcept;

    /// The word at (x, y). Precondition: `contains(x, y)`.
    [[nodiscard]] std::uint32_t pixel(int x, int y) const noexcept;

    /// Replaces the word at (x, y). Precondition: `contains(x, y)`.
    void setPixel(int x, int y, std::uint32_t argb) noexcept;

    /// The `width()` words of row `y`. Precondition: 0 <= y < height().
    [[nodiscard]] std::span<const std::uint32_t> row(int y) const noexcept;

  private:
    ArgbImage(int width, int height, std::size_t stride, std::vector<std::uint32_t> pixels) noexcept;

    [[nodiscard]] std::size_t index(int x, int y) const noexcept;

    int m_Width = 0;
    int m_Height = 0;
    std::size_t m_Stride = 0;
    std::vector<std::uint32_t> m_Pixels;
};

/// `RGBRegion.getLuma`: CCIR 601 weights `0.299 R + 0.587 G + 0.114 B`, evaluated left to right
/// with two roundings per product and sum like Java (the library is built with -ffp-contract=off).
[[nodiscard]] double trackerLuma(double red, double green, double blue) noexcept;

} // namespace motrack::science
