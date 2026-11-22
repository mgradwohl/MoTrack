#include "model/families.hpp"

#include <cmath>

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

} // namespace motrack::model
