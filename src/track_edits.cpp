#include "track_edits.hpp"

#include "measurements.hpp"
#include "track_identity.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>

namespace motrack
{
namespace
{

auto identityCount(const LegacyProject& project, TrackId id)
{
    return std::ranges::count(project.pointMasses, id, &PointMass::id) + std::ranges::count(project.vectors, id, &VectorTrack::id);
}

template <typename Track> auto findTrack(const LegacyProject& project, std::vector<Track>& tracks, TrackId id)
{
    if (id == 0 || identityCount(project, id) != 1)
    {
        return tracks.end();
    }
    return std::ranges::find(tracks, id, &Track::id);
}

template <typename Track> bool eraseTrack(const LegacyProject& project, std::vector<Track>& tracks, TrackId id)
{
    const auto found = findTrack(project, tracks, id);
    if (found == tracks.end())
    {
        return false;
    }
    tracks.erase(found);
    return true;
}

template <typename Track> bool insertTrack(const LegacyProject& project, std::vector<Track>& tracks, std::size_t position, Track track)
{
    if (track.id == 0 || identityCount(project, track.id) != 0 || position > tracks.size())
    {
        return false;
    }
    tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(position), std::move(track));
    return true;
}

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

ClipTiming clipTiming(const Clip& clip)
{
    return {.frameDurationMs = clip.frameDurationMs, .startTimeMs = clip.startTimeMs};
}

void TrackEdits::clear()
{
    m_Commands.clear();
    m_Head = 0;
    ++m_Revision;
    markSaved();
}

void TrackEdits::markSaved()
{
    m_SavedHead = m_Head;
}

void TrackEdits::invalidateSaved()
{
    m_SavedHead.reset();
}

bool TrackEdits::isModified() const
{
    return m_SavedHead != m_Head;
}

std::uint64_t TrackEdits::revision() const
{
    return m_Revision;
}

bool TrackEdits::createPointMass(LegacyProject& project, std::string name)
{
    if (!hasTrackIdCapacity(project))
    {
        return false;
    }
    if (!append(project, AddTrack{.name = std::move(name), .id = project.nextTrackId, .position = project.pointMasses.size()}))
    {
        return false;
    }
    ++project.nextTrackId; // Commit allocation only after the command succeeds.
    // Undo removes the track, but the advanced allocation counter must still be saved.
    invalidateSaved();
    return true;
}

bool TrackEdits::renamePointMass(LegacyProject& project, std::size_t track, std::string name)
{
    if (track >= project.pointMasses.size() || name.empty() ||
        std::ranges::any_of(project.pointMasses, [&name](const auto& mass) { return mass.name == name; }))
    {
        return false;
    }
    return append(
        project,
        RenameTrack{.id = project.pointMasses.at(track).id, .after = std::move(name), .before = project.pointMasses.at(track).name});
}

bool TrackEdits::removePointMass(LegacyProject& project, std::size_t track)
{
    if (track >= project.pointMasses.size())
    {
        return false;
    }
    return append(project, RemoveTrack{.position = track, .before = project.pointMasses.at(track)});
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
        return append(project, ChangeMass{.id = project.pointMasses.at(track).id, .after = mass, .before = before});
    }
    return true;
}

bool TrackEdits::setMassUnit(LegacyProject& project, std::string unit)
{
    if (unit.empty())
    {
        return false;
    }
    if (unit == project.massUnit)
    {
        return true;
    }
    return append(project, ChangeMassUnit{.after = std::move(unit), .before = project.massUnit});
}

bool TrackEdits::setTimeUnit(LegacyProject& project, std::string unit)
{
    if (unit.empty())
    {
        return false;
    }
    if (unit == project.timeUnit)
    {
        return true;
    }
    return append(project, ChangeTimeUnit{.after = std::move(unit), .before = project.timeUnit});
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
        return append(project, ChangeMark{.id = project.pointMasses.at(track).id, .frame = mark.frame, .after = mark, .before = before});
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
    return append(project, ChangeMark{.id = project.pointMasses.at(track).id, .frame = frame, .after = std::nullopt, .before = *mark});
}

bool TrackEdits::createVector(LegacyProject& project, std::string name)
{
    if (!hasTrackIdCapacity(project) || name.empty() ||
        std::ranges::any_of(project.vectors, [&name](const auto& track) { return track.name == name; }))
    {
        return false;
    }
    if (!append(project, AddVector{.name = std::move(name), .id = project.nextTrackId, .position = project.vectors.size()}))
    {
        return false;
    }
    ++project.nextTrackId;
    invalidateSaved();
    return true;
}

bool TrackEdits::renameVector(LegacyProject& project, std::size_t track, std::string name)
{
    if (track >= project.vectors.size() || name.empty() ||
        std::ranges::any_of(project.vectors, [&name](const auto& vector) { return vector.name == name; }))
    {
        return false;
    }
    return append(project,
                  RenameVector{.id = project.vectors.at(track).id, .after = std::move(name), .before = project.vectors.at(track).name});
}

bool TrackEdits::removeVector(LegacyProject& project, std::size_t track)
{
    if (track >= project.vectors.size())
    {
        return false;
    }
    return append(project, RemoveVector{.position = track, .before = project.vectors.at(track)});
}

bool TrackEdits::markVector(LegacyProject& project, std::size_t track, VectorSample sample)
{
    if (track >= project.vectors.size() || sample.frame < 0 || !std::isfinite(sample.tailX) || !std::isfinite(sample.tailY) ||
        !std::isfinite(sample.componentX) || !std::isfinite(sample.componentY) || !std::isfinite(sample.tailX + sample.componentX) ||
        !std::isfinite(sample.tailY + sample.componentY))
    {
        return false;
    }
    const auto& samples = project.vectors.at(track).samples;
    const auto previous = std::ranges::lower_bound(samples, sample.frame, {}, &VectorSample::frame);
    const auto before = previous != samples.end() && previous->frame == sample.frame ? std::optional(*previous) : std::nullopt;
    if (!before || before->tailX != sample.tailX || before->tailY != sample.tailY || before->componentX != sample.componentX ||
        before->componentY != sample.componentY)
    {
        return append(project,
                      ChangeVectorSample{.id = project.vectors.at(track).id, .frame = sample.frame, .after = sample, .before = before});
    }
    return true;
}

bool TrackEdits::removeVectorSample(LegacyProject& project, std::size_t track, int frame)
{
    if (track >= project.vectors.size())
    {
        return false;
    }
    const auto& samples = project.vectors.at(track).samples;
    const auto sample = std::ranges::lower_bound(samples, frame, {}, &VectorSample::frame);
    if (sample == samples.end() || sample->frame != frame)
    {
        return false;
    }
    return append(project,
                  ChangeVectorSample{.id = project.vectors.at(track).id, .frame = frame, .after = std::nullopt, .before = *sample});
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
    return append(project,
                  ChangeCalibration{.after = std::move(next),
                                    .before = project.calibration,
                                    .afterUnit = std::move(distance.unit),
                                    .beforeUnit = project.lengthUnit});
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
    return append(project,
                  ChangeCalibration{.after = std::move(next),
                                    .before = project.calibration,
                                    .afterUnit = initial ? "px" : project.lengthUnit,
                                    .beforeUnit = project.lengthUnit});
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
    return append(project, ChangeClip{.after = after, .before = before});
}

bool TrackEdits::canRedo() const
{
    return m_Head < m_Commands.size();
}

bool TrackEdits::setClipTiming(LegacyProject& project, double frameDurationMs, double startTimeMs)
{
    auto candidate = project.clip;
    candidate.frameDurationMs = frameDurationMs;
    candidate.startTimeMs = startTimeMs;
    const auto before = clipTiming(project.clip);
    const auto after = clipTiming(candidate);
    if (!supportsConstantClipTimingEdits(project) || !validConstantClipTiming(candidate) || after == before)
    {
        return false;
    }
    return append(project, ChangeClipTiming{.after = after, .before = before});
}

bool TrackEdits::undo(LegacyProject& project)
{
    if (!canUndo())
    {
        return false;
    }
    if (!apply(project, m_Commands.at(m_Head - 1), false))
    {
        return false;
    }
    --m_Head;
    ++m_Revision;
    return true;
}

bool TrackEdits::redo(LegacyProject& project)
{
    if (!canRedo())
    {
        return false;
    }
    if (!apply(project, m_Commands.at(m_Head), true))
    {
        return false;
    }
    ++m_Head;
    ++m_Revision;
    return true;
}

bool TrackEdits::apply(LegacyProject& project, const Edit& edit, bool forward)
{
    if (const auto* track = std::get_if<AddVector>(&edit))
    {
        if (forward)
        {
            return insertTrack(project, project.vectors, track->position, VectorTrack{.name = track->name, .samples = {}, .id = track->id});
        }
        return eraseTrack(project, project.vectors, track->id);
    }
    if (const auto* rename = std::get_if<RenameVector>(&edit))
    {
        const auto track = findTrack(project, project.vectors, rename->id);
        if (track == project.vectors.end())
        {
            return false;
        }
        track->name = forward ? rename->after : rename->before;
        return true;
    }
    if (const auto* remove = std::get_if<RemoveVector>(&edit))
    {
        if (forward)
        {
            return eraseTrack(project, project.vectors, remove->before.id);
        }
        return insertTrack(project, project.vectors, remove->position, remove->before);
    }
    if (const auto* change = std::get_if<ChangeVectorSample>(&edit))
    {
        const auto track = findTrack(project, project.vectors, change->id);
        if (track == project.vectors.end())
        {
            return false;
        }
        auto& samples = track->samples;
        const auto position = std::ranges::lower_bound(samples, change->frame, {}, &VectorSample::frame);
        const bool existing = position != samples.end() && position->frame == change->frame;
        const auto& replacement = forward ? change->after : change->before;
        if (replacement && existing)
        {
            *position = *replacement;
        }
        else if (replacement)
        {
            samples.insert(position, *replacement);
        }
        else if (existing)
        {
            samples.erase(position);
        }
        return true;
    }
    if (const auto* track = std::get_if<AddTrack>(&edit))
    {
        if (forward)
        {
            return insertTrack(project, project.pointMasses, track->position, PointMass{.name = track->name, .marks = {}, .id = track->id});
        }
        return eraseTrack(project, project.pointMasses, track->id);
    }
    if (const auto* rename = std::get_if<RenameTrack>(&edit))
    {
        const auto track = findTrack(project, project.pointMasses, rename->id);
        if (track == project.pointMasses.end())
        {
            return false;
        }
        track->name = forward ? rename->after : rename->before;
        return true;
    }
    if (const auto* remove = std::get_if<RemoveTrack>(&edit))
    {
        if (forward)
        {
            return eraseTrack(project, project.pointMasses, remove->before.id);
        }
        return insertTrack(project, project.pointMasses, remove->position, remove->before);
    }
    if (const auto* mass = std::get_if<ChangeMass>(&edit))
    {
        const auto track = findTrack(project, project.pointMasses, mass->id);
        if (track == project.pointMasses.end())
        {
            return false;
        }
        track->mass = forward ? mass->after : mass->before;
        return true;
    }
    if (const auto* change = std::get_if<ChangeMassUnit>(&edit))
    {
        project.massUnit = forward ? change->after : change->before;
        return true;
    }
    if (const auto* change = std::get_if<ChangeTimeUnit>(&edit))
    {
        project.timeUnit = forward ? change->after : change->before;
        return true;
    }
    if (const auto* change = std::get_if<ChangeCalibration>(&edit))
    {
        auto calibration = forward ? std::optional(change->after) : change->before;
        auto unit = forward ? change->afterUnit : change->beforeUnit;
        project.calibration = std::move(calibration);
        project.lengthUnit = std::move(unit);
        return true;
    }
    if (const auto* change = std::get_if<ChangeClip>(&edit))
    {
        const auto& range = forward ? change->after : change->before;
        project.clip.firstFrame = range.firstFrame;
        project.clip.stepSize = range.stepSize;
        project.clip.stepCount = range.stepCount;
        return true;
    }
    if (const auto* change = std::get_if<ChangeClipTiming>(&edit))
    {
        if (!supportsConstantClipTimingEdits(project))
        {
            return false;
        }
        const auto& timing = forward ? change->after : change->before;
        project.clip.frameDurationMs = timing.frameDurationMs;
        project.clip.startTimeMs = timing.startTimeMs;
        return true;
    }

    const auto& change = std::get<ChangeMark>(edit);
    const auto track = findTrack(project, project.pointMasses, change.id);
    if (track == project.pointMasses.end())
    {
        return false;
    }
    auto& marks = track->marks;
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
    return true;
}

bool TrackEdits::append(LegacyProject& project, Edit edit)
{
    // Reserve history first so a failed allocation leaves both the project and history intact.
    if (m_Head == m_Commands.capacity())
    {
        m_Commands.reserve(m_Head + std::max<std::size_t>(1, m_Head / 2));
    }
    if (!apply(project, edit, true))
    {
        return false;
    }
    if (m_SavedHead > m_Head)
    {
        m_SavedHead.reset();
    }
    m_Commands.resize(m_Head);
    m_Commands.push_back(std::move(edit));
    ++m_Head;
    ++m_Revision;
    return true;
}

} // namespace motrack
