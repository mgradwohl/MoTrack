#include "science/sampling/argb_image.hpp"

#include <utility>

namespace motrack::science
{

namespace
{

bool validDimensions(int width, int height) noexcept
{
    return width >= 0 && height >= 0 && width <= ArgbImage::MAX_DIMENSION && height <= ArgbImage::MAX_DIMENSION;
}

} // namespace

ArgbImage::ArgbImage(int width, int height, std::size_t stride, std::vector<std::uint32_t> pixels) noexcept
    : m_Width(width), m_Height(height), m_Stride(stride), m_Pixels(std::move(pixels))
{}

std::optional<ArgbImage> ArgbImage::filled(int width, int height, std::uint32_t argb)
{
    if (!validDimensions(width, height))
    {
        return std::nullopt;
    }
    const auto stride = static_cast<std::size_t>(width);
    return ArgbImage(width, height, stride, std::vector<std::uint32_t>(stride * static_cast<std::size_t>(height), argb));
}

std::optional<ArgbImage> ArgbImage::fromPixels(int width, int height, std::size_t stride, std::vector<std::uint32_t> pixels)
{
    if (!validDimensions(width, height) || std::cmp_less(stride, width))
    {
        return std::nullopt;
    }
    const std::size_t required = height == 0 ? 0 : (stride * static_cast<std::size_t>(height - 1)) + static_cast<std::size_t>(width);
    if (pixels.size() < required)
    {
        return std::nullopt;
    }
    return ArgbImage(width, height, stride, std::move(pixels));
}

int ArgbImage::width() const noexcept
{
    return m_Width;
}

int ArgbImage::height() const noexcept
{
    return m_Height;
}

std::size_t ArgbImage::stride() const noexcept
{
    return m_Stride;
}

bool ArgbImage::contains(int x, int y) const noexcept
{
    return x >= 0 && y >= 0 && x < m_Width && y < m_Height;
}

std::uint32_t ArgbImage::pixel(int x, int y) const noexcept
{
    return m_Pixels[index(x, y)];
}

void ArgbImage::setPixel(int x, int y, std::uint32_t argb) noexcept
{
    m_Pixels[index(x, y)] = argb;
}

std::span<const std::uint32_t> ArgbImage::row(int y) const noexcept
{
    return std::span<const std::uint32_t>(m_Pixels).subspan(index(0, y), static_cast<std::size_t>(m_Width));
}

std::size_t ArgbImage::index(int x, int y) const noexcept
{
    return (static_cast<std::size_t>(y) * m_Stride) + static_cast<std::size_t>(x);
}

int redOf(std::uint32_t argb) noexcept
{
    return static_cast<int>((argb >> 16U) & 0xffU);
}

int greenOf(std::uint32_t argb) noexcept
{
    return static_cast<int>((argb >> 8U) & 0xffU);
}

int blueOf(std::uint32_t argb) noexcept
{
    return static_cast<int>(argb & 0xffU);
}

double trackerLuma(double red, double green, double blue) noexcept
{
    return (0.299 * red) + (0.587 * green) + (0.114 * blue);
}

} // namespace motrack::science
