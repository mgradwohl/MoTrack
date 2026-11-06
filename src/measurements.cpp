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

std::optional<double> pointDistance(WorldPoint first, WorldPoint second)
{
    const auto distance = std::hypot(second.x - first.x, second.y - first.y);
    return std::isfinite(distance) ? std::optional(distance) : std::nullopt;
}

std::optional<double> interiorAngleDegrees(WorldPoint vertex, WorldPoint first, WorldPoint second)
{
    const auto firstLength = pointDistance(vertex, first);
    const auto secondLength = pointDistance(vertex, second);
    if (!firstLength || !secondLength || *firstLength == 0.0 || *secondLength == 0.0)
    {
        return std::nullopt;
    }
    const auto ax = (first.x - vertex.x) / *firstLength;
    const auto ay = (first.y - vertex.y) / *firstLength;
    const auto bx = (second.x - vertex.x) / *secondLength;
    const auto by = (second.y - vertex.y) / *secondLength;
    return std::atan2(std::abs(ax * by - ay * bx), ax * bx + ay * by) * (180.0 / std::numbers::pi);
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

PlotSeries measurementPlot(const LegacyProject& project,
                           std::size_t track,
                           PlotQuantity quantity,
                           std::span<const double> frameTimes,
                           double presentationOffsetSeconds)
{
    struct PositionSample
    {
        int frame;
        double horizontal;
        WorldPoint point;
    };

    const auto frameDurationMs = project.clip.frameDurationMs.value_or(0.0);
    const auto displayOffset = frameTimes.empty() ? 0.0 : presentationOffsetSeconds;
    PlotSeries series{.samples = {}, .timed = !frameTimes.empty() || frameDurationMs > 0.0, .calibrated = project.calibration.has_value()};
    const bool position = quantity == PlotQuantity::PositionX || quantity == PlotQuantity::PositionY;
    if (track >= project.pointMasses.size() || (!position && !series.timed))
    {
        return series;
    }

    const bool vertical = quantity == PlotQuantity::PositionY || quantity == PlotQuantity::VelocityY ||
                          quantity == PlotQuantity::AccelerationY || quantity == PlotQuantity::MomentumY;
    const auto& mass = project.pointMasses.at(track);
    std::vector<PositionSample> positions;
    for (const auto& mark : mass.marks)
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
        if (!point || !std::isfinite(horizontal + displayOffset) || !std::isfinite(point->x) || !std::isfinite(point->y))
        {
            continue;
        }
        if (position)
        {
            series.samples.push_back(
                {.frame = mark.frame, .horizontal = horizontal + displayOffset, .vertical = vertical ? point->y : point->x});
        }
        else
        {
            positions.push_back({.frame = mark.frame, .horizontal = horizontal, .point = *point});
        }
    }

    const bool acceleration = quantity == PlotQuantity::AccelerationX || quantity == PlotQuantity::AccelerationY;
    for (std::size_t index = 1; index + 1 < positions.size(); ++index)
    {
        const auto& previous = positions.at(index - 1);
        const auto& center = positions.at(index);
        const auto& next = positions.at(index + 1);
        const auto before = center.horizontal - previous.horizontal;
        const auto after = next.horizontal - center.horizontal;
        if (center.frame - previous.frame != project.clip.stepSize || next.frame - center.frame != project.clip.stepSize || before <= 0.0 ||
            after <= 0.0)
        {
            continue;
        }
        // Differentiate the quadratic through the three samples at the center time.
        const auto derivative = [&](double WorldPoint::*component)
        {
            const auto firstSlope = (center.point.*component - previous.point.*component) / before;
            const auto secondSlope = (next.point.*component - center.point.*component) / after;
            const auto elapsed = before + after;
            return acceleration ? 2.0 * (secondSlope - firstSlope) / elapsed
                                : (after / elapsed) * firstSlope + (before / elapsed) * secondSlope;
        };
        const bool useSpeed = quantity == PlotQuantity::Speed || quantity == PlotQuantity::KineticEnergy;
        auto value = useSpeed ? std::hypot(derivative(&WorldPoint::x), derivative(&WorldPoint::y))
                              : derivative(vertical ? &WorldPoint::y : &WorldPoint::x);
        if (quantity == PlotQuantity::KineticEnergy)
        {
            value = 0.5 * mass.mass * value * value;
        }
        else if (quantity == PlotQuantity::MomentumX || quantity == PlotQuantity::MomentumY)
        {
            value *= mass.mass;
        }
        if (std::isfinite(value))
        {
            series.samples.push_back({.frame = center.frame, .horizontal = center.horizontal + displayOffset, .vertical = value});
        }
    }
    return series;
}

std::optional<LinearFit> fitStraightLine(std::span<const PlotSample> samples)
{
    std::size_t count = 0;
    double meanX = 0.0;
    double meanY = 0.0;
    double varianceX = 0.0;
    double varianceY = 0.0;
    double covariance = 0.0;
    // Update the centered sums directly to preserve precision when coordinates have a large offset.
    for (const auto& sample : samples)
    {
        if (!std::isfinite(sample.horizontal) || !std::isfinite(sample.vertical))
        {
            continue;
        }
        const auto deltaX = sample.horizontal - meanX;
        const auto deltaY = sample.vertical - meanY;
        const auto size = static_cast<double>(++count);
        meanX += deltaX / size;
        meanY += deltaY / size;
        varianceX += deltaX * (sample.horizontal - meanX);
        varianceY += deltaY * (sample.vertical - meanY);
        covariance += deltaX * (sample.vertical - meanY);
    }
    if (count < 2 || varianceX <= 0.0 || !std::isfinite(varianceX) || !std::isfinite(varianceY) || !std::isfinite(covariance))
    {
        return std::nullopt;
    }
    const auto slope = covariance / varianceX;
    const auto intercept = std::fma(-slope, meanX, meanY);
    if (!std::isfinite(slope) || !std::isfinite(intercept))
    {
        return std::nullopt;
    }
    std::optional<double> rSquared;
    if (varianceY > 0.0)
    {
        const auto correlation = covariance / std::sqrt(varianceX) / std::sqrt(varianceY);
        rSquared = std::clamp(correlation * correlation, 0.0, 1.0);
    }
    return LinearFit{.slope = slope, .intercept = intercept, .rSquared = rSquared};
}

} // namespace motrack
