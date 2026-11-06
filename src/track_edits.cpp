#include "track_edits.hpp"

#include "measurements.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>

namespace motrack
{
namespace
{

Calibration initialCalibration(double x, double y)
{
    return {.keyframes = {{.frame = 0, .originX = x, .originY = y, .angleDegrees = 0.0, .scaleX = 1.0, .scaleY = 1.0}},
            .fixedOrigin = true,
            .fixedAngle = true,
            .fixedScale = true};
}

void writeCalibrationKey(Calibration& calibration, const CalibrationKey& key)
{
    const auto position = std::ranges::lower_bound(calibration.keyframes, key.frame, {}, &CalibrationKey::frame);
    if (position != calibration.keyframes.end() && position->frame == key.frame)
    {
        *position = key;
    }
    else
    {
        calibration.keyframes.insert(position, key);
    }
}

} // namespace

ClipRange clipRange(const Clip& clip)
{
    return {.firstFrame = clip.firstFrame, .stepSize = clip.stepSize, .stepCount = clip.stepCount};
}

void TrackEdits::clear()
{
    m_Commands.clear();
    m_Head = 0;
    markSaved();
}

void TrackEdits::markSaved()
{
    m_SavedHead = m_Head;
}

bool TrackEdits::isModified() const
{
    return m_SavedHead != m_Head;
}

void TrackEdits::createPointMass(LegacyProject& project, std::string name)
{
    append(project, AddTrack{.name = std::move(name)});
}

bool TrackEdits::renamePointMass(LegacyProject& project, std::size_t track, std::string name)
{
    if (track >= project.pointMasses.size() || name.empty() ||
        std::ranges::any_of(project.pointMasses, [&name](const auto& mass) { return mass.name == name; }))
    {
        return false;
    }
    append(project, RenameTrack{.track = track, .after = std::move(name), .before = project.pointMasses.at(track).name});
    return true;
}

bool TrackEdits::removePointMass(LegacyProject& project, std::size_t track)
{
    if (track >= project.pointMasses.size())
    {
        return false;
    }
    append(project, RemoveTrack{.track = track, .before = project.pointMasses.at(track)});
    return true;
}

bool TrackEdits::setPointMassValue(LegacyProject& project, std::size_t track, double mass)
{
    if (track >= project.pointMasses.size() || !std::isfinite(mass) || mass < MIN_POINT_MASS)
    {
        return false;
    }
    const auto before = project.pointMasses.at(track).mass;
    if (mass != before)
    {
        append(project, ChangeMass{.track = track, .after = mass, .before = before});
    }
    return true;
}

bool TrackEdits::markPointMass(LegacyProject& project, std::size_t track, Mark mark)
{
    if (track >= project.pointMasses.size() || mark.frame < 0 || !std::isfinite(mark.x) || !std::isfinite(mark.y))
    {
        return false;
    }
    const auto& marks = project.pointMasses.at(track).marks;
    const auto previous = std::ranges::lower_bound(marks, mark.frame, {}, &Mark::frame);
    const auto before = previous != marks.end() && previous->frame == mark.frame ? std::optional(*previous) : std::nullopt;
    if (!before || before->x != mark.x || before->y != mark.y)
    {
        append(project, ChangeMark{.track = track, .frame = mark.frame, .after = mark, .before = before});
    }
    return true;
}

bool TrackEdits::removeMark(LegacyProject& project, std::size_t track, int frame)
{
    if (track >= project.pointMasses.size())
    {
        return false;
    }
    const auto& marks = project.pointMasses.at(track).marks;
    const auto mark = std::ranges::lower_bound(marks, frame, {}, &Mark::frame);
    if (mark == marks.end() || mark->frame != frame)
    {
        return false;
    }
    append(project, ChangeMark{.track = track, .frame = frame, .after = std::nullopt, .before = *mark});
    return true;
}

bool TrackEdits::calibrateDistance(LegacyProject& project, CalibrationDistance distance)
{
    if (distance.frame < 0 || !std::isfinite(distance.knownLength) || distance.knownLength <= 0.0)
    {
        return false;
    }
    auto next = project.calibration.value_or(Calibration{});
    if (next.keyframes.empty())
    {
        if (!std::isfinite(distance.defaultOriginX) || !std::isfinite(distance.defaultOriginY))
        {
            return false;
        }
        next = initialCalibration(distance.defaultOriginX, distance.defaultOriginY);
    }
    auto key = calibrationAt(next, distance.frame);
    if (!key || key->scaleX == 0.0 || key->scaleY == 0.0)
    {
        return false;
    }
    const auto factor = std::hypot(distance.imageDeltaX / key->scaleX, distance.imageDeltaY / key->scaleY) / distance.knownLength;
    key->scaleX *= factor;
    key->scaleY *= factor;
    if (!std::isfinite(key->scaleX) || !std::isfinite(key->scaleY) || key->scaleX == 0.0 || key->scaleY == 0.0)
    {
        return false;
    }

    if (next.fixedScale)
    {
        for (auto& entry : next.keyframes)
        {
            entry.scaleX = key->scaleX;
            entry.scaleY = key->scaleY;
        }
    }
    else
    {
        writeCalibrationKey(next, *key);
    }
    append(project,
           ChangeCalibration{.after = std::move(next),
                             .before = project.calibration,
                             .afterUnit = std::move(distance.unit),
                             .beforeUnit = project.lengthUnit});
    return true;
}

bool TrackEdits::setCoordinateAxes(LegacyProject& project, AxesPlacement axes)
{
    if (axes.frame < 0 || !std::isfinite(axes.originX) || !std::isfinite(axes.originY))
    {
        return false;
    }
    auto next = project.calibration.value_or(Calibration{});
    const bool initial = next.keyframes.empty();
    if (initial)
    {
        next = initialCalibration(axes.originX, axes.originY);
    }
    auto key = calibrationAt(next, axes.frame);
    if (!key || key->scaleX == 0.0 || key->scaleY == 0.0)
    {
        return false;
    }
    const auto dx = (axes.positiveX - axes.originX) / key->scaleX;
    const auto dy = (axes.originY - axes.positiveY) / key->scaleY;
    if (!std::isfinite(dx) || !std::isfinite(dy) || (dx == 0.0 && dy == 0.0))
    {
        return false;
    }
    key->originX = axes.originX;
    key->originY = axes.originY;
    key->angleDegrees = std::atan2(dy, dx) * (180.0 / std::numbers::pi);
    if (!next.fixedOrigin || !next.fixedAngle)
    {
        writeCalibrationKey(next, *key);
    }
    if (next.fixedOrigin)
    {
        next.keyframes.back().originX = key->originX;
        next.keyframes.back().originY = key->originY;
    }
    if (next.fixedAngle)
    {
        next.keyframes.back().angleDegrees = key->angleDegrees;
    }
    append(project,
           ChangeCalibration{.after = std::move(next),
                             .before = project.calibration,
                             .afterUnit = initial ? "px" : project.lengthUnit,
                             .beforeUnit = project.lengthUnit});
    return true;
}

bool TrackEdits::canUndo() const
{
    return m_Head != 0;
}

bool TrackEdits::setClipRange(LegacyProject& project, int firstFrame, int lastFrame, int stepSize)
{
    if (firstFrame < 0 || lastFrame < firstFrame || stepSize < 1 ||
        (project.clip.sourceFrameCount && lastFrame >= project.clip.sourceFrameCount.value()))
    {
        return false;
    }
    const auto count = 1 + (static_cast<std::int64_t>(lastFrame) - firstFrame) / stepSize;
    if (!std::in_range<int>(count))
    {
        return false;
    }
    const ClipRange after{.firstFrame = firstFrame, .stepSize = stepSize, .stepCount = static_cast<int>(count)};
    const auto before = clipRange(project.clip);
    if (after == before)
    {
        return false;
    }
    append(project, ChangeClip{.after = after, .before = before});
    return true;
}

bool TrackEdits::canRedo() const
{
    return m_Head < m_Commands.size();
}

bool TrackEdits::undo(LegacyProject& project)
{
    if (!canUndo())
    {
        return false;
    }
    apply(project, m_Commands.at(m_Head - 1), false);
    --m_Head;
    return true;
}

bool TrackEdits::redo(LegacyProject& project)
{
    if (!canRedo())
    {
        return false;
    }
    apply(project, m_Commands.at(m_Head), true);
    ++m_Head;
    return true;
}

void TrackEdits::apply(LegacyProject& project, const Edit& edit, bool forward)
{
    if (const auto* track = std::get_if<AddTrack>(&edit))
    {
        if (forward)
        {
            project.pointMasses.push_back({.name = track->name, .marks = {}});
        }
        else
        {
            project.pointMasses.pop_back();
        }
        return;
    }
    if (const auto* rename = std::get_if<RenameTrack>(&edit))
    {
        project.pointMasses.at(rename->track).name = forward ? rename->after : rename->before;
        return;
    }
    if (const auto* remove = std::get_if<RemoveTrack>(&edit))
    {
        const auto position = project.pointMasses.begin() + static_cast<std::ptrdiff_t>(remove->track);
        if (forward)
        {
            project.pointMasses.erase(position);
        }
        else
        {
            project.pointMasses.insert(position, remove->before);
        }
        return;
    }
    if (const auto* mass = std::get_if<ChangeMass>(&edit))
    {
        project.pointMasses.at(mass->track).mass = forward ? mass->after : mass->before;
        return;
    }
    if (const auto* change = std::get_if<ChangeCalibration>(&edit))
    {
        auto calibration = forward ? std::optional(change->after) : change->before;
        auto unit = forward ? change->afterUnit : change->beforeUnit;
        project.calibration = std::move(calibration);
        project.lengthUnit = std::move(unit);
        return;
    }
    if (const auto* change = std::get_if<ChangeClip>(&edit))
    {
        const auto& range = forward ? change->after : change->before;
        project.clip.firstFrame = range.firstFrame;
        project.clip.stepSize = range.stepSize;
        project.clip.stepCount = range.stepCount;
        return;
    }

    const auto& change = std::get<ChangeMark>(edit);
    auto& marks = project.pointMasses.at(change.track).marks;
    const auto position = std::ranges::lower_bound(marks, change.frame, {}, &Mark::frame);
    const bool existing = position != marks.end() && position->frame == change.frame;
    const auto& replacement = forward ? change.after : change.before;
    if (replacement && existing)
    {
        *position = *replacement;
    }
    else if (replacement)
    {
        marks.insert(position, *replacement);
    }
    else if (existing)
    {
        marks.erase(position);
    }
}

void TrackEdits::append(LegacyProject& project, Edit edit)
{
    // Reserve history first so a failed allocation leaves both the project and history intact.
    if (m_Head == m_Commands.capacity())
    {
        m_Commands.reserve(m_Head + std::max<std::size_t>(1, m_Head / 2));
    }
    apply(project, edit, true);
    if (m_SavedHead > m_Head)
    {
        m_SavedHead.reset();
    }
    m_Commands.resize(m_Head);
    m_Commands.push_back(std::move(edit));
    ++m_Head;
}

} // namespace motrack
