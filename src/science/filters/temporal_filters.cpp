// Filters with state carried between evaluations (ghost, dark ghost, strobe, sum) and the baseline filter.

#include "science/filters/java_arithmetic.hpp"
#include "science/filters/video_filters.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace motrack::science
{

namespace
{

int meanValue(std::uint32_t pixel) noexcept
{
    return (redOf(pixel) + greenOf(pixel) + blueOf(pixel)) / 3;
}

/// GhostFilter.setFade / StrobeFilter.setFade: Math.min(Math.abs(fade), 1).
double limitFade(double fade) noexcept
{
    return java::min(std::abs(fade), 1);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Ghost and dark ghost

void GhostFilter::setFade(double fade) noexcept
{
    m_Fade = limitFade(fade);
}

void GhostFilter::setEnabled(bool enabled)
{
    if (enabled != isEnabled())
    {
        invalidateSource();
        VideoFilter::setEnabled(enabled);
    }
}

void GhostFilter::clear()
{
    invalidateSource();
}

void GhostFilter::initialize(const FilterImage& input)
{
    const auto pixels = input.pixels();
    m_Values.resize(pixels.size());
    std::ranges::transform(pixels, m_Values.begin(), meanValue);
}

void GhostFilter::render(const FilterImage& input, FilterImage& output, int /*videoFrame*/)
{
    const auto in = input.pixels();
    auto out = output.pixels();
    const bool dark = kind() == VideoFilterKind::DarkGhost;
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const std::uint32_t pixel = in[i];
        const int value = meanValue(pixel);
        const std::int32_t previous = m_Values[i];
        const std::int32_t ghost = dark ? java::toInt(255 - ((1 - m_Fade) * (255 - previous))) : java::toInt((1 - m_Fade) * previous);
        if (dark ? ghost < value : ghost > value)
        {
            out[i] = java::packGray(ghost);
            m_Values[i] = ghost;
        }
        else
        {
            out[i] = OPAQUE_ALPHA | (pixel & RGB_MASK);
            m_Values[i] = value;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Strobe

void StrobeFilter::setFade(double fade) noexcept
{
    m_Fade = limitFade(fade);
}

void StrobeFilter::setBrightTrails(bool bright) noexcept
{
    m_BrightTrails = bright;
    invalidateSource();
}

void StrobeFilter::setEnabled(bool enabled)
{
    if (enabled != isEnabled())
    {
        invalidateSource();
        VideoFilter::setEnabled(enabled);
    }
}

void StrobeFilter::clear()
{
    invalidateSource();
}

void StrobeFilter::initialize(const FilterImage& input)
{
    m_Previous.assign(input.pixels().begin(), input.pixels().end());
}

void StrobeFilter::render(const FilterImage& input, FilterImage& output, int /*videoFrame*/)
{
    const auto in = input.pixels();
    auto out = output.pixels();
    const double keep = 1 - m_Fade;
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const std::uint32_t pixel = in[i];
        const int value = meanValue(pixel);
        int red = redOf(m_Previous[i]);
        int green = greenOf(m_Previous[i]);
        int blue = blueOf(m_Previous[i]);
        const int previousValue = (red + green + blue) / 3;
        if (m_BrightTrails) // bright trails fade to black
        {
            if (java::toInt(keep * previousValue) > value)
            {
                red = java::toInt(keep * red);
                green = java::toInt(keep * green);
                blue = java::toInt(keep * blue);
                out[i] = java::packBits(red, green, blue);
            }
            else
            {
                out[i] = OPAQUE_ALPHA | (pixel & RGB_MASK);
            }
        }
        else // dark trails fade to white
        {
            if (value > java::toInt(255 - (keep * (255 - previousValue))))
            {
                red = java::toInt(255 - (keep * (255 - red)));
                green = java::toInt(255 - (keep * (255 - green)));
                blue = java::toInt(255 - (keep * (255 - blue)));
                out[i] = java::packBits(red, green, blue);
            }
            else
            {
                out[i] = OPAQUE_ALPHA | (pixel & RGB_MASK);
            }
        }
        m_Previous[i] = out[i];
    }
}

// ---------------------------------------------------------------------------------------------
// Sum

void SumFilter::setBrightness(double fraction) noexcept
{
    // OSP compares before taking the magnitude, so NaN (never equal) is stored as NaN.
    if (fraction != m_Brightness)
    {
        m_Brightness = std::abs(fraction);
    }
}

void SumFilter::clear()
{
    if (isInitialized())
    {
        restart(m_LastInput);
        m_Brightness = 1.0;
        m_SkipSum = true;
    }
}

void SumFilter::initialize(const FilterImage& input)
{
    restart(input.pixels());
}

void SumFilter::restart(std::span<const std::uint32_t> pixels)
{
    m_Red.assign(pixels.size(), 0);
    m_Green.assign(pixels.size(), 0);
    m_Blue.assign(pixels.size(), 0);
    m_ImageCount = 0;
    addPixels(pixels);
}

void SumFilter::addPixels(std::span<const std::uint32_t> pixels)
{
    ++m_ImageCount;
    for (std::size_t i = 0; i < pixels.size(); ++i)
    {
        m_Red[i] = java::addWrapping(m_Red[i], redOf(pixels[i]));
        m_Green[i] = java::addWrapping(m_Green[i], greenOf(pixels[i]));
        m_Blue[i] = java::addWrapping(m_Blue[i], blueOf(pixels[i]));
    }
}

void SumFilter::render(const FilterImage& input, FilterImage& output, int /*videoFrame*/)
{
    const auto in = input.pixels();
    m_LastInput.assign(in.begin(), in.end());
    if (!m_SkipSum)
    {
        addPixels(in);
        m_SkipSum = true;
    }
    auto out = output.pixels();
    const double factor = m_Mean ? 1.0 / m_ImageCount : m_Brightness;
    const auto scale = [factor](std::int32_t sum)
    {
        return java::toInt(java::min(sum * factor, 255));
    };
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = java::packBits(scale(m_Red[i]), scale(m_Green[i]), scale(m_Blue[i]));
    }
}

// ---------------------------------------------------------------------------------------------
// Baseline

bool BaselineFilter::setBaselineImage(FilterImage image)
{
    if (image.width() < m_Width || image.height() < m_Height || image.empty())
    {
        return false;
    }
    m_Baseline = std::move(image);
    readBaselinePixels();
    return true;
}

void BaselineFilter::clear()
{
    m_Baseline.reset();
    m_BaselinePixels.clear();
}

void BaselineFilter::readBaselinePixels()
{
    m_BaselinePixels.clear();
    if (!m_Baseline || m_Width == 0)
    {
        return;
    }
    // Raster.getDataElements(0, 0, w, h): the baseline's top-left w x h region.
    m_BaselinePixels.reserve(static_cast<std::size_t>(m_Width) * static_cast<std::size_t>(m_Height));
    const auto pixels = m_Baseline->pixels();
    const auto stride = static_cast<std::size_t>(m_Baseline->width());
    for (int y = 0; y < m_Height; ++y)
    {
        const auto row = pixels.subspan(static_cast<std::size_t>(y) * stride, static_cast<std::size_t>(m_Width));
        m_BaselinePixels.insert(m_BaselinePixels.end(), row.begin(), row.end());
    }
}

void BaselineFilter::initialize(const FilterImage& input)
{
    m_Width = input.width();
    m_Height = input.height();
    if (m_Baseline && (m_Baseline->width() < m_Width || m_Baseline->height() < m_Height))
    {
        m_Baseline.reset();
    }
    readBaselinePixels();
}

void BaselineFilter::render(const FilterImage& input, FilterImage& output, int /*videoFrame*/)
{
    const auto in = input.pixels();
    auto out = output.pixels();
    if (!m_Baseline)
    {
        std::ranges::transform(in, out.begin(), [](std::uint32_t pixel) { return OPAQUE_ALPHA | (pixel & RGB_MASK); });
        return;
    }
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const std::uint32_t pixel = in[i];
        const std::uint32_t base = m_BaselinePixels[i];
        const auto subtract = [](int value, int baseline)
        {
            return static_cast<std::uint32_t>(std::max(value - baseline, 0));
        };
        out[i] =
            opaqueRgb(subtract(redOf(pixel), redOf(base)), subtract(greenOf(pixel), greenOf(base)), subtract(blueOf(pixel), blueOf(base)));
    }
}

} // namespace motrack::science
