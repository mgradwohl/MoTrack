#pragma once

#include "model/object_id.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

/// Dependency evaluation core shared by every dependent Tracker object (docs/dependency-graph.md).
/// Qt-free and independent of the typed document model, which owns the objects themselves.
namespace motrack::model
{

/// A value of DependencyGraph's mutation counter. Revisions are only compared within one graph and its copies.
struct Revision
{
    std::uint64_t value = 0;

    constexpr auto operator<=>(const Revision&) const = default;
};

/// The computed part of an object that a graph node stands for. Tracker computes image-space positions
/// separately from world-space data: a moving reference frame follows its origin track's image positions,
/// while every track's world data, including the origin's, is measured in that frame. With a single node
/// per object that ordinary workflow, and a frame on a center of mass, would be reported as a cycle.
enum class Facet : std::uint8_t
{
    /// Image-space geometry in pixels: marks, attached end points, model, center and sum positions, and
    /// coordinate transforms.
    Geometry,
    /// World-space values in calibrated units: data columns, masses, and function and parameter values.
    /// Every object's Data implicitly depends on its own Geometry.
    Data,
};

inline constexpr std::size_t FACET_COUNT = 2;

/// One graph node: a facet of a live object.
struct NodeKey
{
    ObjectId object;
    Facet facet = Facet::Geometry;

    constexpr auto operator<=>(const NodeKey&) const = default;
};

/// The relationship an edge records. Every edge points from the dependent owner to the object it reads.
enum class DependencyKind : std::uint8_t
{
    /// The target is a constituent of the owner: CenterOfMass.masses, VectorSum.vectors, DynamicSystem.particles.
    MemberOf,
    /// The owner's point follows the target: TapeMeasure, Protractor and CircleFitter attachments and
    /// CircleFitter.step_attachment.
    AttachedTo,
    /// The owner reads the target's data: a track's data functions, filtered clones and other derived data.
    ReferencesDataOf,
    /// The target defines the owner's coordinate frame: a reference frame's origin track, or the coordinate
    /// system that a track's world data is measured in.
    FrameOf,
    /// The target supplies the owner's initial state: DynamicParticle and DynamicSystem boosters.
    BoosterOf,
    /// The owner's expression refers to the target function or parameter.
    FunctionOf,
};

/// Stable diagnostic spelling, e.g. "member-of". User interfaces translate by value.
[[nodiscard]] std::string_view dependencyKindName(DependencyKind kind) noexcept;

/// What evaluation does when a reference has no live target. Either way the reference is kept and reported.
enum class MissingTarget : std::uint8_t
{
    /// The owner is not evaluated, so no partial result is computed silently (constituents, frames, functions).
    Blocks,
    /// The owner has a defined value of its own: an unattached tool point or a model's own initial state.
    FallsBack,
};

/// How a saved name resolved. These match TrackerReferenceResolution, whose Resolved is Unique here.
enum class BindingOutcome : std::uint8_t
{
    Unique,
    Ambiguous,
    Missing,
    /// An explicitly empty slot: a typed null attachment or a packed "null" token. It creates no dependency.
    NoTarget,
    /// A same-named candidate has a class whose eligibility is not verified, such as a nested dependent track.
    Unclassified,
};

/// One reference held by an owner node.
struct Dependency
{
    DependencyKind kind = DependencyKind::MemberOf;
    /// Position within the owner's field, such as a member or attachment ordinal. The owner, kind and slot
    /// together are unique, so repeated serialized members keep separate slots.
    std::uint32_t slot = 0;
    /// The facet of the target that the owner reads.
    Facet targetFacet = Facet::Geometry;
    /// The bound identity. It is kept when the target is removed, leaving the reference dangling, and binds
    /// again when that identity is restored. Absent only for a saved name that did not resolve uniquely.
    std::optional<ObjectId> target;
    /// The name read from the source file. It is kept for diagnostics and to write an unresolved name back
    /// unchanged; empty for a reference created by a native edit.
    std::string savedName;
    /// Unique when bound; otherwise Ambiguous, Missing or Unclassified.
    BindingOutcome binding = BindingOutcome::Unique;
    /// For an unresolved name, the same-named candidates seen at load. A candidate is never chosen by order.
    std::vector<ObjectId> candidates;
    MissingTarget onMissing = MissingTarget::Blocks;

    bool operator==(const Dependency&) const = default;
};

/// A bound reference created by a native edit.
[[nodiscard]] Dependency boundDependency(
    DependencyKind kind, std::uint32_t slot, ObjectId target, Facet targetFacet, MissingTarget onMissing = MissingTarget::Blocks);

/// Addresses one dependency of one owner node.
struct DependencyKey
{
    NodeKey owner;
    DependencyKind kind = DependencyKind::MemberOf;
    std::uint32_t slot = 0;

    constexpr auto operator<=>(const DependencyKey&) const = default;
};

struct CycleEdge
{
    NodeKey from;
    NodeKey to;
    /// The dependency followed. Absent for the implicit edge from an object's Data to its own Geometry.
    std::optional<DependencyKey> dependency;

    bool operator==(const CycleEdge&) const = default;
};

/// A closed path: each edge ends where the next starts, and the last edge returns to the first node.
struct Cycle
{
    std::vector<CycleEdge> edges;

    bool operator==(const Cycle&) const = default;
};

enum class DependencyErrorCode : std::uint8_t
{
    InvalidObjectId,         ///< Zero is reserved.
    DuplicateObject,         ///< The identity is already live.
    IdentityReused,          ///< The identity belongs to a removed object; only restoreObject may bring it back.
    UnknownObject,           ///< The identity is not live (or, for restoreObject and targets, was never in the graph).
    InvalidDependency,       ///< The target, binding and candidates fields disagree (see Dependency).
    DuplicateDependency,     ///< The owner already has a dependency with this kind and slot.
    UnknownDependency,       ///< The owner has no dependency with this kind and slot.
    CycleRejected,           ///< The edit would close a cycle; DependencyError::cycle is its path.
    CandidateBudgetExceeded, ///< NameIndex: too many same-name candidate visits.
};

/// English diagnostic text for logs and tests; user interfaces translate by code.
[[nodiscard]] std::string_view errorMessage(DependencyErrorCode code) noexcept;

struct DependencyError
{
    DependencyErrorCode code = DependencyErrorCode::UnknownObject;
    /// The object the failed operation addressed; zero when not applicable.
    ObjectId object;
    /// For CycleRejected, the cycle the edit would have closed.
    Cycle cycle;

    bool operator==(const DependencyError&) const = default;
};

/// A value or the reason an operation was refused without changing the graph: the subset of std::expected
/// that callers need, because CI's Clang 18 with libstdc++ 13 does not provide std::expected. Accessing the
/// absent alternative throws std::bad_optional_access. Plain members rather than std::variant keep the
/// static analyzer free of false uninitialized-value reports through libstdc++'s variant.
template <typename T = std::monostate> class [[nodiscard]] DependencyResult
{
    static_assert(std::is_default_constructible_v<T>, "DependencyResult stores a default-constructed value with an error.");

  public:
    // Implicit, like std::expected, so that operations can return either alternative directly.
    DependencyResult(T value) : m_Value(std::move(value)), m_HasValue(true)
    {}

    DependencyResult(DependencyError error) : m_Error(std::move(error))
    {}

    [[nodiscard]] bool hasValue() const noexcept
    {
        return m_HasValue;
    }

    explicit operator bool() const noexcept
    {
        return m_HasValue;
    }

    [[nodiscard]] const T& operator*() const&
    {
        requireValue(true);
        return m_Value;
    }

    [[nodiscard]] T&& operator*() &&
    {
        requireValue(true);
        return std::move(m_Value);
    }

    [[nodiscard]] const T* operator->() const
    {
        requireValue(true);
        return &m_Value;
    }

    [[nodiscard]] const DependencyError& error() const
    {
        requireValue(false);
        return m_Error;
    }

  private:
    void requireValue(bool expected) const
    {
        if (m_HasValue != expected)
        {
            throw std::bad_optional_access();
        }
    }

    T m_Value{};
    DependencyError m_Error{};
    bool m_HasValue = false;
};

struct ObjectInfo
{
    /// The current user-visible label (a Tracker name). It may be empty or shared with other objects.
    std::string label;
    /// The exact Tracker Java class, for name-binding filters only.
    std::string className;

    bool operator==(const ObjectInfo&) const = default;
};

/// Everything removeObject took out of the graph, for undo through restoreObject.
struct RemovedObject
{
    ObjectId id;
    ObjectInfo info;
    std::vector<Dependency> geometryDependencies;
    std::vector<Dependency> dataDependencies;
    /// Other objects' references to this identity, which now dangle. They stay in the graph, are reported by
    /// every plan and bind again when the identity is restored. Informational: restoreObject does not read it.
    std::vector<DependencyKey> danglingReferences;
};

enum class CyclePolicy : std::uint8_t
{
    /// For edits: refuse a change that would close a cycle and report its path.
    Reject,
    /// For loading: keep a cyclic reference read from a file, so that it is preserved and reported by plans
    /// instead of being dropped.
    Record,
};

enum class ReferenceState : std::uint8_t
{
    Dangling, ///< Bound to an identity that is not live.
    Unbound,  ///< The saved name never resolved uniquely.
};

struct ReferenceDiagnostic
{
    DependencyKey key;
    ReferenceState state = ReferenceState::Dangling;
    Dependency dependency;

    bool operator==(const ReferenceDiagnostic&) const = default;
};

enum class BlockReason : std::uint8_t
{
    Cycle,             ///< The node is on a dependency cycle.
    DanglingReference, ///< A Blocks dependency's target is not live.
    UnboundReference,  ///< A Blocks dependency's saved name never resolved.
    DependsOnBlocked,  ///< A dependency of either policy, or the node's own Geometry, is blocked.
};

struct BlockedNode
{
    NodeKey node;
    BlockReason reason = BlockReason::Cycle;
    /// The responsible dependency: the dangling or unbound reference, or the first dependency on a blocked
    /// node. Absent for Cycle, and when only the implicit Data-on-Geometry edge leads to a blocked node.
    std::optional<DependencyKey> via;
    /// For Cycle, the index of the node's cycle in EvaluationPlan::cycles.
    std::optional<std::size_t> cycle;

    bool operator==(const BlockedNode&) const = default;
};

struct EvaluationPlan
{
    /// The graph revision this plan describes.
    Revision revision;
    /// Evaluable nodes with every dependency before its dependents; ties in ascending NodeKey order, so the
    /// order does not depend on insertion history.
    std::vector<NodeKey> order;
    /// Nodes that must not be evaluated, in ascending NodeKey order.
    std::vector<BlockedNode> blocked;
    /// Every cycle in the graph: one shortest path per strongly connected component, starting at its smallest
    /// node, in ascending order of that node.
    std::vector<Cycle> cycles;
    /// Every dangling or unbound reference held by a planned node, including FallsBack ones, in key order.
    std::vector<ReferenceDiagnostic> references;
};

/// Dependencies between document objects, addressed by persistent identity rather than by name or position.
///
/// The graph is a value: a copy is an independent snapshot that worker threads may read concurrently, but
/// one graph is not synchronized, so mutate it from its owning thread only. A refused operation returns an
/// error and leaves the graph and its revision unchanged. Every successful mutation, including a rename,
/// advances revision(). A node's effective revision is the latest revision at which the node, its dependency
/// list, or anything it transitively depends on changed; renames do not change it because names are labels.
/// A result computed for a node at revision R stays valid while isCurrent(node, R) holds. Undo is a new
/// mutation, so results computed before the undone edit are recomputed rather than reused.
///
/// Mutations allocate, so std::bad_alloc is the only exception they can throw. The graph is derived state:
/// after such a failure, discard it and rebuild it from the document rather than continuing to use it.
///
/// The graph never reuses identities: an identity removed from the graph can return only through
/// restoreObject, so a dangling reference cannot silently bind to a different object.
/// Algorithms are iterative and O((nodes + edges) log nodes); deep chains do not recurse.
// The implicit move constructor can throw with MSVC's STL, whose std::map allocates a sentinel when moved.
class DependencyGraph // NOLINT(bugprone-exception-escape)
{
  public:
    /// The mutation counter. It starts at zero for an empty graph and never rewinds; 64 bits cannot be
    /// exhausted by edits.
    [[nodiscard]] Revision revision() const noexcept;

    DependencyResult<> addObject(ObjectId id, ObjectInfo info);
    /// Removes the object and its own dependencies. Other objects' references to it are kept as dangling
    /// references and listed in the result; their owners are invalidated.
    DependencyResult<RemovedObject> removeObject(ObjectId id);
    /// Undoes removeObject. The identity must have been removed from this graph. Its dependencies may refer
    /// to live or removed identities, and dangling references to it bind again. With CyclePolicy::Reject a
    /// restoration that would close a cycle (possible only when undo is applied out of order) is refused.
    DependencyResult<> restoreObject(RemovedObject removed, CyclePolicy policy = CyclePolicy::Reject);
    /// Changes the label only. No reference follows or loses a name, and no node is invalidated.
    DependencyResult<> renameObject(ObjectId id, std::string label);
    /// Adds a dependency to a live owner. A bound target must be live or removed (never unknown) and a
    /// removed target leaves the new reference dangling. With CyclePolicy::Reject, a cycle is refused.
    DependencyResult<> addDependency(NodeKey owner, Dependency dependency, CyclePolicy policy = CyclePolicy::Reject);
    /// Removes and returns a dependency, for an explicit and reversible edit.
    DependencyResult<Dependency> removeDependency(const DependencyKey& key);
    /// Records that the node's own inputs changed, such as a mark or a mass. Its dependents are invalidated.
    DependencyResult<> markChanged(NodeKey node);

    [[nodiscard]] bool contains(ObjectId id) const;
    /// True for an identity that was removed and not restored.
    [[nodiscard]] bool wasRemoved(ObjectId id) const;
    /// Borrowed until the next mutation; null when the object is not live.
    [[nodiscard]] const ObjectInfo* object(ObjectId id) const;
    [[nodiscard]] std::size_t objectCount() const noexcept;
    /// Live identities in ascending order.
    [[nodiscard]] std::vector<ObjectId> objects() const;
    /// The owner's dependencies in (kind, slot) order, borrowed until the next mutation; empty when not live.
    [[nodiscard]] std::span<const Dependency> dependencies(NodeKey owner) const;
    /// Borrowed until the next mutation; null when absent.
    [[nodiscard]] const Dependency* dependency(const DependencyKey& key) const;
    /// Bound references to either facet of the identity, live or dangling, in key order.
    [[nodiscard]] std::vector<DependencyKey> referencesTo(ObjectId target) const;
    /// The cycle that adding the dependency to the owner would close, without changing the graph. Lets an
    /// interface offer only acceptable targets.
    [[nodiscard]] std::optional<Cycle> cycleIfAdded(NodeKey owner, const Dependency& dependency) const;
    /// Absent when the node's object is not live.
    [[nodiscard]] std::optional<Revision> effectiveRevision(NodeKey node) const;
    /// True when the node is live and nothing it depends on changed after computedAt.
    [[nodiscard]] bool isCurrent(NodeKey node, Revision computedAt) const;
    /// The evaluation plan for every live node.
    [[nodiscard]] EvaluationPlan plan() const;
    /// The plan restricted to nodes invalidated after `since`: exactly the nodes that changed and their
    /// transitive dependents. Cycles are still reported for the whole graph.
    [[nodiscard]] EvaluationPlan planSince(Revision since) const;

  private:
    struct FacetRecord
    {
        Revision changed;
        Revision effective;
        std::vector<Dependency> dependencies;
    };

    struct ObjectRecord
    {
        ObjectInfo info;
        std::array<FacetRecord, FACET_COUNT> facets;
    };

    struct Successor
    {
        NodeKey node;
        std::optional<DependencyKey> via;
    };

    [[nodiscard]] const FacetRecord* facet(NodeKey node) const;
    [[nodiscard]] std::vector<Successor> successors(NodeKey node) const;
    /// A shortest path of at least one edge from `from` to `to` over live edges.
    [[nodiscard]] std::optional<std::vector<CycleEdge>> shortestPath(NodeKey from, NodeKey to) const;
    [[nodiscard]] std::optional<DependencyError> validate(NodeKey owner, const Dependency& dependency) const;
    void index(NodeKey owner, const Dependency& dependency);
    void unindex(NodeKey owner, const Dependency& dependency);
    Revision advance();
    void invalidate(std::span<const NodeKey> seeds, Revision revision);
    [[nodiscard]] EvaluationPlan makePlan(std::optional<Revision> since) const;

    std::map<ObjectId, ObjectRecord> m_Objects;
    /// Bound references by target node, kept while the target is removed so that restoration rebinds them.
    std::map<NodeKey, std::set<DependencyKey>> m_Dependents;
    std::set<ObjectId> m_Removed;
    Revision m_Revision;
};

/// A one-line path for logs and tests, e.g. `'CoM' #3 geometry -[member-of 0]-> 'A' #1 geometry -> ...`.
/// Removed objects are shown by identity only.
[[nodiscard]] std::string describeCycle(const DependencyGraph& graph, const Cycle& cycle);

} // namespace motrack::model
