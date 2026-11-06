#include "track_edits.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace motrack
{

void TrackEdits::clear()
{
    m_Commands.clear();
    m_Head = 0;
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

bool TrackEdits::canUndo() const
{
    return m_Head != 0;
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
    m_Commands.resize(m_Head);
    m_Commands.push_back(std::move(edit));
    ++m_Head;
}

} // namespace motrack
