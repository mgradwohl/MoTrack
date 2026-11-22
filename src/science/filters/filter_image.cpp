#include "science/filters/filter_image.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace motrack::science
{

namespace
{

std::size_t pixelCount(int width, int height)
{
    if (width < 0 || height < 0)
    {
        throw std::invalid_argument("FilterImage dimensions must not be negative");
    }
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    // Pixel indices are computed with int arithmetic, as in Java.
    if (w != 0 && h > static_cast<std::size_t>(std::numeric_limits<int>::max()) / w)
    {
        throw std::invalid_argument("FilterImage is too large");
    }
    return w * h;
}

/// Java2D's mul8table (AlphaMath.c): round(a * b / 255) computed in 8.24 fixed point.
std::array<std::array<std::uint8_t, 256>, 256> makeMul8Table() noexcept
{
    std::array<std::array<std::uint8_t, 256>, 256> table{};
    for (std::uint32_t i = 1; i < 256; ++i)
    {
        const std::uint32_t increment = (i << 16U) + (i << 8U) + i;
        std::uint32_t value = increment + (1U << 23U);
        for (std::uint32_t j = 1; j < 256; ++j)
        {
            table[i][j] = static_cast<std::uint8_t>(value >> 24U);
            value += increment;
        }
    }
    return table;
}

const std::array<std::array<std::uint8_t, 256>, 256>& mul8() noexcept
{
    static const auto table = makeMul8Table();
    return table;
}

} // namespace

FilterImage::FilterImage(int width, int height, std::uint32_t fill) : m_Width(width), m_Height(height)
{
    m_Pixels.assign(pixelCount(width, height), fill);
    if (m_Pixels.empty())
    {
        m_Width = m_Height = 0;
    }
}

FilterImage::FilterImage(int width, int height, std::vector<std::uint32_t> pixels)
    : m_Width(width), m_Height(height), m_Pixels(std::move(pixels))
{
    if (m_Pixels.size() != pixelCount(width, height))
    {
        throw std::invalid_argument("FilterImage pixel count does not match its dimensions");
    }
    if (m_Pixels.empty())
    {
        m_Width = m_Height = 0;
    }
}

std::uint32_t FilterImage::at(int x, int y) const
{
    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height)
    {
        throw std::out_of_range("FilterImage pixel outside the image");
    }
    return m_Pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(m_Width)) + static_cast<std::size_t>(x)];
}

bool FilterImage::sameColors(const FilterImage& other) const noexcept
{
    return m_Width == other.m_Width && m_Height == other.m_Height &&
           std::ranges::equal(m_Pixels, other.m_Pixels, [](std::uint32_t a, std::uint32_t b) { return ((a ^ b) & RGB_MASK) == 0; });
}

void compositeSourceOver(const FilterImage& source, FilterImage& destination)
{
    const int width = std::min(source.width(), destination.width());
    const int height = std::min(source.height(), destination.height());
    const auto in = source.pixels();
    auto out = destination.pixels();
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::uint32_t pixel = in[(static_cast<std::size_t>(y) * static_cast<std::size_t>(source.width())) + x];
            std::uint32_t& target = out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(destination.width())) + x];
            const auto alpha = static_cast<std::size_t>(alphaOf(pixel));
            if (alpha == 0)
            {
                continue;
            }
            if (alpha == 255)
            {
                target = OPAQUE_ALPHA | (pixel & RGB_MASK);
                continue;
            }
            // AlphaMacros.h DEFINE_SRCOVER_MASKBLIT without a mask, for an opaque, non-premultiplied
            // IntRgb destination: res = MUL8(srcA, src) + MUL8(0xff - srcA, dst).
            const auto& table = mul8();
            const std::size_t inverse = 255 - alpha;
            const auto blend = [&](int s, int d)
            {
                return static_cast<std::uint32_t>(table[alpha][static_cast<std::size_t>(s)] + table[inverse][static_cast<std::size_t>(d)]);
            };
            target =
                opaqueRgb(blend(redOf(pixel), redOf(target)), blend(greenOf(pixel), greenOf(target)), blend(blueOf(pixel), blueOf(target)));
        }
    }
}

} // namespace motrack::science
