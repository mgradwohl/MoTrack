#include "track_edits.hpp"

#include "measurements.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace motrack
{

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
        append(project, ChangeMark{.track = track, .after = mark, .before = before});
    }
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
        next.fixedOrigin = true;
        next.fixedAngle = true;
        next.fixedScale = true;
        next.keyframes.push_back({.frame = 0,
                                  .originX = distance.defaultOriginX,
                                  .originY = distance.defaultOriginY,
                                  .angleDegrees = 0.0,
                                  .scaleX = 1.0,
                                  .scaleY = 1.0});
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
        const auto position = std::ranges::lower_bound(next.keyframes, distance.frame, {}, &CalibrationKey::frame);
        if (position != next.keyframes.end() && position->frame == distance.frame)
        {
            *position = *key;
        }
        else
        {
            next.keyframes.insert(position, *key);
        }
    }
    append(project,
           ChangeCalibration{.after = std::move(next),
                             .before = project.calibration,
                             .afterUnit = std::move(distance.unit),
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
    const auto position = std::ranges::lower_bound(marks, change.after.frame, {}, &Mark::frame);
    const bool existing = position != marks.end() && position->frame == change.after.frame;
    const auto replacement = forward ? std::optional(change.after) : change.before;
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
