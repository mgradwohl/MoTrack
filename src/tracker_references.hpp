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
    PlotOwner,
    CenterOfMassMember,
    VectorSumMember,
    PlotGuest,
    TableDatasetOwner,
    Attachment,
    CircleStepAttachment
};

enum class TrackerReferenceResolution : std::uint8_t
{
    Resolved,
    Missing,
    Ambiguous,
    NoTarget,
    /// A same-named candidate has a class whose constituent eligibility is not verified.
    Unclassified
};

struct TrackerNameReference
{
    TrackerReferenceKind kind;
    /// Diagnostic source path: collection ordinals are zero based; nested array paths use decoded indices.
    std::string path;
    std::string originalName;
    TrackerReferenceResolution resolution;
    /// Original source ordinals, including unclassified possible constituents. Never choose duplicates by order.
    std::vector<std::size_t> candidateSourceIndices;
};

struct TrackerReferenceIndex
{
    std::vector<TrackerSourceTrack> tracks;
    std::vector<TrackerNameReference> references;
    std::vector<std::string> malformedPaths;
};

/// Read-only, partial index of original identities, panel selections/referenceframe, saved-view owners and constituents.
/// The packed selection codec requires exactly four plot/table pairs; its "null" tokens mean no target.
/// Validates persisted bindings but does not require a bound track to remain live. Source XML is unchanged.
/// Bounds: 10000 tracks and 10000 total nested entries, 4096 UTF-16 units per string, 65536 packed units, plus the XML byte cap.
/// Same-name candidate visits have an aggregate limit of 100000, checked before filtering/copying.
/// CenterOfMass.masses and VectorSum.vectors resolve unique exact PointMass/Vector candidates only.
/// Other same-named classes stay Unclassified; duplicates stay Ambiguous, not simulated Java membership.
/// Plot guests retain ordered source slots, including duplicate names; lookup does not claim drawable/data compatibility.
/// Table dataset rows index owners only; canonical nonnegative Java-int settings remain data, not references.
/// TapeMeasure/Protractor/CircleFitter attachment slots retain explicit nulls as NoTarget with an empty originalName.
/// CircleFitter.step_attachment is indexed separately. Resolution does not certify supported attachment geometry.
/// Malformed known fields are explicit. Model dependencies are NOT indexed:
/// an empty malformedPaths list is NOT proof that a rename/export is safe. Tracker's vector-sum reload defect still applies.
[[nodiscard]] TrackerReferenceIndex indexTrackerReferences(const TrackerSource& source, const LegacyProject& project);

} // namespace motrack
