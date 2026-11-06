#pragma once

#include "legacy_project.hpp"

namespace motrack
{

struct TrackerSourceTrack
{
    std::size_t sourceIndex;
    std::string className;
    std::optional<std::string> originalName;
    /// Absent for unsupported tracks or native documents with unknown original associations.
    /// A known ID remains here after a native rename, reorder or deletion.
    std::optional<TrackId> nativeId;
};

enum class TrackerReferenceKind : std::uint8_t
{
    SelectedTrack,
    ReferenceFrame,
    PlotSelection,
    TableSelection
};

enum class TrackerReferenceResolution : std::uint8_t
{
    Resolved,
    Missing,
    Ambiguous,
    NoTarget
};

struct TrackerNameReference
{
    TrackerReferenceKind kind;
    /// Diagnostic source path; track and view ordinals are zero based, not XML array labels.
    std::string path;
    std::string originalName;
    TrackerReferenceResolution resolution;
    /// Original source ordinals, including unsupported tracks. Never resolve duplicate names by order.
    std::vector<std::size_t> candidateSourceIndices;
};

struct TrackerReferenceIndex
{
    std::vector<TrackerSourceTrack> tracks;
    std::vector<TrackerNameReference> references;
    std::vector<std::string> malformedPaths;
};

/// Read-only, partial index of original track identities and panel selections/referenceframe.
/// The packed selection codec requires exactly four plot/table pairs; only its "null" tokens mean no target.
/// Validates persisted bindings but does not require a bound track to remain live. Source XML is unchanged.
/// Bounds: 10000 track entries, 4096 UTF-16 units per name, 65536 units for packed views, plus the XML byte cap.
/// Malformed known fields are reported explicitly. Nested views, attachments and dependencies are NOT
/// indexed yet: an empty malformedPaths list is NOT proof that a rename/export is safe.
[[nodiscard]] TrackerReferenceIndex indexTrackerReferences(const TrackerSource& source, const LegacyProject& project);

} // namespace motrack
