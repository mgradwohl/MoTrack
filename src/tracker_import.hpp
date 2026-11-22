#pragma once

#include "legacy_project.hpp"
#include "osp/source.hpp"
#include "model/tracker_loaders.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

/// Internal import layer shared by `LegacyProject` parsing, the reference index and the standard writers.
namespace motrack::detail
{

/// A Tracker document read with OSP's line-oriented reader, its typed panel load and the reduced project.
struct ImportedTracker
{
    std::shared_ptr<const osp::Source> source;
    tracker::PanelLoad panel;
    LegacyProject project;
    std::vector<TrackSourceBinding> bindings;
};

/// Parses bounded Tracker XML with `motrack_osp`. Throws ProjectError for oversized or unreadable input.
[[nodiscard]] std::shared_ptr<const osp::Source> parseTrackerXml(std::string_view xml);
/// Applies MoTrack's import policy to a parsed document. Throws ProjectError like `parseLegacyProject`.
[[nodiscard]] ImportedTracker importTracker(std::shared_ptr<const osp::Source> source);
/// `validateTrackerSource`, returning the source's import so callers parse it only once.
[[nodiscard]] ImportedTracker validatedTrackerSource(const TrackerSource& source, const LegacyProject& project);
/// Whether known bindings still name exactly the editable track objects of `original`, the import of their source.
/// Throws ProjectError when they contradict the document itself (a zero, unreserved, duplicate or wrong-kind track
/// ID). Returns false when they only disagree with the source's objects, as ordinals recorded by MoTrack's former
/// DOM reader can for a source that OSP's line reader reads differently.
[[nodiscard]] bool
trackerBindingsMatch(const std::vector<TrackSourceBinding>& bindings, const LegacyProject& project, const ImportedTracker& original);

} // namespace motrack::detail

namespace motrack
{
struct TrackerReferenceIndex;
}

namespace motrack::detail
{
/// `indexTrackerReferences` for a source the caller has already validated and parsed.
[[nodiscard]] TrackerReferenceIndex indexTrackerReferences(const osp::Source& xml, const TrackerSource& source);

} // namespace motrack::detail
