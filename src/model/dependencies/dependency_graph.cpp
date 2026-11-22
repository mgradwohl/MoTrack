#include "model/dependencies/dependency_graph.hpp"

#include <queue>

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <unordered_map>

namespace motrack::model
{
namespace
{

constexpr std::array<Facet, FACET_COUNT> FACETS{Facet::Definition, Facet::Geometry, Facet::Data};

constexpr std::size_t facetIndex(Facet facet) noexcept
{
    return static_cast<std::size_t>(facet);
}

std::string_view facetName(Facet facet) noexcept
{
    switch (facet)
    {
    case Facet::Definition:
        return "definition";
    case Facet::Geometry:
        return "geometry";
    case Facet::Data:
        return "data";
    }
    return "unknown";
}

/// The facet an object's node implicitly depends on, if any: Geometry reads Definition, Data reads Geometry.
std::optional<Facet> previousFacet(Facet facet) noexcept
{
    switch (facet)
    {
    case Facet::Geometry:
        return Facet::Definition;
    case Facet::Data:
        return Facet::Geometry;
    case Facet::Definition:
        break;
    }
    return std::nullopt;
}

/// The facet that implicitly depends on this one, if any.
std::optional<Facet> nextFacet(Facet facet) noexcept
{
    switch (facet)
    {
    case Facet::Definition:
        return Facet::Geometry;
    case Facet::Geometry:
        return Facet::Data;
    case Facet::Data:
        break;
    }
    return std::nullopt;
}

std::pair<DependencyKind, std::uint32_t> slotKey(const Dependency& dependency) noexcept
{
    return {dependency.kind, dependency.slot};
}

template <typename List> auto findSlot(List& list, DependencyKind kind, std::uint32_t slot)
{
    const auto found = std::ranges::lower_bound(list, std::pair(kind, slot), {}, slotKey);
    return found != list.end() && found->kind == kind && found->slot == slot ? found : list.end();
}

DependencyError failure(DependencyErrorCode code, ObjectId object = {})
{
    return DependencyError{.code = code, .object = object, .cycle = {}};
}

DependencyKey keyOf(NodeKey owner, const Dependency& dependency) noexcept
{
    return DependencyKey{.owner = owner, .kind = dependency.kind, .slot = dependency.slot};
}

} // namespace

std::string_view dependencyKindName(DependencyKind kind) noexcept
{
    switch (kind)
    {
    case DependencyKind::MemberOf:
        return "member-of";
    case DependencyKind::AttachedTo:
        return "attached-to";
    case DependencyKind::ReferencesDataOf:
        return "references-data-of";
    case DependencyKind::FrameOf:
        return "frame-of";
    case DependencyKind::BoosterOf:
        return "booster-of";
    case DependencyKind::FunctionOf:
        return "function-of";
    }
    return "unknown";
}

std::string_view errorMessage(DependencyErrorCode code) noexcept
{
    switch (code)
    {
    case DependencyErrorCode::InvalidObjectId:
        return "Object identity zero is reserved.";
    case DependencyErrorCode::DuplicateObject:
        return "The object is already in the dependency graph.";
    case DependencyErrorCode::IdentityReused:
        return "A removed object's identity cannot be reused.";
    case DependencyErrorCode::UnknownObject:
        return "The object is not in the dependency graph.";
    case DependencyErrorCode::InvalidDependency:
        return "The dependency's target and name binding are inconsistent, or a definition reads a later facet.";
    case DependencyErrorCode::DuplicateDependency:
        return "The owner already has a dependency in this slot.";
    case DependencyErrorCode::UnknownDependency:
        return "The owner has no dependency in this slot.";
    case DependencyErrorCode::CycleRejected:
        return "The change would create a dependency cycle.";
    case DependencyErrorCode::CandidateBudgetExceeded:
        return "Too many same-name candidates to bind.";
    }
    return "Unknown dependency error.";
}

Dependency boundDependency(DependencyKind kind, std::uint32_t slot, ObjectId target, Facet targetFacet, MissingTarget onMissing)
{
    return Dependency{.kind = kind,
                      .slot = slot,
                      .targetFacet = targetFacet,
                      .target = target,
                      .savedName = {},
                      .binding = BindingOutcome::Unique,
                      .candidates = {},
                      .onMissing = onMissing};
}

Revision DependencyGraph::revision() const noexcept
{
    return m_Revision;
}

Revision DependencyGraph::advance()
{
    ++m_Revision.value; // One increment per edit cannot wrap 64 bits.
    return m_Revision;
}

const DependencyGraph::FacetRecord* DependencyGraph::facet(NodeKey node) const
{
    const auto found = m_Objects.find(node.object);
    return found == m_Objects.end() ? nullptr : &found->second.facets[facetIndex(node.facet)];
}

std::vector<DependencyGraph::Successor> DependencyGraph::successors(NodeKey node) const
{
    std::vector<Successor> result;
    const auto* record = facet(node);
    if (record == nullptr)
    {
        return result;
    }
    if (const auto previous = previousFacet(node.facet))
    {
        result.push_back(Successor{.node = {.object = node.object, .facet = *previous}, .via = std::nullopt});
    }
    for (const auto& dependency : record->dependencies)
    {
        if (dependency.target && contains(*dependency.target))
        {
            result.push_back(
                Successor{.node = {.object = *dependency.target, .facet = dependency.targetFacet}, .via = keyOf(node, dependency)});
        }
    }
    return result;
}

std::optional<std::vector<CycleEdge>> DependencyGraph::shortestPath(NodeKey from, NodeKey to) const
{
    std::map<NodeKey, CycleEdge> reachedBy;
    std::deque<NodeKey> queue{from};
    while (!queue.empty())
    {
        const auto node = queue.front();
        queue.pop_front();
        for (const auto& next : successors(node))
        {
            const CycleEdge edge{.from = node, .to = next.node, .dependency = next.via};
            if (next.node == to)
            {
                std::vector<CycleEdge> path{edge};
                for (auto at = node; at != from;)
                {
                    const auto& previous = reachedBy.at(at);
                    path.push_back(previous);
                    at = previous.from;
                }
                std::ranges::reverse(path);
                return path;
            }
            if (next.node != from && reachedBy.emplace(next.node, edge).second)
            {
                queue.push_back(next.node);
            }
        }
    }
    return std::nullopt;
}

std::optional<DependencyError> DependencyGraph::validate(NodeKey owner, const Dependency& dependency) const
{
    const bool consistent =
        dependency.target ? dependency.target->isValid() && dependency.binding == BindingOutcome::Unique && dependency.candidates.empty()
                          : dependency.binding == BindingOutcome::Ambiguous || dependency.binding == BindingOutcome::Missing ||
                                dependency.binding == BindingOutcome::Unclassified;
    // A Definition holds coordinate-independent inputs, so it may read only other Definitions; otherwise a
    // later reader of the input would silently inherit a frame dependency.
    const bool inputOnly = owner.facet != Facet::Definition || dependency.targetFacet == Facet::Definition;
    if (!consistent || !inputOnly)
    {
        return failure(DependencyErrorCode::InvalidDependency, owner.object);
    }
    if (dependency.target && !contains(*dependency.target) && !wasRemoved(*dependency.target))
    {
        return failure(DependencyErrorCode::UnknownObject, *dependency.target);
    }
    return std::nullopt;
}

void DependencyGraph::index(NodeKey owner, const Dependency& dependency)
{
    if (dependency.target)
    {
        m_Dependents[NodeKey{.object = *dependency.target, .facet = dependency.targetFacet}].insert(keyOf(owner, dependency));
    }
}

void DependencyGraph::unindex(NodeKey owner, const Dependency& dependency)
{
    if (!dependency.target)
    {
        return;
    }
    const NodeKey target{.object = *dependency.target, .facet = dependency.targetFacet};
    auto& owners = m_Dependents[target];
    owners.erase(keyOf(owner, dependency));
    if (owners.empty())
    {
        m_Dependents.erase(target);
    }
}

void DependencyGraph::invalidate(std::span<const NodeKey> seeds, Revision revision)
{
    std::deque<NodeKey> queue;
    const auto touch = [&](NodeKey node)
    {
        const auto found = m_Objects.find(node.object);
        if (found == m_Objects.end())
        {
            return;
        }
        auto& record = found->second.facets[facetIndex(node.facet)];
        if (record.effective != revision)
        {
            record.effective = revision;
            queue.push_back(node);
        }
    };
    for (const auto seed : seeds)
    {
        touch(seed);
    }
    while (!queue.empty())
    {
        const auto node = queue.front();
        queue.pop_front();
        if (const auto next = nextFacet(node.facet))
        {
            touch(NodeKey{.object = node.object, .facet = *next});
        }
        if (const auto found = m_Dependents.find(node); found != m_Dependents.end())
        {
            for (const auto& key : found->second)
            {
                touch(key.owner);
            }
        }
    }
}

DependencyResult<> DependencyGraph::addObject(ObjectId id, ObjectInfo info)
{
    if (!id.isValid())
    {
        return failure(DependencyErrorCode::InvalidObjectId);
    }
    if (contains(id))
    {
        return failure(DependencyErrorCode::DuplicateObject, id);
    }
    if (wasRemoved(id))
    {
        return failure(DependencyErrorCode::IdentityReused, id);
    }
    const auto revision = advance();
    ObjectRecord record{.info = std::move(info), .facets = {}};
    for (auto& facetRecord : record.facets)
    {
        facetRecord.effective = revision;
    }
    m_Objects.emplace(id, std::move(record));
    return std::monostate{};
}

DependencyResult<RemovedObject> DependencyGraph::removeObject(ObjectId id)
{
    const auto found = m_Objects.find(id);
    if (found == m_Objects.end())
    {
        return failure(DependencyErrorCode::UnknownObject, id);
    }
    auto& facets = found->second.facets;
    RemovedObject removed{.id = id,
                          .info = std::move(found->second.info),
                          .definitionDependencies = std::move(facets[facetIndex(Facet::Definition)].dependencies),
                          .geometryDependencies = std::move(facets[facetIndex(Facet::Geometry)].dependencies),
                          .dataDependencies = std::move(facets[facetIndex(Facet::Data)].dependencies),
                          .danglingReferences = {}};
    m_Objects.erase(found);
    const std::array<const std::vector<Dependency>*, FACET_COUNT> own{
        &removed.definitionDependencies, &removed.geometryDependencies, &removed.dataDependencies};
    for (const auto facetValue : FACETS)
    {
        for (const auto& dependency : *own[facetIndex(facetValue)])
        {
            unindex(NodeKey{.object = id, .facet = facetValue}, dependency);
        }
    }
    m_Removed.insert(id);
    for (const auto facetValue : FACETS)
    {
        if (const auto dependents = m_Dependents.find(NodeKey{.object = id, .facet = facetValue}); dependents != m_Dependents.end())
        {
            removed.danglingReferences.insert(removed.danglingReferences.end(), dependents->second.begin(), dependents->second.end());
        }
    }
    std::ranges::sort(removed.danglingReferences);
    const auto revision = advance();
    std::vector<NodeKey> owners;
    owners.reserve(removed.danglingReferences.size());
    for (const auto& reference : removed.danglingReferences)
    {
        owners.push_back(reference.owner);
    }
    invalidate(owners, revision);
    return removed;
}

DependencyResult<> DependencyGraph::restoreObject(const RemovedObject& removed, CyclePolicy policy)
{
    const auto id = removed.id;
    if (!id.isValid())
    {
        return failure(DependencyErrorCode::InvalidObjectId);
    }
    if (contains(id))
    {
        return failure(DependencyErrorCode::DuplicateObject, id);
    }
    if (!wasRemoved(id))
    {
        return failure(DependencyErrorCode::UnknownObject, id);
    }
    ObjectRecord record{.info = removed.info, .facets = {}};
    record.facets[facetIndex(Facet::Definition)].dependencies = removed.definitionDependencies;
    record.facets[facetIndex(Facet::Geometry)].dependencies = removed.geometryDependencies;
    record.facets[facetIndex(Facet::Data)].dependencies = removed.dataDependencies;
    for (const auto facetValue : FACETS)
    {
        auto& list = record.facets[facetIndex(facetValue)].dependencies;
        for (const auto& dependency : list)
        {
            if (auto error = validate(NodeKey{.object = id, .facet = facetValue}, dependency))
            {
                return *error;
            }
        }
        std::ranges::sort(list, {}, slotKey);
        if (std::ranges::adjacent_find(list, {}, slotKey) != list.end())
        {
            return failure(DependencyErrorCode::DuplicateDependency, id);
        }
    }
    auto& restored = m_Objects.emplace(id, std::move(record)).first->second;
    const auto forEachOwn = [&](const auto& apply)
    {
        for (const auto facetValue : FACETS)
        {
            for (const auto& dependency : restored.facets[facetIndex(facetValue)].dependencies)
            {
                apply(NodeKey{.object = id, .facet = facetValue}, dependency);
            }
        }
    };
    forEachOwn([this](NodeKey owner, const Dependency& dependency) { index(owner, dependency); });
    if (policy == CyclePolicy::Reject)
    {
        for (const auto facetValue : FACETS)
        {
            const NodeKey node{.object = id, .facet = facetValue};
            if (auto path = shortestPath(node, node))
            {
                forEachOwn([this](NodeKey owner, const Dependency& dependency) { unindex(owner, dependency); });
                m_Objects.erase(id);
                return DependencyError{.code = DependencyErrorCode::CycleRejected, .object = id, .cycle = Cycle{.edges = std::move(*path)}};
            }
        }
    }
    m_Removed.erase(id);
    const auto revision = advance();
    const NodeKey seed{.object = id, .facet = Facet::Definition};
    invalidate(std::span(&seed, 1), revision);
    return std::monostate{};
}

DependencyResult<> DependencyGraph::retireIdentity(ObjectId id)
{
    if (!id.isValid())
    {
        return failure(DependencyErrorCode::InvalidObjectId);
    }
    if (contains(id))
    {
        return failure(DependencyErrorCode::DuplicateObject, id);
    }
    advance();
    m_Removed.insert(id);
    return std::monostate{};
}
DependencyResult<> DependencyGraph::renameObject(ObjectId id, std::string label)
{
    const auto found = m_Objects.find(id);
    if (found == m_Objects.end())
    {
        return failure(DependencyErrorCode::UnknownObject, id);
    }
    advance();
    found->second.info.label = std::move(label);
    return std::monostate{};
}

DependencyResult<> DependencyGraph::addDependency(NodeKey owner, Dependency dependency, CyclePolicy policy)
{
    const auto found = m_Objects.find(owner.object);
    if (found == m_Objects.end())
    {
        return failure(DependencyErrorCode::UnknownObject, owner.object);
    }
    if (auto error = validate(owner, dependency))
    {
        return *error;
    }
    auto& record = found->second.facets[facetIndex(owner.facet)];
    const auto position = std::ranges::lower_bound(record.dependencies, slotKey(dependency), {}, slotKey);
    if (position != record.dependencies.end() && slotKey(*position) == slotKey(dependency))
    {
        return failure(DependencyErrorCode::DuplicateDependency, owner.object);
    }
    if (policy == CyclePolicy::Reject)
    {
        if (auto cycle = cycleIfAdded(owner, dependency))
        {
            return DependencyError{.code = DependencyErrorCode::CycleRejected, .object = owner.object, .cycle = std::move(*cycle)};
        }
    }
    index(owner, dependency);
    record.dependencies.insert(position, std::move(dependency));
    const auto revision = advance();
    invalidate(std::span(&owner, 1), revision);
    return std::monostate{};
}

DependencyResult<Dependency> DependencyGraph::removeDependency(const DependencyKey& key)
{
    const auto found = m_Objects.find(key.owner.object);
    if (found == m_Objects.end())
    {
        return failure(DependencyErrorCode::UnknownObject, key.owner.object);
    }
    auto& record = found->second.facets[facetIndex(key.owner.facet)];
    const auto position = findSlot(record.dependencies, key.kind, key.slot);
    if (position == record.dependencies.end())
    {
        return failure(DependencyErrorCode::UnknownDependency, key.owner.object);
    }
    auto removed = std::move(*position);
    record.dependencies.erase(position);
    unindex(key.owner, removed);
    const auto revision = advance();
    invalidate(std::span(&key.owner, 1), revision);
    return removed;
}

DependencyResult<> DependencyGraph::markChanged(NodeKey node)
{
    const auto found = m_Objects.find(node.object);
    if (found == m_Objects.end())
    {
        return failure(DependencyErrorCode::UnknownObject, node.object);
    }
    const auto revision = advance();
    invalidate(std::span(&node, 1), revision);
    return std::monostate{};
}

bool DependencyGraph::contains(ObjectId id) const
{
    return m_Objects.contains(id);
}

bool DependencyGraph::wasRemoved(ObjectId id) const
{
    return m_Removed.contains(id);
}

const ObjectInfo* DependencyGraph::object(ObjectId id) const
{
    const auto found = m_Objects.find(id);
    return found == m_Objects.end() ? nullptr : &found->second.info;
}

std::size_t DependencyGraph::objectCount() const noexcept
{
    return m_Objects.size();
}

std::vector<ObjectId> DependencyGraph::objects() const
{
    std::vector<ObjectId> result;
    result.reserve(m_Objects.size());
    for (const auto& entry : m_Objects)
    {
        result.push_back(entry.first);
    }
    return result;
}

std::span<const Dependency> DependencyGraph::dependencies(NodeKey owner) const
{
    const auto* record = facet(owner);
    return record == nullptr ? std::span<const Dependency>{} : std::span<const Dependency>{record->dependencies};
}

const Dependency* DependencyGraph::dependency(const DependencyKey& key) const
{
    const auto* record = facet(key.owner);
    if (record == nullptr)
    {
        return nullptr;
    }
    const auto found = findSlot(record->dependencies, key.kind, key.slot);
    return found == record->dependencies.end() ? nullptr : &*found;
}

std::vector<DependencyKey> DependencyGraph::referencesTo(ObjectId target) const
{
    std::vector<DependencyKey> result;
    for (const auto facetValue : FACETS)
    {
        if (const auto found = m_Dependents.find(NodeKey{.object = target, .facet = facetValue}); found != m_Dependents.end())
        {
            result.insert(result.end(), found->second.begin(), found->second.end());
        }
    }
    std::ranges::sort(result);
    return result;
}

std::optional<Cycle> DependencyGraph::cycleIfAdded(NodeKey owner, const Dependency& dependency) const
{
    if (!contains(owner.object) || !dependency.target || !contains(*dependency.target))
    {
        return std::nullopt;
    }
    const NodeKey target{.object = *dependency.target, .facet = dependency.targetFacet};
    Cycle cycle{.edges = {CycleEdge{.from = owner, .to = target, .dependency = keyOf(owner, dependency)}}};
    if (target == owner)
    {
        return cycle;
    }
    auto path = shortestPath(target, owner);
    if (!path)
    {
        return std::nullopt;
    }
    cycle.edges.insert(cycle.edges.end(), path->begin(), path->end());
    return cycle;
}

std::optional<Revision> DependencyGraph::effectiveRevision(NodeKey node) const
{
    const auto* record = facet(node);
    return record == nullptr ? std::nullopt : std::optional(record->effective);
}

bool DependencyGraph::isCurrent(NodeKey node, Revision computedAt) const
{
    const auto effective = effectiveRevision(node);
    return effective && *effective <= computedAt;
}

EvaluationPlan DependencyGraph::plan() const
{
    return makePlan(std::nullopt);
}

EvaluationPlan DependencyGraph::planSince(Revision since) const
{
    return makePlan(since);
}

EvaluationPlan DependencyGraph::makePlan(std::optional<Revision> since) const
{
    EvaluationPlan plan{.revision = m_Revision, .order = {}, .blocked = {}, .cycles = {}, .references = {}};

    // Two nodes per object in ascending NodeKey order, so index order is key order.
    std::vector<NodeKey> nodes;
    nodes.reserve(m_Objects.size() * FACET_COUNT);
    std::unordered_map<ObjectId, std::size_t> firstNode;
    firstNode.reserve(m_Objects.size());
    for (const auto& entry : m_Objects)
    {
        firstNode.emplace(entry.first, nodes.size());
        for (const auto facetValue : FACETS)
        {
            nodes.push_back(NodeKey{.object = entry.first, .facet = facetValue});
        }
    }
    const auto count = nodes.size();
    struct Edge
    {
        std::size_t to;
        std::optional<DependencyKey> via;
    };
    std::vector<std::vector<Edge>> out(count);
    std::vector<std::vector<std::size_t>> in(count);
    for (std::size_t node = 0; node < count; ++node)
    {
        for (const auto& next : successors(nodes[node]))
        {
            const auto to = firstNode.at(next.node.object) + facetIndex(next.node.facet);
            out[node].push_back(Edge{.to = to, .via = next.via});
            in[to].push_back(node);
        }
    }

    // Tarjan's strongly connected components, iteratively so long chains cannot exhaust the stack.
    constexpr auto UNVISITED = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> discovery(count, UNVISITED);
    std::vector<std::size_t> low(count, 0);
    std::vector<std::size_t> component(count, UNVISITED);
    std::vector<bool> onStack(count, false);
    std::vector<std::size_t> stack;
    std::vector<std::vector<std::size_t>> components;
    struct Frame
    {
        std::size_t node;
        std::size_t next;
    };
    std::vector<Frame> frames;
    std::size_t visited = 0;
    const auto enter = [&](std::size_t node)
    {
        discovery[node] = visited;
        low[node] = visited;
        ++visited;
        stack.push_back(node);
        onStack[node] = true;
        frames.push_back(Frame{.node = node, .next = 0});
    };
    for (std::size_t root = 0; root < count; ++root)
    {
        if (discovery[root] != UNVISITED)
        {
            continue;
        }
        enter(root);
        while (!frames.empty())
        {
            const auto node = frames.back().node;
            if (frames.back().next < out[node].size())
            {
                const auto to = out[node][frames.back().next++].to;
                if (discovery[to] == UNVISITED)
                {
                    enter(to);
                }
                else if (onStack[to])
                {
                    low[node] = std::min(low[node], discovery[to]);
                }
                continue;
            }
            if (low[node] == discovery[node])
            {
                std::vector<std::size_t> members;
                std::size_t member = UNVISITED;
                do
                {
                    member = stack.back();
                    stack.pop_back();
                    onStack[member] = false;
                    component[member] = components.size();
                    members.push_back(member);
                } while (member != node);
                components.push_back(std::move(members));
            }
            frames.pop_back();
            if (!frames.empty())
            {
                low[frames.back().node] = std::min(low[frames.back().node], low[node]);
            }
        }
    }

    // One shortest closed path per cyclic component, from its smallest node.
    std::vector<std::size_t> cycleStarts;
    for (const auto& members : components)
    {
        const auto start = std::ranges::min(members);
        if (members.size() > 1 || std::ranges::any_of(out[start], [&](const Edge& edge) { return edge.to == start; }))
        {
            cycleStarts.push_back(start);
        }
    }
    std::ranges::sort(cycleStarts);
    std::vector<std::optional<std::size_t>> cycleOf(count);
    for (const auto start : cycleStarts)
    {
        const auto cycleIndex = plan.cycles.size();
        for (const auto member : components[component[start]])
        {
            cycleOf[member] = cycleIndex;
        }
        std::unordered_map<std::size_t, std::pair<std::size_t, std::size_t>> reachedBy; // node -> (previous, edge)
        std::deque<std::size_t> queue{start};
        Cycle cycle;
        while (!queue.empty() && cycle.edges.empty())
        {
            const auto node = queue.front();
            queue.pop_front();
            for (std::size_t edge = 0; edge < out[node].size(); ++edge)
            {
                const auto to = out[node][edge].to;
                if (component[to] != component[start])
                {
                    continue;
                }
                if (to == start)
                {
                    cycle.edges.push_back(CycleEdge{.from = nodes[node], .to = nodes[start], .dependency = out[node][edge].via});
                    for (auto at = node; at != start;)
                    {
                        const auto [previous, previousEdge] = reachedBy.at(at);
                        cycle.edges.push_back(
                            CycleEdge{.from = nodes[previous], .to = nodes[at], .dependency = out[previous][previousEdge].via});
                        at = previous;
                    }
                    std::ranges::reverse(cycle.edges);
                    break;
                }
                if (reachedBy.emplace(to, std::pair(node, edge)).second)
                {
                    queue.push_back(to);
                }
            }
        }
        plan.cycles.push_back(std::move(cycle));
    }

    // Root causes, then everything that transitively depends on them.
    std::vector<std::optional<BlockedNode>> blocked(count);
    for (std::size_t node = 0; node < count; ++node)
    {
        if (cycleOf[node])
        {
            blocked[node] = BlockedNode{.node = nodes[node], .reason = BlockReason::Cycle, .via = std::nullopt, .cycle = cycleOf[node]};
            continue;
        }
        for (const auto& dependency : facet(nodes[node])->dependencies)
        {
            if (dependency.onMissing != MissingTarget::Blocks || (dependency.target && contains(*dependency.target)))
            {
                continue;
            }
            blocked[node] = BlockedNode{.node = nodes[node],
                                        .reason = dependency.target ? BlockReason::DanglingReference : BlockReason::UnboundReference,
                                        .via = keyOf(nodes[node], dependency),
                                        .cycle = std::nullopt};
            break;
        }
    }
    std::deque<std::size_t> queue;
    for (std::size_t node = 0; node < count; ++node)
    {
        if (blocked[node])
        {
            queue.push_back(node);
        }
    }
    while (!queue.empty())
    {
        const auto node = queue.front();
        queue.pop_front();
        for (const auto dependent : in[node])
        {
            if (!blocked[dependent])
            {
                blocked[dependent] = BlockedNode{.node = nodes[dependent], .reason = BlockReason::DependsOnBlocked, .via = {}, .cycle = {}};
                queue.push_back(dependent);
            }
        }
    }
    for (std::size_t node = 0; node < count; ++node)
    {
        if (blocked[node] && blocked[node]->reason == BlockReason::DependsOnBlocked)
        {
            // Prefer an explicit dependency; an implicit edge within the object has no key to report.
            const auto edge = std::ranges::find_if(
                out[node], [&](const Edge& candidate) { return candidate.via.has_value() && blocked[candidate.to].has_value(); });
            blocked[node]->via = edge == out[node].end() ? std::nullopt : edge->via;
        }
    }

    std::vector<bool> planned(count, true);
    if (since)
    {
        for (std::size_t node = 0; node < count; ++node)
        {
            planned[node] = facet(nodes[node])->effective > *since;
        }
    }
    for (std::size_t node = 0; node < count; ++node)
    {
        if (!planned[node])
        {
            continue;
        }
        if (blocked[node])
        {
            plan.blocked.push_back(*blocked[node]);
        }
        for (const auto& dependency : facet(nodes[node])->dependencies)
        {
            if (!dependency.target || !contains(*dependency.target))
            {
                plan.references.push_back(
                    ReferenceDiagnostic{.key = keyOf(nodes[node], dependency),
                                        .state = dependency.target ? ReferenceState::Dangling : ReferenceState::Unbound,
                                        .dependency = dependency});
            }
        }
    }

    // Kahn's algorithm over the evaluable planned nodes. Their dependencies are never blocked, and a planned
    // node's dependents are planned too, so unplanned dependencies are simply up to date.
    std::vector<std::size_t> remaining(count, 0);
    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
    const auto evaluable = [&](std::size_t node)
    {
        return planned[node] && !blocked[node];
    };
    for (std::size_t node = 0; node < count; ++node)
    {
        if (!evaluable(node))
        {
            continue;
        }
        remaining[node] = static_cast<std::size_t>(std::ranges::count_if(out[node], [&](const Edge& edge) { return planned[edge.to]; }));
        if (remaining[node] == 0)
        {
            ready.push(node);
        }
    }
    while (!ready.empty())
    {
        const auto node = ready.top();
        ready.pop();
        plan.order.push_back(nodes[node]);
        for (const auto dependent : in[node])
        {
            if (evaluable(dependent) && --remaining[dependent] == 0)
            {
                ready.push(dependent);
            }
        }
    }
    return plan;
}

std::string describeCycle(const DependencyGraph& graph, const Cycle& cycle)
{
    const auto describe = [&](NodeKey node)
    {
        std::string text;
        if (const auto* info = graph.object(node.object); info != nullptr && info->label)
        {
            text = "'" + *info->label + "' ";
        }
        return text + "#" + std::to_string(node.object.value) + " " + std::string(facetName(node.facet));
    };
    if (cycle.edges.empty())
    {
        return {};
    }
    auto text = describe(cycle.edges.front().from);
    for (const auto& edge : cycle.edges)
    {
        text += edge.dependency
                    ? " -[" + std::string(dependencyKindName(edge.dependency->kind)) + " " + std::to_string(edge.dependency->slot) + "]-> "
                    : " -[own " + std::string(facetName(edge.to.facet)) + "]-> ";
        text += describe(edge.to);
    }
    return text;
}

} // namespace motrack::model
