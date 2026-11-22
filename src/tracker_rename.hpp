#pragma once

#include "legacy_project.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace motrack
{
class SourceEdits;
}

namespace motrack::osp
{
class Source;
}

namespace motrack
{
/// Validate the bounded schema used for audited identity edits, including the final emitted document.
void validateTrackerEditSchema(const osp::Source& xml);
/// Explicit movie adapter audit, also used by the movie identity/membership path.
void validateTrackerMovieEditSchema(const osp::Source& xml);

/// A validated rename of live tracks: the new names by `tracks` ordinal and the saved references they rewrite.
struct TrackerNamePlan
{
    struct Rewrite
    {
        std::string path; ///< The reference's source path (see TrackerNameReference).
        std::string originalName;
        std::string name;
    };
    std::map<std::size_t, std::string> replacements;
    std::vector<Rewrite> rewrites;
};

/// The admission check of a rename (by native ID, among the live IDs): the audited schema, every original and final name
/// unique and in the name codec, and every reference resolved, of an audited kind and to an eligible target. Nothing when
/// no track is renamed. Throws ProjectError.
[[nodiscard]] std::optional<TrackerNamePlan> planTrackerNames(const osp::Source& xml,
                                                              const std::map<TrackId, std::string>& renamed,
                                                              const std::set<TrackId>& liveIds,
                                                              const TrackerSource& source,
                                                              bool movie);
/// Writes a plan's names and the references they rewrite. Call before removing tracks, whose ordinals it uses.
void applyTrackerNames(SourceEdits& edits, const TrackerNamePlan& plan);
/// Internal conservative mass/vector rename patcher (`planTrackerNames` then `applyTrackerNames`). Updates baseline
/// names only after validating every source relationship.
[[nodiscard]] bool
patchTrackerNames(SourceEdits& xml, const LegacyProject& project, LegacyProject& baseline, const TrackerSource& source, bool movie);
/// Audit independent-track membership edits; deleted tracks may have selection, plot or table-column/format references.
/// Validates original schemas and resolved links without mutating source content.
void validateTrackerTrackMembership(const osp::Source& xml, const LegacyProject& project, const TrackerSource& source, bool movie);
/// Clear deleted selections, choose surviving view targets, and remove deleted plot links or table column/format owners.
/// Call after live-name patches and after appending created tracks; membership must already be audited.
void patchTrackerDeletedReferences(SourceEdits& xml, const LegacyProject& project, const TrackerSource& source);
} // namespace motrack
