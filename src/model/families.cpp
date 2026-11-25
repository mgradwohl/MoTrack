#include "model/families.hpp"

#include <cmath>
#include <limits>

namespace motrack::model
{

std::string_view familyName(Family family) noexcept
{
    switch (family)
    {
    case Family::Panel:
        return "panel";
    case Family::Clip:
        return "clip";
    case Family::Video:
        return "video";
    case Family::ClipControl:
        return "clip-control";
    case Family::Coordinates:
        return "coordinates";
    case Family::PointMass:
        return "point-mass";
    case Family::Vector:
        return "vector";
    case Family::Unknown:
        break;
    }
    return "unknown";
}

std::string_view roleName(Role role) noexcept
{
    switch (role)
    {
    case Role::Panel:
        return "panel";
    case Role::Clip:
        return "videoclip";
    case Role::Video:
        return "video";
    case Role::ClipControl:
        return "clipcontrol";
    case Role::Coordinates:
        return "coords";
    case Role::Track:
        break;
    }
    return "track";
}

std::optional<double> finiteValue(const Read<double>& value)
{
    return value && std::isfinite(*value) ? std::optional(*value) : std::nullopt;
}

double loadedMass(const PointMassState& state)
{
    return std::max(MINIMUM_POINT_MASS, std::abs(finiteValue(state.mass).value_or(1.0)));
}

std::vector<std::int32_t> loadedKeyFrames(const PointMassState& state)
{
    std::vector<std::int32_t> keys;
    if (state.keyFrames && !state.keyFrames->empty())
    {
        keys = *state.keyFrames;
        std::ranges::sort(keys);
        const auto [first, last] = std::ranges::unique(keys);
        keys.erase(first, last);
    }
    else
    {
        state.marks.forEach([&](std::int32_t frame, const FrameElement<Mark>& /*mark*/) { keys.push_back(frame); });
    }
    return keys;
}

std::int32_t clipFrameShift(const Read<std::int32_t>& frameShift)
{
    return frameShift && *frameShift != std::numeric_limits<std::int32_t>::min() ? *frameShift : 0;
}

std::optional<std::int32_t> loadedStartFrame(const ClipState& clip)
{
    if (!clip.startFrame)
    {
        return std::nullopt;
    }
    const auto start = static_cast<std::int32_t>(static_cast<std::uint32_t>(*clip.startFrame) -
                                                 static_cast<std::uint32_t>(clipFrameShift(clip.frameShift)));
    return std::max<std::int32_t>(start, 0);
}

std::int32_t trackFrameShift(const ClipState& clip)
{
    return std::max(clipFrameShift(clip.frameShift), 0);
}

std::string loadedName(const Read<std::string>& name)
{
    return name ? *name : std::string{};
}

bool supportsConstantClipTimingEdits(std::span<const std::string> mediaReferences, std::span<const std::string> unknownTrackClasses)
{
    return mediaReferences.empty() &&
           std::ranges::all_of(
               unknownTrackClasses,
               [](const std::string& type)
               { return type == tracker::COORD_AXES_CLASS || type == "org.opensourcephysics.cabrillo.tracker.Protractor"; });
}

bool validConstantClipTiming(const ClipTimeline& clip)
{
    if (clip.media || !clip.frameDurationMs || !std::isfinite(*clip.frameDurationMs) || *clip.frameDurationMs <= 0.0 ||
        !std::isfinite(clip.startTimeMs) || clip.firstFrame < 0 || clip.stepSize <= 0 || !clip.stepCount || *clip.stepCount <= 0)
    {
        return false;
    }
    const auto duration = *clip.frameDurationMs;
    const auto span = static_cast<std::int64_t>(*clip.stepCount - 1) * clip.stepSize;
    const auto end = static_cast<std::int64_t>(clip.firstFrame) + span;
    // Tracker's loader also computes first + count * stride in a Java int, even for one step.
    if (end > 500000 || end + clip.stepSize > std::numeric_limits<std::int32_t>::max() ||
        (clip.sourceFrameCount && end >= *clip.sourceFrameCount))
    {
        return false;
    }
    double previous = clip.startTimeMs;
    for (std::int64_t offset = 1; offset <= std::max<std::int64_t>(1, span); ++offset)
    {
        const auto time = clip.startTimeMs + (static_cast<double>(offset) * duration);
        if (!std::isfinite(time) || time <= previous || time / 1000.0 <= previous / 1000.0)
        {
            return false;
        }
        // Tracker consumers can subtract absolute timestamps. Monotonicity alone permits large
        // origins to materially distort their interval; bound rounding in both saved/display units.
        const auto durationSeconds = duration / 1000.0;
        if (std::abs((time - previous) - duration) > duration * 1e-9 ||
            std::abs((time / 1000.0 - previous / 1000.0) - durationSeconds) > durationSeconds * 1e-9)
        {
            return false;
        }
        previous = time;
    }
    return true;
}

} // namespace motrack::model
