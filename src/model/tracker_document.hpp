#pragma once

#include "model/dependencies/dependency_graph.hpp"
#include "model/families.hpp"
#include "model/object_id.hpp"
#include "osp/source.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/// The typed Tracker document (docs/tracker-document.md, "Type model"): the immutable import baseline, the typed
/// objects bound to its nodes, the identity allocators, the media extension and the derived dependency graph.
/// A copyable value; copies share the baseline, typed values and graph. Mutate only through the owner's commands
/// (B2.2), on the owner's thread; workers receive copies. Standard-only interface.
namespace motrack::model
{

/// Whether typed objects are bound to baseline nodes.
enum class Associations : std::uint8_t
{
    None,    ///< No baseline (native version 1, or a document never imported from Tracker).
    Known,   ///< Every addressable baseline object has exactly one typed or Unknown object.
    Unknown, ///< A baseline is preserved but was never indexed (native versions 2 and 3).
    Stale,   ///< Bindings no longer match the current reader, or the reader refuses the baseline (#317).
};

[[nodiscard]] std::string_view associationsName(Associations associations) noexcept;

enum class ObjectState : std::uint8_t
{
    Live,
    /// Kept with its binding so that undo restores the same node and export removes exactly that element.
    Removed,
};

/// The allocator an identity came from. It classifies the identity for life, whatever its class later becomes.
enum class AllocationRange : std::uint8_t
{
    /// Typed tracks: 1, 2, ... in source order at import, then creation. The counter never rewinds.
    Counter,
    /// Unknown tracks and every other object: down from 2^64 - 1, skipping values counter identities use.
    Countdown,
};

[[nodiscard]] std::string_view allocationRangeName(AllocationRange range) noexcept;

/// The loader revision this build reads Tracker sources with (native v5's `trackerReading`, v7's `readerRevision`):
/// 1 applies Tracker's clip frame shift and the last of duplicate coordinate keys (#313, #314), 2 also takes Java's
/// slots for track frames (part of #315). Native versions 1 to 4 count as revision 0.
inline constexpr std::uint32_t CURRENT_READER_REVISION = 2;

/// The first countdown identity; also the low-water mark of a document that has handed out none below it.
inline constexpr std::uint64_t COUNTDOWN_START = std::numeric_limits<std::uint64_t>::max();

/// One addressable object.
struct ObjectSlot
{
    ObjectId id{};
    AllocationRange range = AllocationRange::Countdown;
    Role role = Role::Track;
    /// The exact (effective) Tracker class.
    std::string className{};
    /// The object node in the baseline, or none for an object MoTrack created or a document without associations.
    std::optional<osp::NodeId> binding{};
    ObjectState state = ObjectState::Live;
    /// Never null. Shared by copies and replaced, not modified, by edits.
    std::shared_ptr<const TypedValue> value{};

    [[nodiscard]] Family family() const noexcept
    {
        return familyOf(*value);
    }

    friend bool operator==(const ObjectSlot& left, const ObjectSlot& right);
};

/// The clip values Tracker XML does not hold in this form: the current media reference spellings (relative to the
/// document, or archive-root-relative) and the discovered source frame count.
struct MediaExtension
{
    std::vector<std::string> references{};
    std::optional<std::int32_t> sourceFrameCount{};

    bool operator==(const MediaExtension&) const = default;
};

/// A saved reference of the baseline that did not become a graph dependency, reported instead of dropped. The
/// reference stays in the baseline bytes; only its relationship is unknown.
struct ReferenceIssue
{
    ObjectId owner{};
    std::string field{}; ///< The reference descriptor's id.
    std::string reason{};

    bool operator==(const ReferenceIssue&) const = default;
};

/// Everything a document holds except the derived index and graph: the input of `TrackerDocument::assemble`.
struct DocumentParts
{
    std::shared_ptr<const osp::Source> baseline{};
    Associations associations = Associations::None;
    /// Imported objects in baseline order (singletons, then the elements of `tracks`), then objects MoTrack added.
    std::vector<ObjectSlot> objects{};
    /// Presentation order of the live point masses and vectors (the track list), separate from export order.
    std::vector<ObjectId> trackOrder{};
    /// The next counter identity (today's `nextTrackId`).
    std::uint64_t nextObjectId = 1;
    /// The lowest countdown identity handed out; it never rises.
    std::uint64_t countdownLowWater = COUNTDOWN_START;
    MediaExtension media{};
    /// The loader revision the typed values follow. A document imported by this build follows the current one; one
    /// converted from a native file keeps the revision that file records, never transformed again.
    std::uint32_t readerRevision = CURRENT_READER_REVISION;
};

struct DocumentError
{
    std::string message{};

    bool operator==(const DocumentError&) const = default;
};

// The implicit move constructor can throw with MSVC's STL, whose std::map allocates a sentinel when moved.
class TrackerDocument // NOLINT(bugprone-exception-escape)
{
  public:
    /// An empty native document: a panel, clip and clip control holding the values of an empty project, without a
    /// baseline or tracks.
    TrackerDocument();

    /// Types a parsed Tracker document with B1's import policy: refusals where B1 refuses (with its messages),
    /// Unknown objects where it cannot present a class, counter identities for the typed tracks in source order
    /// and countdown identities for everything else. Associations are known.
    [[nodiscard]] static std::expected<TrackerDocument, DocumentError> importBaseline(std::shared_ptr<const osp::Source> baseline);
    /// Validates the parts (identity ranges and uniqueness, roles and families, bindings to baseline objects of the
    /// recorded class at their role's place, exactly one object per addressable baseline object with known
    /// associations, the track order and the reader revision) and derives the index and the graph. Countdown objects
    /// without an identity receive one, in baseline order.
    [[nodiscard]] static std::expected<TrackerDocument, DocumentError> assemble(DocumentParts parts);

    [[nodiscard]] const std::shared_ptr<const osp::Source>& baseline() const noexcept;
    [[nodiscard]] Associations associations() const noexcept;
    [[nodiscard]] std::span<const ObjectSlot> objects() const noexcept;
    /// Borrowed until the document changes; null for an unknown identity.
    [[nodiscard]] const ObjectSlot* object(ObjectId id) const;
    /// The live typed object of a singleton role, else its live Unknown object; null when there is none.
    [[nodiscard]] const ObjectSlot* singleton(Role role) const;
    [[nodiscard]] std::span<const ObjectId> trackOrder() const noexcept;
    [[nodiscard]] std::uint64_t nextObjectId() const noexcept;
    [[nodiscard]] std::uint64_t countdownLowWater() const noexcept;
    /// Whether creation can still take a counter identity: the counter stays below the countdown low-water mark.
    [[nodiscard]] bool hasCounterCapacity() const noexcept;
    [[nodiscard]] const MediaExtension& media() const noexcept;
    [[nodiscard]] std::uint32_t readerRevision() const noexcept;
    [[nodiscard]] const DependencyGraph& graph() const noexcept;
    [[nodiscard]] const std::shared_ptr<const DependencyGraph>& sharedGraph() const noexcept;
    [[nodiscard]] std::span<const ReferenceIssue> referenceIssues() const noexcept;
    /// Classes of the live Unknown elements of `tracks`, in document order (`unsupportedTrackTypes`).
    [[nodiscard]] std::vector<std::string> unknownTrackClasses() const;
    [[nodiscard]] const DocumentParts& parts() const noexcept;

    /// Compares the baseline (pointer first, then bytes), associations, reader revision, objects, track order, counters
    /// and media extension; never the graph, which is derived.
    friend bool operator==(const TrackerDocument& left, const TrackerDocument& right);

  private:
    explicit TrackerDocument(DocumentParts parts);
    [[nodiscard]] std::expected<void, DocumentError> derive();

    DocumentParts m_Parts;
    std::unordered_map<ObjectId, std::size_t> m_Index;
    std::shared_ptr<const DependencyGraph> m_Graph;
    std::vector<ReferenceIssue> m_ReferenceIssues;
};

/// One step of a baseline path: the child ordinal under the parent node and the property name there.
struct PathStep
{
    std::size_t ordinal = 0;
    std::string name{};

    bool operator==(const PathStep&) const = default;
};

/// The path from the root object to a node, through the properties that contain it; it generalizes native v4's
/// `sourceIndex` and is stable across parses of the same bytes.
[[nodiscard]] std::vector<PathStep> baselinePath(const osp::Source& source, osp::NodeId node);

} // namespace motrack::model
