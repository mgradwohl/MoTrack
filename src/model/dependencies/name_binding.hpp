#pragma once

#include "model/dependencies/dependency_graph.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/// Saved names become identities at load and current labels again at save (docs/dependency-graph.md).
/// Names are labels, not identity: nothing here follows a rename or chooses among duplicates by position.
namespace motrack::model
{

/// An object that a saved name could refer to, in source order.
struct NamedObject
{
    ObjectId id;
    /// Absent for an object without a readable name; it is not indexed, as in the source reference index.
    /// An empty name is a name.
    std::optional<std::string> label;
    /// The exact Tracker Java class.
    std::string className;
};

/// The type rule of one reference field, mirroring its Tracker loader. A candidate whose class is listed in
/// excludedClasses is skipped. Otherwise it is eligible when eligibleClasses is empty (a panel-wide lookup
/// such as a selection or plot guest) or lists its class, and unclassified when its class is not listed.
/// For example, CenterOfMass.masses accepts PointMass and excludes Vector, so a same-named center of mass or
/// model is Unclassified rather than a silent member.
struct CandidateFilter
{
    std::vector<std::string> eligibleClasses;
    std::vector<std::string> excludedClasses;
};

/// The load-time meaning of one saved name.
struct NameBinding
{
    BindingOutcome outcome = BindingOutcome::Missing;
    /// Unique: the target. Ambiguous: every eligible candidate. Unclassified: every candidate that is not
    /// excluded. Missing and NoTarget: empty. Candidates keep the index's source order.
    std::vector<ObjectId> candidates;

    /// The target of a Unique binding with exactly one candidate.
    [[nodiscard]] std::optional<ObjectId> target() const;

    bool operator==(const NameBinding&) const = default;
};

struct NameIndexLimits
{
    /// Same-name candidates that bind() may visit in total, charged before filtering. Duplicate names would
    /// otherwise allow tracks times references visits; this matches the source reference index's budget.
    std::size_t maxCandidateVisits = 100000;
};

/// Exact, case-sensitive lookup of one name scope, such as the tracks of one Tracker panel. Build one index
/// per scope, from every object in it including unsupported ones, so that a possible capture is reported.
///
/// Filters reproduce the conservative source reference index, not every Tracker loader: a center of mass
/// of models, or a model boosted by another model, binds Unclassified here, although Tracker accepts any
/// PointMass subclass. The C4 and C5 families widen those filters with their application cases.
class NameIndex
{
  public:
    explicit NameIndex(std::span<const NamedObject> objects, NameIndexLimits limits = {});
    /// Every live object of the graph, in ascending identity order.
    [[nodiscard]] static NameIndex fromGraph(const DependencyGraph& graph, NameIndexLimits limits = {});

    /// Resolves one saved name; std::nullopt is an explicitly empty slot (NoTarget). Unique when exactly one
    /// eligible candidate and no unclassified one exists; Unclassified when any same-named candidate has an
    /// unverified class; otherwise Ambiguous or Missing. Fails only when the visit budget is exhausted.
    [[nodiscard]] DependencyResult<NameBinding> bind(std::optional<std::string_view> savedName, const CandidateFilter& filter);
    [[nodiscard]] std::size_t remainingCandidateVisits() const noexcept;

  private:
    struct Candidate
    {
        ObjectId id;
        std::string className;
    };

    struct NameHash
    {
        using is_transparent = void; // NOLINT(readability-identifier-naming): the standard heterogeneous-lookup tag.

        std::size_t operator()(std::string_view name) const noexcept
        {
            return std::hash<std::string_view>{}(name);
        }
    };

    std::unordered_map<std::string, std::vector<Candidate>, NameHash, std::equal_to<>> m_ByName;
    std::size_t m_RemainingVisits;
};

/// Converts a load-time binding into a graph dependency: bound when Unique, otherwise an unbound reference
/// that keeps the saved name, outcome and candidates. Returns std::nullopt for NoTarget, because an empty
/// slot has no dependency (its slot ordinal remains the typed model's data).
[[nodiscard]] std::optional<Dependency> makeDependency(DependencyKind kind,
                                                       std::uint32_t slot,
                                                       Facet targetFacet,
                                                       std::string_view savedName,
                                                       const NameBinding& binding,
                                                       MissingTarget onMissing = MissingTarget::Blocks);

/// Whether writing a reference's name keeps its meaning when Tracker reloads the file.
enum class RenderStatus : std::uint8_t
{
    /// A reload binds the same target, or reproduces the same unresolved outcome and the same set of candidates, in
    /// whatever order the index lists them.
    Faithful,
    /// More than one eligible object now has the name.
    Ambiguous,
    /// A same-named object of an unverified class could capture the reference.
    Unclassified,
    /// A reload would bind a different object, such as an unresolved name that now resolves uniquely.
    Rebinds,
    /// A reload would find no eligible target although the reference has one.
    Lost,
    /// The bound target is not live, so there is no current name. The saved name, possibly empty, is returned.
    Dangling,
    /// The bound target is live but has no name to write.
    Unnamed,
};

struct RenderedName
{
    std::string name;
    RenderStatus status = RenderStatus::Faithful;
    /// What a reload of `name` would bind to in the current index.
    NameBinding reload;
};

/// Renders a reference at save: a bound target's current label, or an unresolved reference's saved name.
/// `current` must index the scope being written (for example NameIndex::fromGraph of that panel), and the
/// filter must be the field's own. Writers write the name only when the status is Faithful and must refuse
/// or make an explicit edit otherwise; the name is never patched by text replacement.
[[nodiscard]] DependencyResult<RenderedName>
renderName(const DependencyGraph& graph, const Dependency& dependency, NameIndex& current, const CandidateFilter& filter);

} // namespace motrack::model
