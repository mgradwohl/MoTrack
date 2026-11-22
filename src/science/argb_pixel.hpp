#pragma once

#include <cstdint>

/// Channel access for packed 0xAARRGGBB pixels in Java's `int` layout, shared by the video filter (K6)
/// and image sampling (K8) kernels. Tracker reads channels as `(pixel >> 16) & 0xff` and so on.
namespace motrack::science
{

/// Alpha byte of an opaque packed pixel.
constexpr std::uint32_t OPAQUE_ALPHA = 0xFF000000U;
/// Mask of the colour bytes of a packed pixel.
constexpr std::uint32_t RGB_MASK = 0x00FFFFFFU;

/// Packs 8-bit channels as an opaque 0xAARRGGBB pixel. Channels must be in [0, 255].
constexpr std::uint32_t opaqueRgb(std::uint32_t red, std::uint32_t green, std::uint32_t blue) noexcept
{
    return OPAQUE_ALPHA | (red << 16U) | (green << 8U) | blue;
}

[[nodiscard]] constexpr int redOf(std::uint32_t pixel) noexcept
{
    return static_cast<int>((pixel >> 16U) & 0xFFU);
}

[[nodiscard]] constexpr int greenOf(std::uint32_t pixel) noexcept
{
    return static_cast<int>((pixel >> 8U) & 0xFFU);
}

[[nodiscard]] constexpr int blueOf(std::uint32_t pixel) noexcept
{
    return static_cast<int>(pixel & 0xFFU);
}

[[nodiscard]] constexpr int alphaOf(std::uint32_t pixel) noexcept
{
    return static_cast<int>(pixel >> 24U);
}

} // namespace motrack::science
