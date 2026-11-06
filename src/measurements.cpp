#include "measurements.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <numbers>
#include <sstream>
#include <string_view>

namespace motrack
{
namespace
{

bool includesFrame(const Clip& clip, int frame)
{
    if (clip.firstFrame < 0 || frame < clip.firstFrame || clip.stepSize < 1)
    {
        return false;
    }
    const auto offset = frame - clip.firstFrame;
    return offset % clip.stepSize == 0 && (!clip.stepCount || offset / clip.stepSize < *clip.stepCount);
}

void writeCsvField(std::ostream& output, std::string_view value)
{
    if (value.find_first_of(",\"\r\n") == std::string_view::npos)
    {
        output << value;
        return;
    }
    output << '"';
    for (const char character : value)
    {
        if (character == '"')
        {
            output << '"';
        }
        output << character;
    }
    output << '"';
}

} // namespace

std::optional<WorldPoint> worldPosition(const Calibration& calibration, const Mark& mark)
{
    const auto& keys = calibration.keyframes;
    const auto next = std::ranges::upper_bound(keys, mark.frame, {}, &CalibrationKey::frame);
    if (next == keys.begin())
    {
        return std::nullopt;
    }
    const auto& active = *std::prev(next);
    const auto& origin = calibration.fixedOrigin ? keys.back() : active;
    const auto& rotation = calibration.fixedAngle ? keys.back() : active;
    const auto& scale = calibration.fixedScale ? keys.front() : active;
    if (scale.scaleX == 0.0 || scale.scaleY == 0.0)
    {
        return std::nullopt;
    }

    // Invert OSP's world-to-image transform, including the downward image Y axis.
    const auto angle = rotation.angleDegrees * (std::numbers::pi / 180.0);
    const auto cosAngle = std::cos(angle);
    const auto sinAngle = std::sin(angle);
    const auto x = (mark.x - origin.originX) / scale.scaleX;
    const auto y = (mark.y - origin.originY) / scale.scaleY;
    const WorldPoint point{.x = cosAngle * x - sinAngle * y, .y = -sinAngle * x - cosAngle * y};
    if (!std::isfinite(point.x) || !std::isfinite(point.y))
    {
        return std::nullopt;
    }
    return point;
}

std::string measurementsCsv(const LegacyProject& project)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "track,frame,image_x_px,image_y_px,world_x,world_y,length_unit\r\n";
    for (const auto& track : project.pointMasses)
    {
        for (const auto& mark : track.marks)
        {
            if (!includesFrame(project.clip, mark.frame))
            {
                continue;
            }
            writeCsvField(output, track.name);
            output << ',' << mark.frame << ',' << mark.x << ',' << mark.y << ',';
            const auto point = project.calibration ? worldPosition(*project.calibration, mark) : std::nullopt;
            if (point)
            {
                output << point->x << ',' << point->y << ',';
                writeCsvField(output, project.lengthUnit);
            }
            else
            {
                output << ",,";
            }
            output << "\r\n";
        }
    }
    return output.str();
}

} // namespace motrack
