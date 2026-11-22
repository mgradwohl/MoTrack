#include "model/dependencies/name_binding.hpp"

#include <algorithm>
#include <utility>

namespace motrack::model
{
namespace
{

enum class Eligibility : std::uint8_t
{
    Eligible,
    Excluded,
    Unclassified,
};

bool listed(const std::vector<std::string>& classes, std::string_view className)
{
    return std::ranges::find(classes, className) != classes.end();
}

Eligibility classify(const CandidateFilter& filter, std::string_view className)
{
    if (listed(filter.excludedClasses, className))
    {
        return Eligibility::Excluded;
    }
    return filter.eligibleClasses.empty() || listed(filter.eligibleClasses, className) ? Eligibility::Eligible : Eligibility::Unclassified;
}

} // namespace

std::optional<ObjectId> NameBinding::target() const
{
    return outcome == BindingOutcome::Unique && candidates.size() == 1 ? std::optional(candidates.front()) : std::nullopt;
}

NameIndex::NameIndex(std::span<const NamedObject> objects, NameIndexLimits limits) : m_RemainingVisits(limits.maxCandidateVisits)
{
    for (const auto& object : objects)
    {
        m_ByName[object.label].push_back(Candidate{.id = object.id, .className = object.className});
    }
}

NameIndex NameIndex::fromGraph(const DependencyGraph& graph, NameIndexLimits limits)
{
    std::vector<NamedObject> objects;
    objects.reserve(graph.objectCount());
    for (const auto id : graph.objects())
    {
        const auto* info = graph.object(id);
        objects.push_back(NamedObject{.id = id, .label = info->label, .className = info->className});
    }
    return NameIndex(objects, limits);
}

DependencyResult<NameBinding> NameIndex::bind(std::optional<std::string_view> savedName, const CandidateFilter& filter)
{
    NameBinding binding;
    if (!savedName)
    {
        binding.outcome = BindingOutcome::NoTarget;
        return binding;
    }
    const auto found = m_ByName.find(*savedName);
    if (found == m_ByName.end())
    {
        return binding;
    }
    // Charge every same-named candidate before filtering, including wrong-type ones.
    if (found->second.size() > m_RemainingVisits)
    {
        return DependencyError{.code = DependencyErrorCode::CandidateBudgetExceeded, .object = {}, .cycle = {}};
    }
    m_RemainingVisits -= found->second.size();
    bool unclassified = false;
    std::size_t eligible = 0;
    for (const auto& candidate : found->second)
    {
        switch (classify(filter, candidate.className))
        {
        case Eligibility::Excluded:
            continue;
        case Eligibility::Unclassified:
            unclassified = true;
            break;
        case Eligibility::Eligible:
            ++eligible;
            break;
        }
        binding.candidates.push_back(candidate.id);
    }
    if (unclassified)
    {
        binding.outcome = BindingOutcome::Unclassified;
    }
    else if (eligible > 0)
    {
        binding.outcome = eligible == 1 ? BindingOutcome::Unique : BindingOutcome::Ambiguous;
    }
    return binding;
}

std::size_t NameIndex::remainingCandidateVisits() const noexcept
{
    return m_RemainingVisits;
}

std::optional<Dependency> makeDependency(DependencyKind kind,
                                         std::uint32_t slot,
                                         Facet targetFacet,
                                         std::string_view savedName,
                                         const NameBinding& binding,
                                         MissingTarget onMissing)
{
    if (binding.outcome == BindingOutcome::NoTarget)
    {
        return std::nullopt;
    }
    // A Unique outcome without exactly one candidate stays unbound, and the graph then rejects it as invalid.
    const auto target = binding.target();
    auto dependency = target ? boundDependency(kind, slot, *target, targetFacet, onMissing)
                             : Dependency{.kind = kind,
                                          .slot = slot,
                                          .targetFacet = targetFacet,
                                          .target = std::nullopt,
                                          .savedName = {},
                                          .binding = binding.outcome,
                                          .candidates = binding.candidates,
                                          .onMissing = onMissing};
    dependency.savedName = savedName;
    return dependency;
}

DependencyResult<RenderedName>
renderName(const DependencyGraph& graph, const Dependency& dependency, NameIndex& current, const CandidateFilter& filter)
{
    RenderedName rendered;
    const auto* target = dependency.target ? graph.object(*dependency.target) : nullptr;
    const bool dangling = dependency.target && target == nullptr;
    rendered.name = target != nullptr ? target->label : dependency.savedName;
    // A dangling reference keeps no current label; a reload of its saved name is still reported.
    if (!dangling || !rendered.name.empty())
    {
        auto reload = current.bind(rendered.name, filter);
        if (!reload)
        {
            return reload.error();
        }
        rendered.reload = *std::move(reload);
    }
    if (dangling)
    {
        rendered.status = RenderStatus::Dangling;
        return rendered;
    }
    const auto expected = dependency.target ? std::vector<ObjectId>{*dependency.target} : dependency.candidates;
    const auto expectedOutcome = dependency.target ? BindingOutcome::Unique : dependency.binding;
    if (rendered.reload.outcome == expectedOutcome && rendered.reload.candidates == expected)
    {
        rendered.status = RenderStatus::Faithful;
    }
    else
    {
        switch (rendered.reload.outcome)
        {
        case BindingOutcome::Ambiguous:
            rendered.status = RenderStatus::Ambiguous;
            break;
        case BindingOutcome::Unclassified:
            rendered.status = RenderStatus::Unclassified;
            break;
        case BindingOutcome::Unique:
            rendered.status = RenderStatus::Rebinds;
            break;
        case BindingOutcome::Missing:
        case BindingOutcome::NoTarget:
            rendered.status = RenderStatus::Lost;
            break;
        }
    }
    return rendered;
}

} // namespace motrack::model
