// Per-pixel filters: negative, grayscale, brightness/contrast, highlight/shadow (log) and deinterlace.
// Each port keeps the operation order of its OSP class (compiled with -ffp-contract=off).

#include "science/filters/java_arithmetic.hpp"
#include "science/filters/video_filters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>

namespace motrack::science
{

// The filters' Java arithmetic (science/filters/java_arithmetic.hpp).
namespace detail = filters::detail;

namespace
{

constexpr double PI = std::numbers::pi; // Math.PI

// java.awt.Color.RGBtoHSB / HSBtoRGB, in single precision.
struct Hsb
{
    float hue;
    float saturation;
    float brightness;
};

Hsb rgbToHsb(int r, int g, int b) noexcept
{
    const int cmax = std::max({r, g, b});
    const int cmin = std::min({r, g, b});
    Hsb hsb{};
    hsb.brightness = static_cast<float>(cmax) / 255.0F;
    hsb.saturation = cmax != 0 ? static_cast<float>(cmax - cmin) / static_cast<float>(cmax) : 0.0F;
    if (hsb.saturation == 0.0F)
    {
        hsb.hue = 0.0F;
        return hsb;
    }
    const auto range = static_cast<float>(cmax - cmin);
    const float redc = static_cast<float>(cmax - r) / range;
    const float greenc = static_cast<float>(cmax - g) / range;
    const float bluec = static_cast<float>(cmax - b) / range;
    float hue = 0.0F;
    if (r == cmax)
    {
        hue = bluec - greenc;
    }
    else if (g == cmax)
    {
        hue = 2.0F + redc - bluec;
    }
    else
    {
        hue = 4.0F + greenc - redc;
    }
    hue = hue / 6.0F;
    if (hue < 0.0F)
    {
        hue = hue + 1.0F;
    }
    hsb.hue = hue;
    return hsb;
}

std::int32_t channel(float value) noexcept
{
    return detail::toInt((value * 255.0F) + 0.5F);
}

std::uint32_t hsbToRgb(float hue, float saturation, float brightness) noexcept
{
    std::int32_t r = 0;
    std::int32_t g = 0;
    std::int32_t b = 0;
    if (saturation == 0.0F)
    {
        r = g = b = channel(brightness);
    }
    else
    {
        const float h = (hue - static_cast<float>(std::floor(static_cast<double>(hue)))) * 6.0F;
        const float f = h - static_cast<float>(std::floor(static_cast<double>(h)));
        const float p = brightness * (1.0F - saturation);
        const float q = brightness * (1.0F - (saturation * f));
        const float t = brightness * (1.0F - (saturation * (1.0F - f)));
        switch (detail::toInt(h))
        {
        case 0:
            r = channel(brightness);
            g = channel(t);
            b = channel(p);
            break;
        case 1:
            r = channel(q);
            g = channel(brightness);
            b = channel(p);
            break;
        case 2:
            r = channel(p);
            g = channel(brightness);
            b = channel(t);
            break;
        case 3:
            r = channel(p);
            g = channel(q);
            b = channel(brightness);
            break;
        case 4:
            r = channel(t);
            g = channel(p);
            b = channel(brightness);
            break;
        default: // 5: h is in [0, 6)
            r = channel(brightness);
            g = channel(p);
            b = channel(q);
            break;
        }
    }
    return detail::packBits(r, g, b);
}

} // namespace

// ---------------------------------------------------------------------------------------------

void NegativeFilter::render(const FilterImage& input, FilterImage& output, int /*videoFrame*/)
{
    std::ranges::transform(input.pixels(), output.pixels().begin(), [](std::uint32_t pixel) { return OPAQUE_ALPHA | (~pixel & RGB_MASK); });
}

// ---------------------------------------------------------------------------------------------

void GrayScaleFilter::setWeights(double red, double green, double blue) noexcept
{
    m_Weights = {red, green, blue};
    m_WeightSum = (red + green) + blue;
}

void GrayScaleFilter::render(const FilterImage& input, FilterImage& output, int /*videoFrame*/)
{
    // Named copies, not structured bindings: clang-analyzer 18 misreads bindings captured by a lambda.
    const double red = m_Weights[0];
    const double green = m_Weights[1];
    const double blue = m_Weights[2];
    std::ranges::transform(input.pixels(),
                           output.pixels().begin(),
                           [&](std::uint32_t pixel)
                           {
                               const double gray =
                                   (((red * redOf(pixel)) + (green * greenOf(pixel))) + (blue * blueOf(pixel))) / m_WeightSum;
                               return detail::packGray(detail::toInt(gray));
                           });
}

// ---------------------------------------------------------------------------------------------

BrightnessFilter::BrightnessFilter() noexcept : VideoFilter(VideoFilterKind::Brightness)
{
    updateFactors();
}

void BrightnessFilter::setBrightness(int brightness) noexcept
{
    m_Brightness = brightness;
    updateFactors();
}

void BrightnessFilter::setContrast(double contrast) noexcept
{
    m_Contrast = contrast;
    updateFactors();
}

void BrightnessFilter::clear()
{
    m_Brightness = DEFAULT_BRIGHTNESS;
    m_Contrast = DEFAULT_CONTRAST;
    updateFactors();
}

void BrightnessFilter::updateFactors() noexcept
{
    const double theta = PI * m_Contrast / 200;
    const auto [sin, cos] = detail::sinCos(theta);
    m_Offset1 = (sin * sin * m_Brightness) - 127;
    m_Offset2 = 127 + (cos * cos * m_Brightness);
    m_Slope = sin / cos;
}

void BrightnessFilter::render(const FilterImage& input, FilterImage& output, int /*videoFrame*/)
{
    // Tracker truncates the exact double; the correction absorbs a few ulps of error below an integer.
    const double guard = m_Arithmetic == FilterArithmetic::Corrected ? 1e-9 : 0.0;
    // The mapping depends on the channel value alone, so a 256-entry table is bit for bit the same.
    std::array<std::uint32_t, 256> table{};
    for (std::size_t value = 0; value < table.size(); ++value)
    {
        const std::int32_t mapped = detail::toInt((m_Slope * (static_cast<int>(value) + m_Offset1)) + m_Offset2 + guard);
        table[value] = static_cast<std::uint32_t>(std::min(std::max(mapped, 0), 255));
    }
    std::ranges::transform(input.pixels(),
                           output.pixels().begin(),
                           [&](std::uint32_t pixel)
                           {
                               return opaqueRgb(table[static_cast<std::size_t>(redOf(pixel))],
                                                table[static_cast<std::size_t>(greenOf(pixel))],
                                                table[static_cast<std::size_t>(blueOf(pixel))]);
                           });
}

// ---------------------------------------------------------------------------------------------

LogFilter::LogFilter() noexcept : VideoFilter(VideoFilterKind::Log)
{
    for (std::size_t i = 0; i < m_Lookup.size(); ++i)
    {
        m_Lookup[i] = static_cast<std::int32_t>(i);
    }
}

void LogFilter::setLevel(int level) noexcept
{
    if (m_Level == level)
    {
        return;
    }
    m_Level = level;
    if (level == 0)
    {
        for (std::size_t i = 0; i < m_Lookup.size(); ++i)
        {
            m_Lookup[i] = static_cast<std::int32_t>(i);
        }
        return;
    }
    double limit = std::abs(0.01 * level);
    limit = detail::min(1, 5 * limit) * std::exp(6 * limit);
    const double c = 255 / limit;
    const double bc = 255 / std::log10(limit + 1);
    for (std::size_t i = 0; i < m_Lookup.size(); ++i)
    {
        const auto index = static_cast<int>(i);
        if (level > 0)
        {
            m_Lookup[i] = detail::lowBits(detail::round(bc * std::log10(1 + (index / c))));
        }
        else
        {
            m_Lookup[i] = 255 - detail::lowBits(detail::round(bc * std::log10(1 + ((255 - index) / c))));
        }
    }
}

void LogFilter::render(const FilterImage& input, FilterImage& output, int /*videoFrame*/)
{
    std::ranges::transform(input.pixels(),
                           output.pixels().begin(),
                           [&](std::uint32_t pixel)
                           {
                               const int r = redOf(pixel);
                               const int g = greenOf(pixel);
                               const int b = blueOf(pixel);
                               if (m_Grayscale)
                               {
                                   return detail::packGray(m_Lookup[static_cast<std::size_t>((r + g + b) / 3)]);
                               }
                               const Hsb hsb = rgbToHsb(r, g, b);
                               const std::int32_t gray = detail::toInt(hsb.brightness * 255.0F);
                               const std::int32_t mapped = m_Lookup[static_cast<std::size_t>(gray)];
                               return hsbToRgb(hsb.hue, hsb.saturation, static_cast<float>(mapped / 255.0));
                           });
}

// ---------------------------------------------------------------------------------------------

void DeinterlaceFilter::render(const FilterImage& input, FilterImage& output, int /*videoFrame*/)
{
    const auto in = input.pixels();
    auto out = output.pixels();
    const auto w = static_cast<std::size_t>(input.width());
    const int h = input.height();
    const std::size_t offset = m_Odd ? w : 0;
    std::size_t p = 0;
    for (int i = 0; i < h - 1; i += 2, p += w)
    {
        for (std::size_t j = 0; j < w; ++j, ++p)
        {
            out[p] = out[p + w] = OPAQUE_ALPHA | (in[p + offset] & RGB_MASK);
        }
    }
    if (h % 2 != 0)
    {
        for (std::size_t j = 0; j < w; ++j, ++p)
        {
            out[p] = OPAQUE_ALPHA | (in[p] & RGB_MASK);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// clone()

std::unique_ptr<VideoFilter> NegativeFilter::clone() const
{
    return std::make_unique<NegativeFilter>(*this);
}

std::unique_ptr<VideoFilter> GrayScaleFilter::clone() const
{
    return std::make_unique<GrayScaleFilter>(*this);
}

std::unique_ptr<VideoFilter> BrightnessFilter::clone() const
{
    return std::make_unique<BrightnessFilter>(*this);
}

std::unique_ptr<VideoFilter> LogFilter::clone() const
{
    return std::make_unique<LogFilter>(*this);
}

std::unique_ptr<VideoFilter> DeinterlaceFilter::clone() const
{
    return std::make_unique<DeinterlaceFilter>(*this);
}

} // namespace motrack::science
