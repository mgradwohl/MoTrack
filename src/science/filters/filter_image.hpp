#pragma once

#include "science/argb_pixel.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace motrack::science
{

/**
 * An owned image of packed 32-bit pixels in Java's `int` layout, 0xAARRGGBB, stored row by row from
 * the top-left pixel. Pixel (x, y) covers the image-space square [x, x + 1) x [y, y + 1); image
 * coordinates grow right and down, in pixels, exactly as in Tracker's image space.
 *
 * Video filters treat every input as Tracker's opaque `BufferedImage.TYPE_INT_RGB` frame buffer: they
 * read only the colour bytes and write opaque pixels. Use compositeSourceOver() to flatten an image whose
 * alpha matters before filtering it.
 */
class FilterImage
{
  public:
    /// An empty (0 x 0) image.
    FilterImage() = default;
    /// A width x height image filled with `fill`. Throws std::invalid_argument for a negative or
    /// overflowing size; a zero dimension gives an empty image.
    FilterImage(int width, int height, std::uint32_t fill = OPAQUE_ALPHA);
    /// Adopts `pixels`, which must hold exactly width x height values (std::invalid_argument otherwise).
    FilterImage(int width, int height, std::vector<std::uint32_t> pixels);

    [[nodiscard]] int width() const noexcept
    {
        return m_Width;
    }

    [[nodiscard]] int height() const noexcept
    {
        return m_Height;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_Pixels.empty();
    }

    [[nodiscard]] std::span<const std::uint32_t> pixels() const noexcept
    {
        return m_Pixels;
    }

    [[nodiscard]] std::span<std::uint32_t> pixels() noexcept
    {
        return m_Pixels;
    }

    /// The pixel at column x, row y. Throws std::out_of_range outside the image.
    [[nodiscard]] std::uint32_t at(int x, int y) const;
    /// True when both images have the same size and colour bytes (alpha is ignored).
    [[nodiscard]] bool sameColors(const FilterImage& other) const noexcept;

    friend bool operator==(const FilterImage&, const FilterImage&) = default;

  private:
    int m_Width = 0;
    int m_Height = 0;
    std::vector<std::uint32_t> m_Pixels;
};

/**
 * Draws `source` (straight, non-premultiplied 0xAARRGGBB) over the opaque `destination` with Java2D's
 * SrcOver rule, aligned at the top-left corner and clipped to the destination; destination alpha stays
 * opaque. Each channel becomes MUL8(a, src) + MUL8(255 - a, dst) using Java2D's 8-bit multiplication
 * table, a = 0 leaves the destination unchanged and a = 255 copies the source.
 *
 * This is `Graphics2D.drawImage` of a TYPE_INT_ARGB image onto a TYPE_INT_RGB one. Tracker's
 * VideoAdapter draws each new frame onto its previous, uncleared frame buffer this way, and a filter
 * handed any other image type first draws it onto a new black buffer (Filter.initializeSource).
 */
void compositeSourceOver(const FilterImage& source, FilterImage& destination);

} // namespace motrack::science
