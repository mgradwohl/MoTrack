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
    TableSelection,
    TableColumnOwner,
    TableFormatOwner,
    PlotOwner
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
    /// Diagnostic source path: collection ordinals are zero based; nested array paths use decoded indices.
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

/// Read-only, partial index of original identities, panel selections/referenceframe and saved-view owners.
/// The packed selection codec requires exactly four plot/table pairs; only its "null" tokens mean no target.
/// Validates persisted bindings but does not require a bound track to remain live. Source XML is unchanged.
/// Bounds: 10000 tracks and 10000 nested entries, 4096 UTF-16 units per string, 65536 packed units, plus the XML byte cap.
/// Malformed known fields are reported explicitly. Dataset indices, plot guests, attachments and dependencies are NOT
/// indexed yet: an empty malformedPaths list is NOT proof that a rename/export is safe.
[[nodiscard]] TrackerReferenceIndex indexTrackerReferences(const TrackerSource& source, const LegacyProject& project);

} // namespace motrack
