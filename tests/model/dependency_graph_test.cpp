#include "model/dependencies/dependency_graph.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace
{

using motrack::model::BindingOutcome;
using motrack::model::BlockedNode;
using motrack::model::BlockReason;
using motrack::model::boundDependency;
using motrack::model::Cycle;
using motrack::model::CycleEdge;
using motrack::model::CyclePolicy;
using motrack::model::Dependency;
using motrack::model::DependencyErrorCode;
using motrack::model::DependencyGraph;
using motrack::model::DependencyKey;
using motrack::model::DependencyKind;
using motrack::model::DependencyResult;
using motrack::model::EvaluationPlan;
using motrack::model::Facet;
using motrack::model::MissingTarget;
using motrack::model::NodeKey;
using motrack::model::ObjectId;
using motrack::model::ObjectInfo;
using motrack::model::ReferenceState;
using motrack::model::RemovedObject;
using motrack::model::Revision;

constexpr std::string_view POINT_MASS = "org.opensourcephysics.cabrillo.tracker.PointMass";
constexpr std::string_view CENTER_OF_MASS = "org.opensourcephysics.cabrillo.tracker.CenterOfMass";
constexpr std::string_view DYNAMIC_PARTICLE = "org.opensourcephysics.cabrillo.tracker.DynamicParticle";

constexpr ObjectId id(std::uint64_t value)
{
    return ObjectId{value};
}

constexpr NodeKey definition(std::uint64_t value)
{
    return NodeKey{.object = ObjectId{value}, .facet = Facet::Definition};
}

constexpr NodeKey geometry(std::uint64_t value)
{
    return NodeKey{.object = ObjectId{value}, .facet = Facet::Geometry};
}

constexpr NodeKey data(std::uint64_t value)
{
    return NodeKey{.object = ObjectId{value}, .facet = Facet::Data};
}

DependencyKey key(NodeKey owner, DependencyKind kind, std::uint32_t slot = 0)
{
    return DependencyKey{.owner = owner, .kind = kind, .slot = slot};
}

void add(DependencyGraph& graph, std::uint64_t value, std::string label = {}, std::string_view className = POINT_MASS)
{
    ASSERT_TRUE(graph.addObject(id(value), ObjectInfo{.label = std::move(label), .className = std::string(className)}));
}

Dependency on(NodeKey target, DependencyKind kind = DependencyKind::MemberOf, std::uint32_t slot = 0)
{
    return boundDependency(kind, slot, target.object, target.facet);
}

void depend(DependencyGraph& graph, NodeKey owner, NodeKey target, DependencyKind kind = DependencyKind::MemberOf, std::uint32_t slot = 0)
{
    ASSERT_TRUE(graph.addDependency(owner, on(target, kind, slot)));
}

Dependency unbound(BindingOutcome outcome, std::string name, std::vector<ObjectId> candidates = {}, std::uint32_t slot = 0)
{
    return Dependency{.kind = DependencyKind::MemberOf,
                      .slot = slot,
                      .targetFacet = Facet::Geometry,
                      .target = std::nullopt,
                      .savedName = std::move(name),
                      .binding = outcome,
                      .candidates = std::move(candidates),
                      .onMissing = MissingTarget::Blocks};
}

std::size_t position(const EvaluationPlan& plan, NodeKey node)
{
    const auto found = std::ranges::find(plan.order, node);
    EXPECT_NE(found, plan.order.end());
    return static_cast<std::size_t>(found - plan.order.begin());
}

/// Every evaluated node appears after its object's earlier facet and after all of its live dependencies.
void expectTopological(const DependencyGraph& graph, const EvaluationPlan& plan)
{
    for (std::size_t index = 0; index < plan.order.size(); ++index)
    {
        const auto node = plan.order[index];
        if (node.facet != Facet::Definition)
        {
            const auto previous = node.facet == Facet::Data ? Facet::Geometry : Facet::Definition;
            EXPECT_LT(position(plan, NodeKey{.object = node.object, .facet = previous}), index);
        }
        for (const auto& dependency : graph.dependencies(node))
        {
            if (dependency.target && graph.contains(*dependency.target))
            {
                EXPECT_LT(position(plan, NodeKey{.object = *dependency.target, .facet = dependency.targetFacet}), index);
            }
        }
    }
}

std::set<NodeKey> planned(const EvaluationPlan& plan)
{
    std::set<NodeKey> result(plan.order.begin(), plan.order.end());
    for (const auto& blocked : plan.blocked)
    {
        result.insert(blocked.node);
    }
    return result;
}

std::vector<NodeKey> allNodes(std::initializer_list<std::uint64_t> values)
{
    std::vector<NodeKey> result;
    for (const auto value : values)
    {
        result.push_back(definition(value));
        result.push_back(geometry(value));
        result.push_back(data(value));
    }
    return result;
}

BlockedNode dependsOnBlocked(NodeKey node, std::optional<DependencyKey> via = std::nullopt)
{
    return BlockedNode{.node = node, .reason = BlockReason::DependsOnBlocked, .via = via, .cycle = std::nullopt};
}

BlockedNode onCycle(NodeKey node, std::size_t cycle)
{
    return BlockedNode{.node = node, .reason = BlockReason::Cycle, .via = std::nullopt, .cycle = cycle};
}

TEST(DependencyGraph, IdentitiesRevisionsAndDiagnosticNamesAreValueTypes)
{
    EXPECT_FALSE(ObjectId{}.isValid());
    EXPECT_TRUE(id(1).isValid());
    EXPECT_LT(id(1), id(2));
    EXPECT_EQ(std::hash<ObjectId>{}(id(7)), std::hash<std::uint64_t>{}(7));
    EXPECT_LT(Revision{1}, Revision{2});
    EXPECT_LT(definition(1), geometry(1));
    EXPECT_LT(geometry(1), data(1));
    EXPECT_LT(data(1), definition(2));

    EXPECT_EQ(motrack::model::dependencyKindName(DependencyKind::MemberOf), "member-of");
    EXPECT_EQ(motrack::model::dependencyKindName(DependencyKind::AttachedTo), "attached-to");
    EXPECT_EQ(motrack::model::dependencyKindName(DependencyKind::ReferencesDataOf), "references-data-of");
    EXPECT_EQ(motrack::model::dependencyKindName(DependencyKind::FrameOf), "frame-of");
    EXPECT_EQ(motrack::model::dependencyKindName(DependencyKind::BoosterOf), "booster-of");
    EXPECT_EQ(motrack::model::dependencyKindName(DependencyKind::FunctionOf), "function-of");
    // Values outside the enumeration are diagnosed, not undefined.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_EQ(motrack::model::dependencyKindName(static_cast<DependencyKind>(200)), "unknown");

    std::set<std::string_view> messages;
    for (auto code = static_cast<int>(DependencyErrorCode::InvalidObjectId);
         code <= static_cast<int>(DependencyErrorCode::CandidateBudgetExceeded);
         ++code)
    {
        const auto message = motrack::model::errorMessage(static_cast<DependencyErrorCode>(code));
        EXPECT_FALSE(message.empty());
        messages.insert(message);
    }
    EXPECT_EQ(messages.size(), 9u);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_EQ(motrack::model::errorMessage(static_cast<DependencyErrorCode>(200)), "Unknown dependency error.");

    const DependencyResult<int> value = 3;
    EXPECT_TRUE(value.hasValue());
    EXPECT_EQ(*value, 3);
    EXPECT_EQ(*value.operator->(), 3);
    const DependencyResult<int> error =
        motrack::model::DependencyError{.code = DependencyErrorCode::UnknownObject, .object = id(4), .cycle = {}};
    EXPECT_FALSE(error);
    EXPECT_EQ(error.error().object, id(4));
    EXPECT_THROW(static_cast<void>(*error), std::bad_optional_access);
    EXPECT_THROW(static_cast<void>(error.operator->()), std::bad_optional_access);
    EXPECT_THROW(static_cast<void>(value.error()), std::bad_optional_access);
}

TEST(DependencyGraph, AddsObjectsAndRefusesReservedDuplicateAndReusedIdentitiesWithoutChangingRevision)
{
    DependencyGraph graph;
    EXPECT_EQ(graph.revision(), Revision{0});
    EXPECT_TRUE(graph.plan().order.empty());

    const auto zero = graph.addObject(ObjectId{}, {});
    ASSERT_FALSE(zero);
    EXPECT_EQ(zero.error().code, DependencyErrorCode::InvalidObjectId);
    add(graph, 5, "mass B");
    add(graph, 2, "mass A");
    EXPECT_EQ(graph.revision(), Revision{2});
    const auto duplicate = graph.addObject(id(5), {});
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, DependencyErrorCode::DuplicateObject);
    EXPECT_EQ(duplicate.error().object, id(5));
    EXPECT_EQ(graph.revision(), Revision{2});

    EXPECT_EQ(graph.objects(), (std::vector<ObjectId>{id(2), id(5)}));
    EXPECT_EQ(graph.objectCount(), 2u);
    ASSERT_NE(graph.object(id(5)), nullptr);
    EXPECT_EQ(graph.object(id(5))->label, "mass B");
    EXPECT_EQ(graph.object(id(5))->className, POINT_MASS);
    EXPECT_EQ(graph.object(id(9)), nullptr);
    EXPECT_TRUE(graph.dependencies(geometry(9)).empty());
    EXPECT_EQ(graph.dependency(key(geometry(9), DependencyKind::MemberOf)), nullptr);
    EXPECT_FALSE(graph.effectiveRevision(geometry(9)));
    EXPECT_FALSE(graph.isCurrent(geometry(9), Revision{100}));

    ASSERT_TRUE(graph.removeObject(id(5)));
    EXPECT_FALSE(graph.contains(id(5)));
    EXPECT_TRUE(graph.wasRemoved(id(5)));
    const auto reused = graph.addObject(id(5), {});
    ASSERT_FALSE(reused);
    EXPECT_EQ(reused.error().code, DependencyErrorCode::IdentityReused);
    const auto missing = graph.removeObject(id(5));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, DependencyErrorCode::UnknownObject);
    EXPECT_EQ(graph.revision(), Revision{3});
}

TEST(DependencyGraph, OrdersChainsDependenciesFirstIndependentlyOfInsertionHistory)
{
    // A center of mass (3) of two masses (1, 2), a sum (4) reading the center, and an unrelated mass (9).
    // The center's positions read the members' geometry, which includes their masses; its total mass, an
    // input to its own geometry, reads their definitions.
    const auto build = [](bool reversed)
    {
        DependencyGraph graph;
        std::vector<std::uint64_t> objects{1, 2, 3, 4, 9};
        if (reversed)
        {
            std::ranges::reverse(objects);
        }
        for (const auto value : objects)
        {
            add(graph, value);
        }
        std::vector<std::function<void()>> edges{
            [&] { depend(graph, geometry(3), geometry(1), DependencyKind::MemberOf, 0); },
            [&] { depend(graph, geometry(3), geometry(2), DependencyKind::MemberOf, 1); },
            [&] { depend(graph, definition(3), definition(1), DependencyKind::MemberOf, 0); },
            [&] { depend(graph, definition(3), definition(2), DependencyKind::MemberOf, 1); },
            [&] { depend(graph, geometry(4), geometry(3), DependencyKind::ReferencesDataOf); },
        };
        if (reversed)
        {
            std::ranges::reverse(edges);
        }
        for (const auto& edge : edges)
        {
            edge();
        }
        return graph;
    };
    const auto forward = build(false);
    const auto backward = build(true);
    const auto plan = forward.plan();
    EXPECT_EQ(plan.order, backward.plan().order);
    EXPECT_EQ(plan.order, allNodes({1, 2, 3, 4, 9}));
    EXPECT_TRUE(plan.blocked.empty());
    EXPECT_TRUE(plan.cycles.empty());
    EXPECT_TRUE(plan.references.empty());
    EXPECT_EQ(plan.revision, forward.revision());
    expectTopological(forward, plan);

    // Dependencies stay sorted by kind and slot, whatever order they were added in.
    const auto members = backward.dependencies(geometry(3));
    ASSERT_EQ(members.size(), 2u);
    EXPECT_EQ(members[0].slot, 0u);
    EXPECT_EQ(members[1].slot, 1u);
    EXPECT_EQ(forward.referencesTo(id(1)),
              (std::vector<DependencyKey>{key(definition(3), DependencyKind::MemberOf), key(geometry(3), DependencyKind::MemberOf)}));
}

TEST(DependencyGraph, DiamondInvalidatesOnlyAffectedNodes)
{
    // 4 reads 2 and 3, which both read 1; 5 is independent.
    DependencyGraph graph;
    for (const auto value : {1, 2, 3, 4, 5})
    {
        add(graph, value);
    }
    depend(graph, geometry(2), geometry(1));
    depend(graph, geometry(3), geometry(1));
    depend(graph, geometry(4), geometry(2), DependencyKind::MemberOf, 0);
    depend(graph, geometry(4), geometry(3), DependencyKind::MemberOf, 1);
    const auto plan = graph.plan();
    expectTopological(graph, plan);
    EXPECT_LT(position(plan, geometry(2)), position(plan, geometry(3)));

    const auto computed = graph.revision();
    for (const auto node : allNodes({1, 2, 3, 4, 5}))
    {
        EXPECT_TRUE(graph.isCurrent(node, computed));
    }
    EXPECT_TRUE(graph.planSince(computed).order.empty());

    ASSERT_TRUE(graph.markChanged(geometry(2)));
    const auto afterBranch = graph.planSince(computed);
    EXPECT_EQ(afterBranch.order, (std::vector<NodeKey>{geometry(2), data(2), geometry(4), data(4)}));
    EXPECT_TRUE(graph.isCurrent(geometry(1), computed));
    EXPECT_TRUE(graph.isCurrent(geometry(3), computed));
    EXPECT_FALSE(graph.isCurrent(geometry(4), computed));
    EXPECT_TRUE(graph.isCurrent(geometry(5), computed));

    const auto second = graph.revision();
    ASSERT_TRUE(graph.markChanged(geometry(1)));
    const auto afterRoot = graph.planSince(second);
    EXPECT_EQ(afterRoot.order,
              (std::vector<NodeKey>{geometry(1), data(1), geometry(2), data(2), geometry(3), data(3), geometry(4), data(4)}));
    EXPECT_EQ(graph.effectiveRevision(data(4)), graph.revision());
    EXPECT_EQ(graph.effectiveRevision(geometry(5)), Revision{5});

    // A mass edit changes the definition, which every position and data value of the object includes.
    const auto third = graph.revision();
    ASSERT_TRUE(graph.markChanged(definition(1)));
    EXPECT_EQ(
        graph.planSince(third).order,
        (std::vector<NodeKey>{definition(1), geometry(1), data(1), geometry(2), data(2), geometry(3), data(3), geometry(4), data(4)}));

    // A change that affects only world data, such as a derivative setting, leaves every geometry current.
    const auto fourth = graph.revision();
    ASSERT_TRUE(graph.markChanged(data(1)));
    EXPECT_EQ(graph.planSince(fourth).order, (std::vector<NodeKey>{data(1)}));
}

TEST(DependencyGraph, RefusesSelfOwnFacetChainAndDiamondCyclesWithTheirPath)
{
    DependencyGraph graph;
    for (const auto value : {1, 2, 3, 4})
    {
        add(graph, value, "track " + std::to_string(value));
    }
    const auto before = graph.revision();

    const auto self = graph.addDependency(geometry(1), on(geometry(1)));
    ASSERT_FALSE(self);
    EXPECT_EQ(self.error().code, DependencyErrorCode::CycleRejected);
    EXPECT_EQ(self.error().object, id(1));
    EXPECT_EQ(self.error().cycle.edges,
              (std::vector<CycleEdge>{{.from = geometry(1), .to = geometry(1), .dependency = key(geometry(1), DependencyKind::MemberOf)}}));
    EXPECT_EQ(motrack::model::describeCycle(graph, self.error().cycle), "'track 1' #1 geometry -[member-of 0]-> 'track 1' #1 geometry");

    // Geometry reading its own data closes the implicit data-on-geometry edge.
    const auto ownData = graph.addDependency(geometry(1), on(data(1), DependencyKind::ReferencesDataOf, 3));
    ASSERT_FALSE(ownData);
    EXPECT_EQ(
        ownData.error().cycle.edges,
        (std::vector<CycleEdge>{{.from = geometry(1), .to = data(1), .dependency = key(geometry(1), DependencyKind::ReferencesDataOf, 3)},
                                {.from = data(1), .to = geometry(1), .dependency = std::nullopt}}));
    EXPECT_EQ(motrack::model::describeCycle(graph, ownData.error().cycle),
              "'track 1' #1 geometry -[references-data-of 3]-> 'track 1' #1 data -[own geometry]-> 'track 1' #1 geometry");
    // A definition may read only other definitions, so no input inherits a frame dependency.
    EXPECT_EQ(graph.addDependency(definition(1), on(geometry(1), DependencyKind::ReferencesDataOf, 4)).error().code,
              DependencyErrorCode::InvalidDependency);
    EXPECT_EQ(graph.addDependency(definition(1), on(data(2), DependencyKind::FunctionOf)).error().code,
              DependencyErrorCode::InvalidDependency);
    depend(graph, definition(1), definition(2), DependencyKind::FunctionOf);
    ASSERT_TRUE(graph.removeDependency(key(definition(1), DependencyKind::FunctionOf)));
    // Data reading its own geometry explicitly is not a cycle.
    depend(graph, data(1), geometry(1), DependencyKind::ReferencesDataOf);
    ASSERT_TRUE(graph.removeDependency(key(data(1), DependencyKind::ReferencesDataOf)));

    // Diamond 4 -> {2, 3} -> 1; closing 1 -> 4 reports the shortest path through the first branch.
    depend(graph, geometry(2), geometry(1));
    depend(graph, geometry(3), geometry(1));
    depend(graph, geometry(4), geometry(2), DependencyKind::MemberOf, 0);
    depend(graph, geometry(4), geometry(3), DependencyKind::MemberOf, 1);
    const auto revision = graph.revision();
    const auto closing = on(geometry(4), DependencyKind::AttachedTo);
    const Cycle expected{.edges = {{.from = geometry(1), .to = geometry(4), .dependency = key(geometry(1), DependencyKind::AttachedTo)},
                                   {.from = geometry(4), .to = geometry(2), .dependency = key(geometry(4), DependencyKind::MemberOf, 0)},
                                   {.from = geometry(2), .to = geometry(1), .dependency = key(geometry(2), DependencyKind::MemberOf)}}};
    EXPECT_EQ(graph.cycleIfAdded(geometry(1), closing), expected);
    const auto diamond = graph.addDependency(geometry(1), closing);
    ASSERT_FALSE(diamond);
    EXPECT_EQ(diamond.error().cycle, expected);
    EXPECT_EQ(graph.revision(), revision);
    EXPECT_TRUE(graph.dependencies(geometry(1)).empty());
    EXPECT_GT(revision, before);

    // Data of 1 may read 4's geometry: different facets, no path back.
    EXPECT_FALSE(graph.cycleIfAdded(data(1), on(geometry(4))));
    // cycleIfAdded ignores unknown owners and dangling or unbound targets.
    EXPECT_FALSE(graph.cycleIfAdded(geometry(8), on(geometry(1))));
    EXPECT_FALSE(graph.cycleIfAdded(geometry(1), unbound(BindingOutcome::Missing, "x")));
    ASSERT_TRUE(graph.removeObject(id(4)));
    EXPECT_FALSE(graph.cycleIfAdded(geometry(1), on(geometry(4))));
    EXPECT_EQ(motrack::model::describeCycle(graph, expected),
              "'track 1' #1 geometry -[attached-to 0]-> #4 geometry -[member-of 0]-> 'track 2' #2 geometry -[member-of 0]-> 'track 1' "
              "#1 geometry");
    EXPECT_EQ(motrack::model::describeCycle(graph, Cycle{}), "");
}

TEST(DependencyGraph, ReferenceFramesOnCentersAndModelsFollowTrackersFrameRules)
{
    // Default coordinates (10) and a reference frame (11) are separate objects. The frame reads the default
    // coordinates and its origin's positions; every track's world data is measured in the frame.
    DependencyGraph graph;
    add(graph, 1, "mass A");
    add(graph, 2, "mass B");
    add(graph, 3, "center", CENTER_OF_MASS);
    add(graph, 10, "coordinates", "org.opensourcephysics.media.core.ImageCoordSystem");
    add(graph, 11, "frame", "org.opensourcephysics.cabrillo.tracker.ReferenceFrame");
    depend(graph, geometry(11), geometry(10), DependencyKind::FrameOf, 0);
    depend(graph, geometry(11), geometry(3), DependencyKind::FrameOf, 1);
    depend(graph, geometry(3), geometry(1), DependencyKind::MemberOf, 0);
    depend(graph, geometry(3), geometry(2), DependencyKind::MemberOf, 1);
    for (const auto track : {1, 2, 3})
    {
        depend(graph, data(track), geometry(11), DependencyKind::FrameOf);
    }
    const auto plan = graph.plan();
    EXPECT_TRUE(plan.cycles.empty());
    EXPECT_TRUE(plan.blocked.empty());
    expectTopological(graph, plan);
    EXPECT_LT(position(plan, geometry(11)), position(plan, data(1)));

    // Editing a member's mass moves the center, so the frame and every track's world data are stale too.
    const auto computed = graph.revision();
    ASSERT_TRUE(graph.markChanged(definition(1)));
    const auto stale = graph.planSince(computed);
    EXPECT_TRUE(stale.cycles.empty());
    EXPECT_EQ(planned(stale),
              (std::set<NodeKey>{definition(1), geometry(1), data(1), geometry(3), data(3), geometry(11), data(11), data(2)}));

    // Tracker computes a model that is the frame's origin in the default coordinates, so a frame directly on
    // a model is acyclic.
    add(graph, 4, "origin model", DYNAMIC_PARTICLE);
    depend(graph, geometry(4), geometry(10), DependencyKind::FrameOf);
    ASSERT_TRUE(graph.removeDependency(key(geometry(11), DependencyKind::FrameOf, 1)));
    depend(graph, geometry(11), geometry(4), DependencyKind::FrameOf, 1);
    EXPECT_TRUE(graph.plan().cycles.empty());

    // Any other model is computed in the active frame. A frame on a center of mass that contains such a model
    // is a real cycle.
    add(graph, 5, "model", DYNAMIC_PARTICLE);
    depend(graph, geometry(5), geometry(11), DependencyKind::FrameOf);
    depend(graph, geometry(3), geometry(5), DependencyKind::MemberOf, 2);
    ASSERT_TRUE(graph.removeDependency(key(geometry(11), DependencyKind::FrameOf, 1)));
    const auto indirect = graph.addDependency(geometry(11), on(geometry(3), DependencyKind::FrameOf, 1));
    ASSERT_FALSE(indirect);
    EXPECT_EQ(
        indirect.error().cycle.edges,
        (std::vector<CycleEdge>{{.from = geometry(11), .to = geometry(3), .dependency = key(geometry(11), DependencyKind::FrameOf, 1)},
                                {.from = geometry(3), .to = geometry(5), .dependency = key(geometry(3), DependencyKind::MemberOf, 2)},
                                {.from = geometry(5), .to = geometry(11), .dependency = key(geometry(5), DependencyKind::FrameOf)}}));
}

TEST(DependencyGraph, TwoBodySystemsReadMemberDefinitionsSoComputedPositionsAreNotFalseCycles)
{
    // System 10 computes the positions of particles 1 and 2 from their definitions (initial values, masses)
    // and a force function (12). Mass 30 boosts particle 1's initial state.
    DependencyGraph graph;
    add(graph, 1, "A", DYNAMIC_PARTICLE);
    add(graph, 2, "B", DYNAMIC_PARTICLE);
    add(graph, 10, "system", "org.opensourcephysics.cabrillo.tracker.DynamicSystem");
    add(graph, 12, "fx", "org.opensourcephysics.tools.UserFunction");
    add(graph, 30, "booster");
    depend(graph, geometry(10), definition(1), DependencyKind::MemberOf, 0);
    depend(graph, geometry(10), definition(2), DependencyKind::MemberOf, 1);
    depend(graph, geometry(10), definition(12), DependencyKind::FunctionOf);
    depend(graph, geometry(10), data(30), DependencyKind::BoosterOf, 0);
    depend(graph, geometry(1), geometry(10), DependencyKind::ReferencesDataOf);
    depend(graph, geometry(2), geometry(10), DependencyKind::ReferencesDataOf);
    const auto plan = graph.plan();
    EXPECT_TRUE(plan.cycles.empty());
    expectTopological(graph, plan);
    EXPECT_LT(position(plan, data(30)), position(plan, geometry(10)));
    EXPECT_LT(position(plan, geometry(10)), position(plan, data(1)));

    // A member's mass changes both computed trajectories.
    const auto computed = graph.revision();
    ASSERT_TRUE(graph.markChanged(definition(1)));
    EXPECT_EQ(planned(graph.planSince(computed)),
              (std::set<NodeKey>{definition(1), geometry(1), data(1), geometry(10), data(10), geometry(2), data(2)}));

    // Tracker's booster list excludes members of the same system: that would be a real cycle.
    const auto sameSystem = graph.addDependency(geometry(10), on(data(1), DependencyKind::BoosterOf, 1));
    ASSERT_FALSE(sameSystem);
    EXPECT_EQ(sameSystem.error().cycle.edges.size(), 3u);
}

TEST(DependencyGraph, RecordsLoadedCyclesAndBlocksOnlyThemAndTheirDependents)
{
    DependencyGraph graph;
    for (const auto value : {1, 2, 3, 4, 5, 6, 7, 8})
    {
        add(graph, value, "t" + std::to_string(value));
    }
    // A file may contain a center of mass that lists itself, and two sums that contain each other.
    ASSERT_TRUE(graph.addDependency(geometry(5), on(geometry(5)), CyclePolicy::Record));
    ASSERT_TRUE(graph.addDependency(geometry(2), on(geometry(3)), CyclePolicy::Record));
    ASSERT_TRUE(graph.addDependency(geometry(3), on(geometry(2)), CyclePolicy::Record));
    depend(graph, geometry(4), geometry(3), DependencyKind::AttachedTo, 1); // Depends on the cycle.
    depend(graph, geometry(6), geometry(1));                                // Unaffected.
    ASSERT_TRUE(graph.addDependency(data(7), on(data(7), DependencyKind::FunctionOf), CyclePolicy::Record));
    // Object 8's data is blocked both through its own geometry and explicitly; the explicit edge is reported.
    depend(graph, geometry(8), geometry(5), DependencyKind::AttachedTo);
    depend(graph, data(8), geometry(2), DependencyKind::ReferencesDataOf);

    const auto plan = graph.plan();
    ASSERT_EQ(plan.cycles.size(), 3u);
    EXPECT_EQ(plan.cycles[0].edges,
              (std::vector<CycleEdge>{{.from = geometry(2), .to = geometry(3), .dependency = key(geometry(2), DependencyKind::MemberOf)},
                                      {.from = geometry(3), .to = geometry(2), .dependency = key(geometry(3), DependencyKind::MemberOf)}}));
    EXPECT_EQ(plan.cycles[1].edges,
              (std::vector<CycleEdge>{{.from = geometry(5), .to = geometry(5), .dependency = key(geometry(5), DependencyKind::MemberOf)}}));
    EXPECT_EQ(plan.cycles[2].edges.size(), 1u);
    EXPECT_EQ(plan.cycles[2].edges[0].from, data(7));

    const std::vector<BlockedNode> blocked{
        onCycle(geometry(2), 0),
        dependsOnBlocked(data(2)),
        onCycle(geometry(3), 0),
        dependsOnBlocked(data(3)),
        dependsOnBlocked(geometry(4), key(geometry(4), DependencyKind::AttachedTo, 1)),
        dependsOnBlocked(data(4)),
        onCycle(geometry(5), 1),
        dependsOnBlocked(data(5)),
        onCycle(data(7), 2),
        dependsOnBlocked(geometry(8), key(geometry(8), DependencyKind::AttachedTo)),
        dependsOnBlocked(data(8), key(data(8), DependencyKind::ReferencesDataOf)),
    };
    EXPECT_EQ(plan.blocked, blocked);
    EXPECT_EQ(plan.order,
              (std::vector<NodeKey>{definition(1),
                                    geometry(1),
                                    data(1),
                                    definition(2),
                                    definition(3),
                                    definition(4),
                                    definition(5),
                                    definition(6),
                                    geometry(6),
                                    data(6),
                                    definition(7),
                                    geometry(7),
                                    definition(8)}));
    expectTopological(graph, plan);

    // Breaking the cycle with an explicit, reversible edit makes its dependents evaluable again.
    const auto revision = graph.revision();
    auto removed = graph.removeDependency(key(geometry(3), DependencyKind::MemberOf));
    ASSERT_TRUE(removed);
    const auto repaired = graph.planSince(revision);
    EXPECT_EQ(repaired.order, (std::vector<NodeKey>{geometry(3), geometry(2), data(2), data(3), geometry(4), data(4)}));
    EXPECT_EQ(repaired.cycles.size(), 2u);                                              // Cycles are still reported for the whole graph.
    EXPECT_EQ(repaired.blocked, (std::vector<BlockedNode>{dependsOnBlocked(data(8))})); // Still blocked by its own geometry.
    // A new edit that closes the cycle is refused; undoing the removal restores the loaded state.
    EXPECT_EQ(graph.addDependency(geometry(3), *removed).error().code, DependencyErrorCode::CycleRejected);
    ASSERT_TRUE(graph.addDependency(geometry(3), *std::move(removed), CyclePolicy::Record));
    EXPECT_EQ(graph.plan().cycles.size(), 3u);
}

TEST(DependencyGraph, DeletionKeepsDanglingReferencesAndUndoRestoresEveryEdge)
{
    // Center 3 of masses 1 and 2; tape 4 attached to mass 1 with its own fallback point.
    DependencyGraph graph;
    add(graph, 1, "mass A");
    add(graph, 2, "mass B");
    add(graph, 3, "center", CENTER_OF_MASS);
    add(graph, 4, "tape", "org.opensourcephysics.cabrillo.tracker.TapeMeasure");
    depend(graph, geometry(3), geometry(1), DependencyKind::MemberOf, 0);
    depend(graph, geometry(3), geometry(2), DependencyKind::MemberOf, 1);
    ASSERT_TRUE(
        graph.addDependency(geometry(4), boundDependency(DependencyKind::AttachedTo, 0, id(1), Facet::Geometry, MissingTarget::FallsBack)));
    depend(graph, geometry(1), geometry(2), DependencyKind::AttachedTo, 7); // Mass A's own dependency, removed with it.
    const auto original = graph.dependencies(geometry(3));
    const std::vector<Dependency> members(original.begin(), original.end());
    const auto computed = graph.revision();

    auto removedResult = graph.removeObject(id(1));
    ASSERT_TRUE(removedResult);
    auto removed = *std::move(removedResult);
    EXPECT_EQ(removed.id, id(1));
    EXPECT_EQ(removed.info.label, "mass A");
    ASSERT_EQ(removed.geometryDependencies.size(), 1u);
    EXPECT_TRUE(removed.definitionDependencies.empty());
    EXPECT_TRUE(removed.dataDependencies.empty());
    EXPECT_EQ(removed.danglingReferences,
              (std::vector<DependencyKey>{key(geometry(3), DependencyKind::MemberOf), key(geometry(4), DependencyKind::AttachedTo)}));
    // Nothing is dropped: the center keeps both member slots and still reports the removed identity.
    EXPECT_EQ(graph.dependencies(geometry(3)).size(), 2u);
    EXPECT_EQ(graph.referencesTo(id(1)).size(), 2u);
    EXPECT_EQ(graph.referencesTo(id(2)).size(), 1u); // Mass A's own attachment to B is gone.

    const auto plan = graph.plan();
    const std::vector<BlockedNode> blocked{
        {.node = geometry(3),
         .reason = BlockReason::DanglingReference,
         .via = key(geometry(3), DependencyKind::MemberOf),
         .cycle = std::nullopt},
        dependsOnBlocked(data(3)),
    };
    EXPECT_EQ(plan.blocked, blocked);
    // The tape falls back to its own point; the center's definition is independent of its members.
    EXPECT_EQ(plan.order, (std::vector<NodeKey>{definition(2), geometry(2), data(2), definition(3), definition(4), geometry(4), data(4)}));
    ASSERT_EQ(plan.references.size(), 2u);
    EXPECT_EQ(plan.references[1].key, key(geometry(4), DependencyKind::AttachedTo));
    EXPECT_EQ(plan.references[1].state, ReferenceState::Dangling);
    EXPECT_EQ(plan.references[1].dependency.onMissing, MissingTarget::FallsBack);
    EXPECT_FALSE(graph.isCurrent(geometry(3), computed));
    EXPECT_FALSE(graph.isCurrent(geometry(4), computed));
    EXPECT_TRUE(graph.isCurrent(geometry(2), computed));

    // The identity cannot be reused while removed, so no new object captures the dangling references.
    EXPECT_EQ(graph.addObject(id(1), {}).error().code, DependencyErrorCode::IdentityReused);

    const auto beforeUndo = graph.revision();
    ASSERT_TRUE(graph.restoreObject(removed));
    EXPECT_FALSE(graph.wasRemoved(id(1)));
    EXPECT_EQ(graph.object(id(1))->label, "mass A");
    const auto restored = graph.dependencies(geometry(3));
    EXPECT_EQ(std::vector<Dependency>(restored.begin(), restored.end()), members);
    ASSERT_EQ(graph.dependencies(geometry(1)).size(), 1u);
    EXPECT_EQ(graph.referencesTo(id(2)).size(), 2u);
    const auto after = graph.plan();
    EXPECT_TRUE(after.blocked.empty());
    EXPECT_TRUE(after.references.empty());
    EXPECT_EQ(planned(graph.planSince(beforeUndo)),
              (std::set<NodeKey>{definition(1), geometry(1), data(1), geometry(3), data(3), geometry(4), data(4)}));
    expectTopological(graph, after);

    // Restoring changes observable state, so later edits still propagate through restored edges.
    const auto settled = graph.revision();
    ASSERT_TRUE(graph.markChanged(geometry(2)));
    EXPECT_EQ(planned(graph.planSince(settled)),
              (std::set<NodeKey>{geometry(1), data(1), geometry(2), data(2), geometry(3), data(3), geometry(4), data(4)}));

    // Restore refuses live, never-removed and reserved identities, and leaves the record usable.
    EXPECT_EQ(graph.restoreObject(removed).error().code, DependencyErrorCode::DuplicateObject);
    EXPECT_EQ(removed.geometryDependencies.size(), 1u);
    removed.id = id(40);
    EXPECT_EQ(graph.restoreObject(removed).error().code, DependencyErrorCode::UnknownObject);
    removed.id = ObjectId{};
    EXPECT_EQ(graph.restoreObject(removed).error().code, DependencyErrorCode::InvalidObjectId);
}

TEST(DependencyGraph, UndoRestoresALoadedCycleInsteadOfRefusingIt)
{
    // A file's center of mass "cm" (2) lists mass A (1) and itself.
    DependencyGraph graph;
    add(graph, 1, "A");
    add(graph, 2, "cm", CENTER_OF_MASS);
    depend(graph, geometry(2), geometry(1), DependencyKind::MemberOf, 0);
    ASSERT_TRUE(graph.addDependency(geometry(2), on(geometry(2), DependencyKind::MemberOf, 1), CyclePolicy::Record));
    ASSERT_EQ(graph.plan().cycles.size(), 1u);

    // Deleting the center and undoing the deletion brings the loaded cycle back, still reported.
    const auto removed = *graph.removeObject(id(2));
    EXPECT_TRUE(graph.plan().cycles.empty());
    ASSERT_TRUE(graph.restoreObject(removed));
    const auto plan = graph.plan();
    ASSERT_EQ(plan.cycles.size(), 1u);
    EXPECT_EQ(plan.blocked.front(), onCycle(geometry(2), 0));
    // A caller that restores out of order can still ask for a refusal.
    const auto again = *graph.removeObject(id(2));
    const auto refused = graph.restoreObject(again, CyclePolicy::Reject);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().cycle.edges.size(), 1u);
    ASSERT_TRUE(graph.restoreObject(again));
}

TEST(DependencyGraph, OutOfOrderRestoreThatClosesACycleIsReportedOrRefusedOnRequest)
{
    // 1 reads 2, and 2 reads 3. Remove 2, let 3 read 1, then try to restore 2.
    DependencyGraph graph;
    for (const auto value : {1, 2, 3})
    {
        add(graph, value);
    }
    depend(graph, geometry(1), geometry(2));
    depend(graph, geometry(2), geometry(3));
    const auto removed = *graph.removeObject(id(2));
    depend(graph, geometry(3), geometry(1));
    const auto revision = graph.revision();
    const auto refused = graph.restoreObject(removed, CyclePolicy::Reject);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, DependencyErrorCode::CycleRejected);
    EXPECT_EQ(refused.error().cycle.edges,
              (std::vector<CycleEdge>{{.from = geometry(2), .to = geometry(3), .dependency = key(geometry(2), DependencyKind::MemberOf)},
                                      {.from = geometry(3), .to = geometry(1), .dependency = key(geometry(3), DependencyKind::MemberOf)},
                                      {.from = geometry(1), .to = geometry(2), .dependency = key(geometry(1), DependencyKind::MemberOf)}}));
    EXPECT_EQ(graph.revision(), revision);
    EXPECT_FALSE(graph.contains(id(2)));
    EXPECT_TRUE(graph.wasRemoved(id(2)));
    EXPECT_EQ(graph.referencesTo(id(3)), (std::vector<DependencyKey>{}));
    EXPECT_EQ(graph.plan().blocked.size(), 4u); // Mass 1 still dangles, and 3 depends on it.

    // A cycle closing through the restored object's data facet is found too.
    DependencyGraph dataCycle;
    add(dataCycle, 1);
    add(dataCycle, 2);
    depend(dataCycle, data(1), geometry(2), DependencyKind::ReferencesDataOf);
    const auto dataRemoved = *dataCycle.removeObject(id(1));
    depend(dataCycle, geometry(2), data(1), DependencyKind::ReferencesDataOf);
    EXPECT_EQ(
        dataCycle.restoreObject(dataRemoved, CyclePolicy::Reject).error().cycle.edges,
        (std::vector<CycleEdge>{{.from = data(1), .to = geometry(2), .dependency = key(data(1), DependencyKind::ReferencesDataOf)},
                                {.from = geometry(2), .to = data(1), .dependency = key(geometry(2), DependencyKind::ReferencesDataOf)}}));

    // By default the restoration is recorded, and the new cycle is reported rather than hidden.
    ASSERT_TRUE(graph.restoreObject(removed));
    const auto plan = graph.plan();
    ASSERT_EQ(plan.cycles.size(), 1u);
    EXPECT_EQ(plan.cycles[0].edges.front().from, geometry(1));
    EXPECT_EQ(plan.cycles[0].edges.size(), 3u);
}

TEST(DependencyGraph, RestoreValidatesFabricatedRecords)
{
    DependencyGraph graph;
    add(graph, 1);
    add(graph, 2);
    const auto removed = *graph.removeObject(id(1));
    auto invalid = removed;
    invalid.dataDependencies.push_back(unbound(BindingOutcome::Unique, "x"));
    EXPECT_EQ(graph.restoreObject(invalid).error().code, DependencyErrorCode::InvalidDependency);
    auto unknown = removed;
    unknown.geometryDependencies.push_back(on(geometry(77)));
    EXPECT_EQ(graph.restoreObject(unknown).error().code, DependencyErrorCode::UnknownObject);
    auto readsGeometry = removed;
    readsGeometry.definitionDependencies = {on(geometry(2))};
    EXPECT_EQ(graph.restoreObject(readsGeometry).error().code, DependencyErrorCode::InvalidDependency);
    auto duplicate = removed;
    duplicate.definitionDependencies = {on(definition(2)), on(definition(2))};
    EXPECT_EQ(graph.restoreObject(duplicate).error().code, DependencyErrorCode::DuplicateDependency);
    auto self = removed;
    self.geometryDependencies = {on(geometry(1), DependencyKind::AttachedTo), on(geometry(2), DependencyKind::MemberOf, 4)};
    ASSERT_FALSE(graph.restoreObject(self, CyclePolicy::Reject)); // A self-reference is a cycle.
    EXPECT_FALSE(graph.contains(id(1)));
    EXPECT_EQ(self.geometryDependencies.size(), 2u);
    // Unsorted dependencies are accepted and stored in (kind, slot) order.
    auto unsorted = removed;
    unsorted.geometryDependencies = {on(geometry(2), DependencyKind::AttachedTo), on(geometry(2), DependencyKind::MemberOf, 4)};
    ASSERT_TRUE(graph.restoreObject(unsorted));
    const auto stored = graph.dependencies(geometry(1));
    ASSERT_EQ(stored.size(), 2u);
    EXPECT_EQ(stored[0].kind, DependencyKind::MemberOf);
    EXPECT_EQ(stored[1].kind, DependencyKind::AttachedTo);
}

TEST(DependencyGraph, RetiredIdentitiesKeepDanglingReferencesAcrossANativeReopen)
{
    // A reopened document's center of mass (1) still names member 5, removed before the save.
    DependencyGraph graph;
    add(graph, 1, "center", CENTER_OF_MASS);
    EXPECT_EQ(graph.addDependency(geometry(1), on(geometry(5))).error().code, DependencyErrorCode::UnknownObject);
    const auto revision = graph.revision();
    ASSERT_TRUE(graph.retireIdentity(id(5)));
    EXPECT_GT(graph.revision(), revision);
    EXPECT_TRUE(graph.wasRemoved(id(5)));
    ASSERT_TRUE(graph.addDependency(geometry(1), on(geometry(5))));
    const auto plan = graph.plan();
    ASSERT_EQ(plan.references.size(), 1u);
    EXPECT_EQ(plan.references[0].state, ReferenceState::Dangling);
    EXPECT_EQ(plan.blocked.front().reason, BlockReason::DanglingReference);
    // A retired identity is never given to a new object, but a restored record can bring it back.
    EXPECT_EQ(graph.addObject(id(5), {}).error().code, DependencyErrorCode::IdentityReused);
    EXPECT_EQ(graph.retireIdentity(id(1)).error().code, DependencyErrorCode::DuplicateObject);
    EXPECT_EQ(graph.retireIdentity(ObjectId{}).error().code, DependencyErrorCode::InvalidObjectId);
    ASSERT_TRUE(graph.restoreObject(RemovedObject{.id = id(5),
                                                  .info = ObjectInfo{.label = "mass", .className = std::string(POINT_MASS)},
                                                  .definitionDependencies = {},
                                                  .geometryDependencies = {},
                                                  .dataDependencies = {},
                                                  .danglingReferences = {}}));
    EXPECT_TRUE(graph.plan().blocked.empty());
}

TEST(DependencyGraph, UnboundAndDanglingReferencesAreKeptReportedAndBlockOnlyWhenRequired)
{
    DependencyGraph graph;
    add(graph, 1, "mass A");
    add(graph, 2, "duplicate");
    add(graph, 3, "duplicate");
    add(graph, 4, "center", CENTER_OF_MASS);
    add(graph, 5, "model", DYNAMIC_PARTICLE);
    add(graph, 6, "removed");
    ASSERT_TRUE(graph.removeObject(id(6)));
    // Loaded center members: one bound, one ambiguous, one missing.
    depend(graph, geometry(4), geometry(1), DependencyKind::MemberOf, 0);
    ASSERT_TRUE(graph.addDependency(geometry(4), unbound(BindingOutcome::Ambiguous, "duplicate", {id(2), id(3)}, 1)));
    ASSERT_TRUE(graph.addDependency(geometry(4), unbound(BindingOutcome::Missing, "gone", {}, 2)));
    // A model booster that did not resolve falls back to the model's own initial state.
    auto booster = unbound(BindingOutcome::Unclassified, "maybe", {id(4)});
    booster.kind = DependencyKind::BoosterOf;
    booster.onMissing = MissingTarget::FallsBack;
    ASSERT_TRUE(graph.addDependency(geometry(5), booster));
    // A native edit may target a removed identity; the new reference dangles immediately.
    ASSERT_TRUE(graph.addDependency(data(5), on(data(6), DependencyKind::FunctionOf)));

    const auto plan = graph.plan();
    ASSERT_EQ(plan.references.size(), 4u);
    EXPECT_EQ(plan.references[0].key, key(geometry(4), DependencyKind::MemberOf, 1));
    EXPECT_EQ(plan.references[0].state, ReferenceState::Unbound);
    EXPECT_EQ(plan.references[0].dependency.savedName, "duplicate");
    EXPECT_EQ(plan.references[0].dependency.candidates, (std::vector<ObjectId>{id(2), id(3)}));
    EXPECT_EQ(plan.references[1].dependency.binding, BindingOutcome::Missing);
    EXPECT_EQ(plan.references[2].key, key(geometry(5), DependencyKind::BoosterOf));
    EXPECT_EQ(plan.references[3].state, ReferenceState::Dangling);
    const std::vector<BlockedNode> blocked{
        {.node = geometry(4),
         .reason = BlockReason::UnboundReference,
         .via = key(geometry(4), DependencyKind::MemberOf, 1),
         .cycle = std::nullopt},
        dependsOnBlocked(data(4)),
        {.node = data(5), .reason = BlockReason::DanglingReference, .via = key(data(5), DependencyKind::FunctionOf), .cycle = std::nullopt},
    };
    EXPECT_EQ(plan.blocked, blocked);
    EXPECT_EQ(plan.order,
              (std::vector<NodeKey>{definition(1),
                                    geometry(1),
                                    data(1),
                                    definition(2),
                                    geometry(2),
                                    data(2),
                                    definition(3),
                                    geometry(3),
                                    data(3),
                                    definition(4),
                                    definition(5),
                                    geometry(5)}));
    // Unbound references never join the dependents index.
    EXPECT_TRUE(graph.referencesTo(id(2)).empty());
    EXPECT_EQ(graph.referencesTo(id(6)), (std::vector<DependencyKey>{key(data(5), DependencyKind::FunctionOf)}));

    // Fixing a member is an explicit edit: remove the unbound reference, add a bound one.
    ASSERT_TRUE(graph.removeDependency(key(geometry(4), DependencyKind::MemberOf, 1)));
    depend(graph, geometry(4), geometry(3), DependencyKind::MemberOf, 1);
    ASSERT_TRUE(graph.removeDependency(key(geometry(4), DependencyKind::MemberOf, 2)));
    EXPECT_TRUE(std::ranges::none_of(graph.plan().blocked, [](const BlockedNode& node) { return node.node.object == id(4); }));
}

TEST(DependencyGraph, ValidatesDependencyEditsWithoutChangingTheGraph)
{
    DependencyGraph graph;
    add(graph, 1);
    add(graph, 2);
    depend(graph, geometry(1), geometry(2));
    const auto revision = graph.revision();
    const auto expectRefused = [&](const auto& result, DependencyErrorCode code)
    {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, code);
        EXPECT_EQ(graph.revision(), revision);
    };
    expectRefused(graph.addDependency(geometry(9), on(geometry(2))), DependencyErrorCode::UnknownObject);
    expectRefused(graph.addDependency(geometry(1), on(geometry(2))), DependencyErrorCode::DuplicateDependency);
    expectRefused(graph.addDependency(geometry(1), on(geometry(9), DependencyKind::AttachedTo)), DependencyErrorCode::UnknownObject);
    expectRefused(graph.addDependency(geometry(1), on(NodeKey{}, DependencyKind::AttachedTo)), DependencyErrorCode::InvalidDependency);
    auto withCandidates = on(geometry(2), DependencyKind::AttachedTo);
    withCandidates.candidates = {id(2)};
    expectRefused(graph.addDependency(geometry(1), withCandidates), DependencyErrorCode::InvalidDependency);
    auto notUnique = on(geometry(2), DependencyKind::AttachedTo);
    notUnique.binding = BindingOutcome::Ambiguous;
    expectRefused(graph.addDependency(geometry(1), notUnique), DependencyErrorCode::InvalidDependency);
    expectRefused(graph.addDependency(geometry(1), unbound(BindingOutcome::Unique, "x", {}, 5)), DependencyErrorCode::InvalidDependency);
    expectRefused(graph.addDependency(geometry(1), unbound(BindingOutcome::NoTarget, "", {}, 5)), DependencyErrorCode::InvalidDependency);
    expectRefused(graph.removeDependency(key(geometry(9), DependencyKind::MemberOf)), DependencyErrorCode::UnknownObject);
    expectRefused(graph.removeDependency(key(geometry(1), DependencyKind::MemberOf, 1)), DependencyErrorCode::UnknownDependency);
    expectRefused(graph.removeDependency(key(data(1), DependencyKind::MemberOf)), DependencyErrorCode::UnknownDependency);
    expectRefused(graph.markChanged(data(9)), DependencyErrorCode::UnknownObject);
    expectRefused(graph.renameObject(id(9), "x"), DependencyErrorCode::UnknownObject);

    // The same slot number is independent across kinds and facets.
    depend(graph, geometry(1), geometry(2), DependencyKind::AttachedTo);
    depend(graph, data(1), geometry(2));
    ASSERT_NE(graph.dependency(key(geometry(1), DependencyKind::AttachedTo)), nullptr);
    EXPECT_EQ(graph.dependency(key(geometry(1), DependencyKind::BoosterOf)), nullptr);
    const auto removed = graph.removeDependency(key(geometry(1), DependencyKind::AttachedTo));
    ASSERT_TRUE(removed);
    EXPECT_EQ(*removed, on(geometry(2), DependencyKind::AttachedTo));
    EXPECT_EQ(graph.referencesTo(id(2)).size(), 2u);
}

TEST(DependencyGraph, RenamesAreLabelsThatNeitherRebindNorInvalidate)
{
    DependencyGraph graph;
    add(graph, 1, "mass A");
    add(graph, 2, "mass B");
    add(graph, 3, "center", CENTER_OF_MASS);
    depend(graph, geometry(3), geometry(1), DependencyKind::MemberOf, 0);
    depend(graph, geometry(3), geometry(2), DependencyKind::MemberOf, 1);
    ASSERT_TRUE(graph.addDependency(data(3), unbound(BindingOutcome::Missing, "mass C", {}, 2)));
    const auto computed = graph.revision();

    // Both masses take the same label, and one takes the unresolved reference's saved name.
    ASSERT_TRUE(graph.renameObject(id(1), "same"));
    ASSERT_TRUE(graph.renameObject(id(2), "same"));
    ASSERT_TRUE(graph.renameObject(id(2), "mass C"));
    EXPECT_EQ(graph.revision(), Revision{computed.value + 3});
    EXPECT_EQ(graph.object(id(1))->label, "same");
    EXPECT_EQ(graph.dependencies(geometry(3))[0].target, id(1));
    EXPECT_EQ(graph.dependencies(geometry(3))[1].target, id(2));
    EXPECT_FALSE(graph.dependencies(data(3))[0].target); // Never bound by name after load.
    EXPECT_TRUE(graph.planSince(computed).order.empty());
    EXPECT_TRUE(graph.isCurrent(geometry(3), computed));
}

TEST(DependencyGraph, CopiesAreIndependentSnapshots)
{
    DependencyGraph graph;
    add(graph, 1);
    add(graph, 2);
    depend(graph, geometry(2), geometry(1));
    const auto snapshot = graph;
    ASSERT_TRUE(graph.removeObject(id(1)));
    ASSERT_TRUE(graph.markChanged(geometry(2)));
    EXPECT_TRUE(snapshot.contains(id(1)));
    EXPECT_TRUE(snapshot.plan().blocked.empty());
    EXPECT_EQ(snapshot.revision(), Revision{3});
    EXPECT_EQ(graph.plan().blocked.size(), 2u);
}

TEST(DependencyGraph, TenThousandObjectGraphsStayBoundedAndIterative)
{
    constexpr std::uint64_t COUNT = 10000;
    const auto started = std::chrono::steady_clock::now();
    DependencyGraph graph;
    for (std::uint64_t value = 1; value <= COUNT; ++value)
    {
        add(graph, value, "track");
    }
    // A 10000-deep chain of geometry, plus every data facet measured in the frame of object 1.
    for (std::uint64_t value = 2; value <= COUNT; ++value)
    {
        ASSERT_TRUE(graph.addDependency(geometry(value), on(geometry(value - 1)), CyclePolicy::Record));
        ASSERT_TRUE(graph.addDependency(data(value), on(geometry(1), DependencyKind::FrameOf), CyclePolicy::Record));
    }
    const auto plan = graph.plan();
    ASSERT_EQ(plan.order.size(), 3 * COUNT);
    EXPECT_EQ(plan.order.front(), definition(1));
    EXPECT_EQ(plan.order.back(), data(COUNT));
    EXPECT_TRUE(plan.cycles.empty());

    // Closing the chain is refused with the full path, found without recursion.
    const auto cycle = graph.cycleIfAdded(geometry(1), on(geometry(COUNT), DependencyKind::AttachedTo));
    ASSERT_TRUE(cycle.has_value());
    EXPECT_EQ(cycle.value_or(Cycle{}).edges.size(), COUNT); // value_or: clang-tidy 18 ignores ASSERT_TRUE as a check.

    // Changing the leaf invalidates only the leaf; changing the root invalidates everything after it.
    auto revision = graph.revision();
    ASSERT_TRUE(graph.markChanged(geometry(COUNT)));
    EXPECT_EQ(graph.planSince(revision).order, (std::vector<NodeKey>{geometry(COUNT), data(COUNT)}));
    revision = graph.revision();
    ASSERT_TRUE(graph.markChanged(geometry(1)));
    EXPECT_EQ(graph.planSince(revision).order.size(), 2 * COUNT);

    // A recorded cycle through the whole chain is one component with one shortest path.
    ASSERT_TRUE(graph.addDependency(geometry(1), on(geometry(COUNT), DependencyKind::AttachedTo), CyclePolicy::Record));
    const auto cyclic = graph.plan();
    ASSERT_EQ(cyclic.cycles.size(), 1u);
    EXPECT_EQ(cyclic.cycles[0].edges.size(), COUNT);
    EXPECT_EQ(cyclic.blocked.size(), 2 * COUNT);
    EXPECT_EQ(cyclic.order.size(), COUNT); // Only the definitions remain evaluable.

    // Removing the frame origin dangles the chain link and 9999 frame references in one bounded operation.
    ASSERT_TRUE(graph.removeDependency(key(geometry(1), DependencyKind::AttachedTo)));
    const auto removed = graph.removeObject(id(1));
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->danglingReferences.size(), COUNT);
    ASSERT_TRUE(graph.restoreObject(*removed));
    EXPECT_TRUE(graph.plan().blocked.empty());
    // Generous enough for unoptimized sanitizer builds, yet far below a quadratic algorithm's cost.
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(30));
}

} // namespace
