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
#include <utility>

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

std::optional<CalibrationKey> calibrationAt(const Calibration& calibration, int frame)
{
    const auto& keys = calibration.keyframes;
    const auto next = std::ranges::upper_bound(keys, frame, {}, &CalibrationKey::frame);
    if (next == keys.begin())
    {
        return std::nullopt;
    }
    const auto& active = *std::prev(next);
    const auto& origin = calibration.fixedOrigin ? keys.back() : active;
    const auto& rotation = calibration.fixedAngle ? keys.back() : active;
    const auto& scale = calibration.fixedScale ? keys.front() : active;
    return CalibrationKey{.frame = frame,
                          .originX = origin.originX,
                          .originY = origin.originY,
                          .angleDegrees = rotation.angleDegrees,
                          .scaleX = scale.scaleX,
                          .scaleY = scale.scaleY};
}

std::optional<WorldPoint> worldPosition(const Calibration& calibration, const Mark& mark)
{
    const auto key = calibrationAt(calibration, mark.frame);
    if (!key || key->scaleX == 0.0 || key->scaleY == 0.0)
    {
        return std::nullopt;
    }

    // Invert OSP's world-to-image transform, including the downward image Y axis.
    const auto angle = key->angleDegrees * (std::numbers::pi / 180.0);
    const auto cosAngle = std::cos(angle);
    const auto sinAngle = std::sin(angle);
    const auto x = (mark.x - key->originX) / key->scaleX;
    const auto y = (mark.y - key->originY) / key->scaleY;
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

PlotSeries measurementPlot(const LegacyProject& project, std::size_t track, PlotQuantity quantity, std::span<const double> frameTimes)
{
    const auto frameDurationMs = project.clip.frameDurationMs.value_or(0.0);
    PlotSeries series{.samples = {}, .timed = !frameTimes.empty() || frameDurationMs > 0.0, .calibrated = project.calibration.has_value()};
    const bool velocity = quantity == PlotQuantity::VelocityX || quantity == PlotQuantity::VelocityY;
    if (track >= project.pointMasses.size() || (velocity && !series.timed))
    {
        return series;
    }

    const bool vertical = quantity == PlotQuantity::PositionY || quantity == PlotQuantity::VelocityY;
    for (const auto& mark : project.pointMasses.at(track).marks)
    {
        if (!includesFrame(project.clip, mark.frame))
        {
            continue;
        }
        const auto point =
            project.calibration ? worldPosition(*project.calibration, mark) : std::optional(WorldPoint{.x = mark.x, .y = mark.y});
        double horizontal = mark.frame;
        if (!frameTimes.empty())
        {
            if (mark.frame < 0 || static_cast<std::size_t>(mark.frame) >= frameTimes.size())
            {
                continue;
            }
            horizontal = frameTimes[static_cast<std::size_t>(mark.frame)];
        }
        else if (series.timed)
        {
            horizontal = (project.clip.startTimeMs + (mark.frame - project.clip.firstFrame) * frameDurationMs) / 1000.0;
        }
        if (point && std::isfinite(horizontal))
        {
            series.samples.push_back({.frame = mark.frame, .horizontal = horizontal, .vertical = vertical ? point->y : point->x});
        }
    }

    if (velocity)
    {
        const auto positions = std::exchange(series.samples, {});
        for (std::size_t index = 1; index + 1 < positions.size(); ++index)
        {
            const auto& previous = positions.at(index - 1);
            const auto& center = positions.at(index);
            const auto& next = positions.at(index + 1);
            const auto before = center.horizontal - previous.horizontal;
            const auto after = next.horizontal - center.horizontal;
            if (center.frame - previous.frame == project.clip.stepSize && next.frame - center.frame == project.clip.stepSize &&
                before > 0.0 && after > 0.0)
            {
                const auto elapsed = before + after;
                const auto value = (after / elapsed) * (center.vertical - previous.vertical) / before +
                                   (before / elapsed) * (next.vertical - center.vertical) / after;
                if (std::isfinite(value))
                {
                    series.samples.push_back({.frame = center.frame, .horizontal = center.horizontal, .vertical = value});
                }
            }
        }
    }
    return series;
}

} // namespace motrack
