#include "measurements.hpp"

#include "model/families.hpp"
#include "science/motion/motion_derivatives.hpp"
#include "science/motion/motion_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <numbers>
#include <ranges>
#include <sstream>
#include <string_view>
#include <utility>

namespace motrack
{
bool supportsConstantClipTimingEdits(const LegacyProject& project)
{
    return model::supportsConstantClipTimingEdits(project.clip.mediaReferences, project.unsupportedTrackTypes);
}

bool validConstantClipTiming(const Clip& clip)
{
    if (!clip.mediaReferences.empty() || !clip.frameDurationMs || !std::isfinite(*clip.frameDurationMs) || *clip.frameDurationMs <= 0.0 ||
        !std::isfinite(clip.startTimeMs) || clip.firstFrame < 0 || clip.stepSize <= 0 || !clip.stepCount || *clip.stepCount <= 0)
    {
        return false;
    }
    const auto span = static_cast<std::int64_t>(*clip.stepCount - 1) * clip.stepSize;
    const auto end = static_cast<std::int64_t>(clip.firstFrame) + span;
    // Tracker's loader also computes first + count * stride in a Java int, even for one step.
    if (end > 500000 || end + clip.stepSize > std::numeric_limits<int>::max() || (clip.sourceFrameCount && end >= *clip.sourceFrameCount))
    {
        return false;
    }
    double previous = clip.startTimeMs;
    for (std::int64_t offset = 1; offset <= std::max<std::int64_t>(1, span); ++offset)
    {
        const auto time = clip.startTimeMs + static_cast<double>(offset) * *clip.frameDurationMs;
        if (!std::isfinite(time) || time <= previous || time / 1000.0 <= previous / 1000.0)
        {
            return false;
        }
        // Tracker consumers can subtract absolute timestamps. Monotonicity alone permits large
        // origins to materially distort their interval; bound rounding in both saved/display units.
        const auto durationSeconds = *clip.frameDurationMs / 1000.0;
        if (std::abs((time - previous) - *clip.frameDurationMs) > *clip.frameDurationMs * 1e-9 ||
            std::abs((time / 1000.0 - previous / 1000.0) - durationSeconds) > durationSeconds * 1e-9)
        {
            return false;
        }
        previous = time;
    }
    return true;
}

std::optional<int> clipStepOfFrame(const Clip& clip, int frame)
{
    if (clip.firstFrame < 0 || frame < clip.firstFrame || clip.stepSize < 1)
    {
        return std::nullopt;
    }
    const auto offset = frame - clip.firstFrame;
    const auto step = offset / clip.stepSize;
    if (offset % clip.stepSize != 0 || (clip.stepCount && step >= *clip.stepCount))
    {
        return std::nullopt;
    }
    return step;
}

std::optional<int> clipFrameOfStep(const Clip& clip, int step)
{
    if (clip.firstFrame < 0 || clip.stepSize < 1 || step < 0 || (clip.stepCount && step >= *clip.stepCount))
    {
        return std::nullopt;
    }
    const auto frame = std::int64_t{clip.firstFrame} + std::int64_t{step} * clip.stepSize;
    if (!std::in_range<int>(frame))
    {
        return std::nullopt;
    }
    return static_cast<int>(frame);
}

std::optional<double> clipFrameTimeMs(const Clip& clip, int frame)
{
    const auto duration = clip.frameDurationMs.value_or(0.0);
    if (!(duration > 0.0))
    {
        return std::nullopt;
    }
    return clip.startTimeMs + static_cast<double>(std::int64_t{frame} - clip.firstFrame) * duration;
}

namespace
{

bool includesFrame(const Clip& clip, int frame)
{
    return clipStepOfFrame(clip, frame).has_value();
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
                           double presentationOffsetSeconds,
                           FrameTimeBase frameTimeBase)
{
    const auto frameDurationMs = project.clip.frameDurationMs.value_or(0.0);
    const auto displayOffset = frameTimes.empty() ? 0.0 : presentationOffsetSeconds;
    PlotSeries series{.samples = {},
                      .timed = !frameTimes.empty() || frameDurationMs > 0.0,
                      .calibrated = project.calibration.has_value(),
                      .timeUnit = project.timeUnit};
    const bool position = quantity == PlotQuantity::PositionX || quantity == PlotQuantity::PositionY;
    if (track >= project.pointMasses.size() || (!position && !series.timed))
    {
        return series;
    }

    const bool vertical = quantity == PlotQuantity::PositionY || quantity == PlotQuantity::VelocityY ||
                          quantity == PlotQuantity::AccelerationY || quantity == PlotQuantity::MomentumY;
    const auto& mass = project.pointMasses.at(track);
    // The plotted time of an original frame in seconds, before the presentation offset: the decoded frame
    // time (NaN beyond the decoded frames), the saved constant timing, or the frame number when untimed.
    const auto frameTime = [&](int frame)
    {
        if (!frameTimes.empty())
        {
            return frame >= 0 && std::cmp_less(frame, frameTimes.size()) ? frameTimes[static_cast<std::size_t>(frame)]
                                                                         : std::numeric_limits<double>::quiet_NaN();
        }
        const auto time = clipFrameTimeMs(project.clip, frame);
        return time ? *time / 1000.0 : static_cast<double>(frame);
    };
    std::vector<science::FramePosition> marks;
    for (const auto& mark : mass.marks)
    {
        if (!includesFrame(project.clip, mark.frame))
        {
            continue;
        }
        const auto point =
            project.calibration ? worldPosition(*project.calibration, mark) : std::optional(WorldPoint{.x = mark.x, .y = mark.y});
        const auto horizontal = frameTime(mark.frame);
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
            marks.push_back({.frame = mark.frame, .position = {.x = point->x, .y = point->y}});
        }
    }
    if (position)
    {
        return series;
    }

    // Tracker's default algorithm (Finite Difference: velocity spill 1, acceleration spill 2) on the clip
    // steps, as PointMass.refreshData computes it (docs/motion-kernels.md). A derivative needs every step of
    // its window, so each run of consecutive marked steps is evaluated on its own; a later mark on a frame
    // replaces an earlier one, like Tracker's step array.
    std::ranges::stable_sort(marks, {}, &science::FramePosition::frame);
    std::vector<science::FramePosition> steps;
    for (const auto& mark : marks)
    {
        if (!steps.empty() && steps.back().frame == mark.frame)
        {
            steps.back() = mark;
        }
        else
        {
            steps.push_back(mark);
        }
    }
    const bool acceleration = quantity == PlotQuantity::AccelerationX || quantity == PlotQuantity::AccelerationY;
    const auto stride = project.clip.stepSize;
    for (auto begin = steps.begin(); begin != steps.end();)
    {
        auto end = std::next(begin);
        while (end != steps.end() && end->frame - std::prev(end)->frame == stride)
        {
            ++end;
        }
        std::vector<std::optional<science::PlanarValue>> positions;
        std::vector<double> times;
        for (const auto& mark : std::ranges::subrange(begin, end))
        {
            positions.emplace_back(mark.position);
            // Tracker's media-free step time is (k * duration * stride + start) / 1000 for clip step k, in that
            // operand order (StepperClipControl.getStepTime, VideoPlayer.getStepTime). Omitting the start time
            // shifts every step alike, so intervals are unchanged (bit for bit when the start is 0) and a large
            // time origin cannot distort them by rounding. Decoded frame times are used as they are.
            const auto clipStep = (std::int64_t{mark.frame} - project.clip.firstFrame) / stride;
            times.push_back(frameTimes.empty() ? static_cast<double>(clipStep) * frameDurationMs * static_cast<double>(stride) / 1000.0
                                               : frameTime(mark.frame));
        }
        const science::ClipLattice lattice{.firstFrame = begin->frame, .stepSize = stride, .stepCount = static_cast<int>(positions.size())};
        // Constant timing is evenly spaced by definition: Tracker's values. Decoded frame times get Tracker's
        // values wherever a window is evenly spaced within the timestamps' precision (the stream's time base, or
        // 1 ms for whole-millisecond timestamps in finer ticks), and a time-aware five-point fit elsewhere (#87,
        // #331, #335; docs/motion-kernels.md).
        const auto motion = frameTimes.empty()
                                ? science::pointMassDerivatives(science::TRACKER_DEFAULT_DERIVATIVE_ALGORITHM, lattice, positions, times)
                                : science::measuredTimeDerivatives(lattice, positions, times, frameTimeBase.resolutionSeconds());
        for (std::size_t step = 0; step < positions.size(); ++step)
        {
            const auto& derivative = acceleration ? motion.acceleration[step] : motion.velocity[step];
            if (!derivative)
            {
                continue;
            }
            const bool speed = quantity == PlotQuantity::Speed || quantity == PlotQuantity::KineticEnergy;
            auto value = speed ? std::hypot(derivative->x, derivative->y) : (vertical ? derivative->y : derivative->x);
            if (quantity == PlotQuantity::KineticEnergy)
            {
                value = 0.5 * mass.mass * value * value;
            }
            else if (quantity == PlotQuantity::MomentumX || quantity == PlotQuantity::MomentumY)
            {
                value *= mass.mass;
            }
            const auto frame = lattice.frame(static_cast<int>(step));
            if (std::isfinite(value))
            {
                series.samples.push_back({.frame = frame, .horizontal = frameTime(frame) + displayOffset, .vertical = value});
            }
        }
        begin = end;
    }
    return series;
}

std::string plotCsv(const PlotSeries& series, std::string_view quantity)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "frame,";
    if (series.timed)
    {
        writeCsvField(output, "time_" + series.timeUnit);
        output << ',';
    }
    writeCsvField(output, quantity);
    output << "\r\n";
    for (const auto& sample : series.samples)
    {
        output << sample.frame << ',';
        if (series.timed)
        {
            output << sample.horizontal << ',';
        }
        output << sample.vertical << "\r\n";
    }
    return output.str();
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
