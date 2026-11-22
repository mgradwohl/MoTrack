#include "science/filters/video_filter.hpp"

#include "science/filters/video_filters.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace motrack::science
{

namespace
{

struct KindName
{
    VideoFilterKind kind;
    std::string_view className;
};

// TrackerPanel's registration order (OSP media/core classes).
constexpr std::array<KindName, 14> KIND_NAMES{{
    {.kind = VideoFilterKind::Deinterlace, .className = "org.opensourcephysics.media.core.DeinterlaceFilter"},
    {.kind = VideoFilterKind::Ghost, .className = "org.opensourcephysics.media.core.GhostFilter"},
    {.kind = VideoFilterKind::Strobe, .className = "org.opensourcephysics.media.core.StrobeFilter"},
    {.kind = VideoFilterKind::DarkGhost, .className = "org.opensourcephysics.media.core.DarkGhostFilter"},
    {.kind = VideoFilterKind::Negative, .className = "org.opensourcephysics.media.core.NegativeFilter"},
    {.kind = VideoFilterKind::GrayScale, .className = "org.opensourcephysics.media.core.GrayScaleFilter"},
    {.kind = VideoFilterKind::Log, .className = "org.opensourcephysics.media.core.LogFilter"},
    {.kind = VideoFilterKind::Brightness, .className = "org.opensourcephysics.media.core.BrightnessFilter"},
    {.kind = VideoFilterKind::Baseline, .className = "org.opensourcephysics.media.core.BaselineFilter"},
    {.kind = VideoFilterKind::Sum, .className = "org.opensourcephysics.media.core.SumFilter"},
    {.kind = VideoFilterKind::Resize, .className = "org.opensourcephysics.media.core.ResizeFilter"},
    {.kind = VideoFilterKind::Rotate, .className = "org.opensourcephysics.media.core.RotateFilter"},
    {.kind = VideoFilterKind::Perspective, .className = "org.opensourcephysics.media.core.PerspectiveFilter"},
    {.kind = VideoFilterKind::RadialDistortion, .className = "org.opensourcephysics.media.core.RadialDistortionFilter"},
}};

std::atomic<std::uint64_t> g_NextIdentity{1};

} // namespace

std::span<const VideoFilterKind> trackerVideoFilterKinds() noexcept
{
    static const auto kinds = []
    {
        std::array<VideoFilterKind, KIND_NAMES.size()> result{};
        std::ranges::transform(KIND_NAMES, result.begin(), &KindName::kind);
        return result;
    }();
    return kinds;
}

std::string_view videoFilterClassName(VideoFilterKind kind) noexcept
{
    const auto entry = std::ranges::find(KIND_NAMES, kind, &KindName::kind);
    return entry == KIND_NAMES.end() ? std::string_view{} : entry->className;
}

std::optional<VideoFilterKind> videoFilterKindFromClassName(std::string_view className) noexcept
{
    const auto entry = std::ranges::find(KIND_NAMES, className, &KindName::className);
    if (entry == KIND_NAMES.end())
    {
        return std::nullopt;
    }
    return entry->kind;
}

bool isTemporalVideoFilter(VideoFilterKind kind) noexcept
{
    return kind == VideoFilterKind::Ghost || kind == VideoFilterKind::DarkGhost || kind == VideoFilterKind::Strobe ||
           kind == VideoFilterKind::Sum;
}

std::uint64_t newFilterImageIdentity() noexcept
{
    return g_NextIdentity.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// VideoFilter

void VideoFilter::setEnabled(bool enabled)
{
    m_Enabled = enabled;
}

void VideoFilter::clear()
{}

std::pair<int, int> VideoFilter::outputSize(int width, int height)
{
    return {width, height};
}

void VideoFilter::initialize(const FilterImage& /*input*/)
{}

const FilterImage& VideoFilter::apply(const FilterImage& input, std::uint64_t inputIdentity, int videoFrame)
{
    if (!m_Enabled || input.empty())
    {
        return input;
    }
    if (!m_Initialized || inputIdentity != m_SourceIdentity || input.width() != m_SourceWidth || input.height() != m_SourceHeight)
    {
        // OSP throws IllegalArgumentException from new BufferedImage for a zero dimension.
        const auto [width, height] = outputSize(input.width(), input.height());
        // Java cannot allocate an int raster above INT_MAX pixels either.
        if (width <= 0 || height <= 0 || static_cast<std::int64_t>(width) * height > std::numeric_limits<int>::max())
        {
            m_Initialized = false;
            return input;
        }
        m_Output = FilterImage(width, height);
        m_OutputIdentity = newFilterImageIdentity();
        m_SourceIdentity = inputIdentity;
        m_SourceWidth = input.width();
        m_SourceHeight = input.height();
        m_Initialized = true;
        initialize(input);
    }
    render(input, m_Output, videoFrame);
    return m_Output;
}

std::unique_ptr<VideoFilter> makeVideoFilter(VideoFilterKind kind)
{
    switch (kind)
    {
    case VideoFilterKind::Deinterlace:
        return std::make_unique<DeinterlaceFilter>();
    case VideoFilterKind::Ghost:
        return std::make_unique<GhostFilter>();
    case VideoFilterKind::Strobe:
        return std::make_unique<StrobeFilter>();
    case VideoFilterKind::DarkGhost:
        return std::make_unique<DarkGhostFilter>();
    case VideoFilterKind::Negative:
        return std::make_unique<NegativeFilter>();
    case VideoFilterKind::GrayScale:
        return std::make_unique<GrayScaleFilter>();
    case VideoFilterKind::Log:
        return std::make_unique<LogFilter>();
    case VideoFilterKind::Brightness:
        return std::make_unique<BrightnessFilter>();
    case VideoFilterKind::Baseline:
        return std::make_unique<BaselineFilter>();
    case VideoFilterKind::Sum:
        return std::make_unique<SumFilter>();
    case VideoFilterKind::Resize:
        return std::make_unique<ResizeFilter>();
    case VideoFilterKind::Rotate:
        return std::make_unique<RotateFilter>();
    case VideoFilterKind::Perspective:
        return std::make_unique<PerspectiveFilter>();
    case VideoFilterKind::RadialDistortion:
        return std::make_unique<RadialDistortionFilter>();
    }
    throw std::invalid_argument("Unknown video filter kind");
}

// ---------------------------------------------------------------------------------------------
// VideoFilterStack

VideoFilterStack::VideoFilterStack() : m_SourceIdentity(newFilterImageIdentity())
{}

VideoFilter& VideoFilterStack::at(std::size_t index)
{
    return *m_Filters.at(index);
}

const VideoFilter& VideoFilterStack::at(std::size_t index) const
{
    return *m_Filters.at(index);
}

VideoFilter& VideoFilterStack::add(std::unique_ptr<VideoFilter> filter)
{
    return insert(m_Filters.size(), std::move(filter));
}

VideoFilter& VideoFilterStack::insert(std::size_t index, std::unique_ptr<VideoFilter> filter)
{
    if (!filter)
    {
        throw std::invalid_argument("VideoFilterStack cannot hold a null filter");
    }
    const auto position = m_Filters.begin() + static_cast<std::ptrdiff_t>(std::min(index, m_Filters.size()));
    return **m_Filters.insert(position, std::move(filter));
}

std::unique_ptr<VideoFilter> VideoFilterStack::remove(std::size_t index)
{
    if (index >= m_Filters.size())
    {
        return nullptr;
    }
    auto filter = std::move(m_Filters[index]);
    m_Filters.erase(m_Filters.begin() + static_cast<std::ptrdiff_t>(index));
    return filter;
}

void VideoFilterStack::clear() noexcept
{
    m_Filters.clear();
}

void VideoFilterStack::stepChanged()
{
    for (const auto& filter : m_Filters)
    {
        if (auto* sum = dynamic_cast<SumFilter*>(filter.get()))
        {
            sum->addNextImage();
        }
    }
}

void VideoFilterStack::replaceSource() noexcept
{
    m_SourceIdentity = newFilterImageIdentity();
}

VideoFilterStack VideoFilterStack::clone() const
{
    VideoFilterStack copy;
    copy.m_SourceIdentity = m_SourceIdentity;
    copy.m_SourceWidth = m_SourceWidth;
    copy.m_SourceHeight = m_SourceHeight;
    copy.m_Filters.reserve(m_Filters.size());
    for (const auto& filter : m_Filters)
    {
        copy.m_Filters.push_back(filter->clone());
    }
    return copy;
}

const FilterImage& VideoFilterStack::apply(const FilterImage& frame, int videoFrame)
{
    // VideoAdapter.refreshBufferedImage allocates a new buffer only when the frame size changes.
    if (frame.width() != m_SourceWidth || frame.height() != m_SourceHeight)
    {
        m_SourceWidth = frame.width();
        m_SourceHeight = frame.height();
        replaceSource();
    }
    const FilterImage* current = &frame;
    std::uint64_t identity = m_SourceIdentity;
    for (const auto& filter : m_Filters)
    {
        if (!filter->isEnabled())
        {
            continue;
        }
        const FilterImage& output = filter->apply(*current, identity, videoFrame);
        if (&output != current)
        {
            identity = filter->outputIdentity();
            current = &output;
        }
    }
    return *current;
}

} // namespace motrack::science
