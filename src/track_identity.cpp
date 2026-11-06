#include "track_identity.hpp"

#include <limits>
#include <unordered_set>

namespace motrack
{

bool hasTrackIdCapacity(const LegacyProject& project)
{
    return project.nextTrackId > 0 && project.nextTrackId < std::numeric_limits<TrackId>::max();
}

TrackId allocateTrackId(LegacyProject& project)
{
    if (!hasTrackIdCapacity(project))
    {
        throw ProjectError("Track identity space is exhausted.");
    }
    return project.nextTrackId++;
}

void validateTrackIdentities(const LegacyProject& project)
{
    if (project.nextTrackId == 0)
    {
        throw ProjectError("Invalid track identity counter.");
    }
    std::unordered_set<TrackId> ids;
    const auto check = [&](TrackId id)
    {
        if (id == 0 || id >= project.nextTrackId || !ids.insert(id).second)
        {
            throw ProjectError("Invalid or duplicate track identity.");
        }
    };
    for (const auto& track : project.pointMasses)
    {
        check(track.id);
    }
    for (const auto& track : project.vectors)
    {
        check(track.id);
    }
}

} // namespace motrack
