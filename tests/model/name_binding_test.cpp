#include "model/dependencies/name_binding.hpp"

#include "tracker_references.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace
{

using motrack::model::BindingOutcome;
using motrack::model::boundDependency;
using motrack::model::CandidateFilter;
using motrack::model::Dependency;
using motrack::model::DependencyErrorCode;
using motrack::model::DependencyGraph;
using motrack::model::DependencyKind;
using motrack::model::Facet;
using motrack::model::makeDependency;
using motrack::model::MissingTarget;
using motrack::model::NameBinding;
using motrack::model::NamedObject;
using motrack::model::NameIndex;
using motrack::model::NameIndexLimits;
using motrack::model::ObjectId;
using motrack::model::ObjectInfo;
using motrack::model::RenderStatus;

constexpr const char* POINT_MASS = "org.opensourcephysics.cabrillo.tracker.PointMass";
constexpr const char* VECTOR = "org.opensourcephysics.cabrillo.tracker.Vector";
constexpr const char* CENTER_OF_MASS = "org.opensourcephysics.cabrillo.tracker.CenterOfMass";
constexpr const char* VECTOR_SUM = "org.opensourcephysics.cabrillo.tracker.VectorSum";
constexpr const char* DYNAMIC_PARTICLE = "org.opensourcephysics.cabrillo.tracker.DynamicParticle";
constexpr const char* POLAR_PARTICLE = "org.opensourcephysics.cabrillo.tracker.DynamicParticlePolar";
constexpr const char* DYNAMIC_SYSTEM = "org.opensourcephysics.cabrillo.tracker.DynamicSystem";

// The loader type rules of the source reference index (src/tracker_references.cpp).
const CandidateFilter& panel()
{
    static const CandidateFilter filter{};
    return filter;
}

const CandidateFilter& centerMembers()
{
    static const CandidateFilter filter{.eligibleClasses = {POINT_MASS}, .excludedClasses = {VECTOR}};
    return filter;
}

const CandidateFilter& sumMembers()
{
    static const CandidateFilter filter{.eligibleClasses = {VECTOR}, .excludedClasses = {POINT_MASS}};
    return filter;
}

const CandidateFilter& boosters()
{
    static const CandidateFilter filter{.eligibleClasses = {POINT_MASS}, .excludedClasses = {VECTOR}};
    return filter;
}

const CandidateFilter& systemMembers()
{
    static const CandidateFilter filter{.eligibleClasses = {DYNAMIC_PARTICLE, POLAR_PARTICLE}, .excludedClasses = {POINT_MASS, VECTOR}};
    return filter;
}

constexpr ObjectId id(std::uint64_t value)
{
    return ObjectId{value};
}

NameBinding binding(BindingOutcome outcome, std::vector<ObjectId> candidates = {})
{
    return NameBinding{.outcome = outcome, .candidates = std::move(candidates)};
}

Dependency loaded(DependencyKind kind, std::uint32_t slot, std::string_view name, const NameBinding& result)
{
    auto dependency = makeDependency(kind, slot, Facet::Geometry, name, result);
    EXPECT_TRUE(dependency.has_value());
    return dependency.value_or(Dependency{});
}

NameBinding resolve(NameIndex& index, std::optional<std::string_view> name, const CandidateFilter& filter)
{
    auto result = index.bind(name, filter);
    EXPECT_TRUE(result);
    return result ? *std::move(result) : NameBinding{};
}

TEST(NameBinding, ClassifiesUniqueAmbiguousMissingNoTargetAndUnclassifiedNames)
{
    const std::vector<NamedObject> objects{
        {.id = id(1), .label = "mass A", .className = POINT_MASS},
        {.id = id(2), .label = "twin", .className = POINT_MASS},
        {.id = id(3), .label = "twin", .className = POINT_MASS},
        {.id = id(4), .label = "mass A", .className = VECTOR},
        {.id = id(5), .label = "center", .className = CENTER_OF_MASS},
        {.id = id(6), .label = "null", .className = POINT_MASS},
        {.id = id(7), .label = "Mass A", .className = POINT_MASS},
    };
    NameIndex index(objects);
    // A same-named object of an excluded base type is skipped.
    EXPECT_EQ(resolve(index, "mass A", centerMembers()), binding(BindingOutcome::Unique, {id(1)}));
    EXPECT_EQ(resolve(index, "mass A", centerMembers()).target(), id(1));
    EXPECT_EQ(resolve(index, "mass A", sumMembers()), binding(BindingOutcome::Unique, {id(4)}));
    // A panel-wide lookup sees both, in source order, and never chooses one.
    EXPECT_EQ(resolve(index, "mass A", panel()), binding(BindingOutcome::Ambiguous, {id(1), id(4)}));
    EXPECT_FALSE(resolve(index, "mass A", panel()).target());
    EXPECT_EQ(resolve(index, "twin", centerMembers()), binding(BindingOutcome::Ambiguous, {id(2), id(3)}));
    // A nested center of mass is not silently accepted or rejected as a member.
    EXPECT_EQ(resolve(index, "center", centerMembers()), binding(BindingOutcome::Unclassified, {id(5)}));
    EXPECT_EQ(resolve(index, "twin", sumMembers()), binding(BindingOutcome::Missing));
    EXPECT_EQ(resolve(index, "absent", panel()), binding(BindingOutcome::Missing));
    // Lookup is exact and case-sensitive; a name spelled "null" is a name, and only an empty slot is NoTarget.
    EXPECT_EQ(resolve(index, "Mass A", centerMembers()), binding(BindingOutcome::Unique, {id(7)}));
    EXPECT_EQ(resolve(index, "mass A ", panel()), binding(BindingOutcome::Missing));
    EXPECT_EQ(resolve(index, "null", panel()), binding(BindingOutcome::Unique, {id(6)}));
    EXPECT_EQ(resolve(index, std::nullopt, panel()), binding(BindingOutcome::NoTarget));
    // Exclusion wins over eligibility, and an exclusion-only filter accepts every other class.
    const CandidateFilter conflicting{.eligibleClasses = {POINT_MASS}, .excludedClasses = {POINT_MASS}};
    EXPECT_EQ(resolve(index, "twin", conflicting), binding(BindingOutcome::Missing));
    const CandidateFilter allButVectors{.eligibleClasses = {}, .excludedClasses = {VECTOR}};
    EXPECT_EQ(resolve(index, "mass A", allButVectors), binding(BindingOutcome::Unique, {id(1)}));
}

TEST(NameBinding, ChargesCandidateVisitsBeforeFilteringAndRefusesWhenExhausted)
{
    const std::vector<NamedObject> objects{
        {.id = id(1), .label = "same", .className = VECTOR},
        {.id = id(2), .label = "same", .className = VECTOR},
        {.id = id(3), .label = "same", .className = POINT_MASS},
    };
    NameIndex index(objects, NameIndexLimits{.maxCandidateVisits = 7});
    EXPECT_EQ(index.remainingCandidateVisits(), 7u);
    EXPECT_EQ(resolve(index, "same", centerMembers()), binding(BindingOutcome::Unique, {id(3)}));
    EXPECT_EQ(index.remainingCandidateVisits(), 4u); // Wrong-type candidates are charged too.
    EXPECT_EQ(resolve(index, "absent", panel()), binding(BindingOutcome::Missing));
    EXPECT_EQ(resolve(index, std::nullopt, panel()), binding(BindingOutcome::NoTarget));
    EXPECT_EQ(resolve(index, "same", panel()).outcome, BindingOutcome::Ambiguous);
    EXPECT_EQ(index.remainingCandidateVisits(), 1u);
    const auto exhausted = index.bind("same", panel());
    ASSERT_FALSE(exhausted);
    EXPECT_EQ(exhausted.error().code, DependencyErrorCode::CandidateBudgetExceeded);
    EXPECT_EQ(index.remainingCandidateVisits(), 1u);
}

TEST(NameBinding, MakesBoundOrUnboundDependenciesAndNoneForEmptySlots)
{
    EXPECT_FALSE(makeDependency(DependencyKind::AttachedTo, 1, Facet::Geometry, "", binding(BindingOutcome::NoTarget)));
    const auto bound = makeDependency(
        DependencyKind::BoosterOf, 0, Facet::Data, "mass A", binding(BindingOutcome::Unique, {id(4)}), MissingTarget::FallsBack);
    ASSERT_TRUE(bound);
    auto expected = boundDependency(DependencyKind::BoosterOf, 0, id(4), Facet::Data, MissingTarget::FallsBack);
    expected.savedName = "mass A";
    EXPECT_EQ(bound.value(), expected);
    const auto ambiguous =
        makeDependency(DependencyKind::MemberOf, 3, Facet::Geometry, "twin", binding(BindingOutcome::Ambiguous, {id(2), id(3)}));
    ASSERT_TRUE(ambiguous);
    EXPECT_FALSE(ambiguous.value().target);
    EXPECT_EQ(ambiguous.value().slot, 3u);
    EXPECT_EQ(ambiguous.value().savedName, "twin");
    EXPECT_EQ(ambiguous.value().binding, BindingOutcome::Ambiguous);
    EXPECT_EQ(ambiguous.value().candidates, (std::vector<ObjectId>{id(2), id(3)}));
    EXPECT_EQ(ambiguous.value().onMissing, MissingTarget::Blocks);

    // Both kinds are accepted by the graph as they come from a load.
    DependencyGraph graph;
    for (const auto value : {1, 2, 3, 4})
    {
        ASSERT_TRUE(graph.addObject(id(value), ObjectInfo{.label = "x", .className = POINT_MASS}));
    }
    ASSERT_TRUE(graph.addDependency({.object = id(1), .facet = Facet::Geometry}, bound.value()));
    ASSERT_TRUE(graph.addDependency({.object = id(1), .facet = Facet::Geometry}, ambiguous.value()));
    // A malformed Unique binding without its target stays unbound, and the graph refuses it.
    const auto malformed = makeDependency(DependencyKind::MemberOf, 9, Facet::Geometry, "x", binding(BindingOutcome::Unique));
    ASSERT_TRUE(malformed);
    EXPECT_FALSE(malformed.value().target);
    EXPECT_EQ(graph.addDependency({.object = id(1), .facet = Facet::Geometry}, malformed.value()).error().code,
              DependencyErrorCode::InvalidDependency);
}

TEST(NameBinding, IndexesLiveGraphObjectsInIdentityOrder)
{
    DependencyGraph graph;
    ASSERT_TRUE(graph.addObject(id(9), ObjectInfo{.label = "twin", .className = POINT_MASS}));
    ASSERT_TRUE(graph.addObject(id(3), ObjectInfo{.label = "twin", .className = POINT_MASS}));
    ASSERT_TRUE(graph.addObject(id(5), ObjectInfo{.label = "gone", .className = POINT_MASS}));
    ASSERT_TRUE(graph.removeObject(id(5)));
    auto index = NameIndex::fromGraph(graph, NameIndexLimits{.maxCandidateVisits = 3});
    EXPECT_EQ(resolve(index, "twin", panel()), binding(BindingOutcome::Ambiguous, {id(3), id(9)}));
    EXPECT_EQ(resolve(index, "gone", panel()), binding(BindingOutcome::Missing));
    EXPECT_EQ(index.remainingCandidateVisits(), 1u);
}

TEST(NameBinding, RendersCurrentLabelsOnlyWhenAReloadKeepsTheirMeaning)
{
    DependencyGraph graph;
    ASSERT_TRUE(graph.addObject(id(1), ObjectInfo{.label = "mass A", .className = POINT_MASS}));
    ASSERT_TRUE(graph.addObject(id(2), ObjectInfo{.label = "mass B", .className = POINT_MASS}));
    ASSERT_TRUE(graph.addObject(id(3), ObjectInfo{.label = "center", .className = CENTER_OF_MASS}));
    ASSERT_TRUE(graph.addObject(id(4), ObjectInfo{.label = "twin", .className = POINT_MASS}));
    ASSERT_TRUE(graph.addObject(id(5), ObjectInfo{.label = "twin", .className = POINT_MASS}));
    const auto render = [&](const Dependency& dependency, const CandidateFilter& filter = centerMembers())
    {
        auto index = NameIndex::fromGraph(graph);
        auto result = motrack::model::renderName(graph, dependency, index, filter);
        EXPECT_TRUE(result);
        return *std::move(result);
    };
    auto member = loaded(DependencyKind::MemberOf, 0, "mass A", binding(BindingOutcome::Unique, {id(1)}));

    // A renamed target is written under its new label, never by patching the old text.
    ASSERT_TRUE(graph.renameObject(id(1), "élève β & <A>"));
    auto rendered = render(member);
    EXPECT_EQ(rendered.name, "élève β & <A>");
    EXPECT_EQ(rendered.status, RenderStatus::Faithful);
    EXPECT_EQ(rendered.reload, binding(BindingOutcome::Unique, {id(1)}));

    // Renaming another mass to the same label would make Tracker's reload ambiguous.
    ASSERT_TRUE(graph.renameObject(id(2), "élève β & <A>"));
    rendered = render(member);
    EXPECT_EQ(rendered.status, RenderStatus::Ambiguous);
    EXPECT_EQ(rendered.reload.candidates, (std::vector<ObjectId>{id(1), id(2)}));
    // A same-named object of an unverified class could capture the member.
    ASSERT_TRUE(graph.renameObject(id(2), "mass B"));
    ASSERT_TRUE(graph.renameObject(id(3), "élève β & <A>"));
    EXPECT_EQ(render(member).status, RenderStatus::Unclassified);
    ASSERT_TRUE(graph.renameObject(id(3), "center"));
    // The field's own filter must accept the target class, or the reference would be lost.
    EXPECT_EQ(render(member, sumMembers()).status, RenderStatus::Lost);
    EXPECT_EQ(render(member, panel()).status, RenderStatus::Faithful);

    // A removed target has no current label; its saved name is returned and a reload is still checked.
    auto removed = *graph.removeObject(id(1));
    rendered = render(member);
    EXPECT_EQ(rendered.status, RenderStatus::Dangling);
    EXPECT_EQ(rendered.name, "mass A");
    EXPECT_EQ(rendered.reload, binding(BindingOutcome::Missing));
    ASSERT_TRUE(graph.addObject(id(6), ObjectInfo{.label = "mass A", .className = POINT_MASS}));
    rendered = render(member);
    EXPECT_EQ(rendered.status, RenderStatus::Dangling);
    EXPECT_EQ(rendered.reload, binding(BindingOutcome::Unique, {id(6)})); // Saving the old name would capture mass 6.
    const auto native = boundDependency(DependencyKind::AttachedTo, 0, id(1), Facet::Geometry);
    rendered = render(native);
    EXPECT_EQ(rendered.status, RenderStatus::Dangling);
    EXPECT_TRUE(rendered.name.empty());
    EXPECT_EQ(rendered.reload, binding(BindingOutcome::Missing));
    ASSERT_TRUE(graph.restoreObject(removed));
    EXPECT_EQ(render(native, panel()).status, RenderStatus::Faithful);

    // Unresolved references are written back verbatim while a reload still fails the same way.
    const auto missing = loaded(DependencyKind::MemberOf, 1, "mass C", binding(BindingOutcome::Missing));
    rendered = render(missing);
    EXPECT_EQ(rendered.name, "mass C");
    EXPECT_EQ(rendered.status, RenderStatus::Faithful);
    ASSERT_TRUE(graph.renameObject(id(2), "mass C"));
    rendered = render(missing);
    EXPECT_EQ(rendered.status, RenderStatus::Rebinds); // Saving would silently create membership.
    EXPECT_EQ(rendered.reload, binding(BindingOutcome::Unique, {id(2)}));

    const auto twin = loaded(DependencyKind::MemberOf, 2, "twin", binding(BindingOutcome::Ambiguous, {id(4), id(5)}));
    EXPECT_EQ(render(twin).status, RenderStatus::Faithful);
    ASSERT_TRUE(graph.renameObject(id(5), "mass C"));
    EXPECT_EQ(render(twin).status, RenderStatus::Rebinds);
    ASSERT_TRUE(graph.renameObject(id(4), "other"));
    EXPECT_EQ(render(twin).status, RenderStatus::Lost);
    ASSERT_TRUE(graph.renameObject(id(4), "twin"));
    ASSERT_TRUE(graph.renameObject(id(5), "twin"));
    ASSERT_TRUE(graph.addObject(id(7), ObjectInfo{.label = "twin", .className = POINT_MASS}));
    EXPECT_EQ(render(twin).status, RenderStatus::Ambiguous); // A different candidate set.

    // An exhausted budget is reported rather than treated as a missing name.
    auto small = NameIndex::fromGraph(graph, NameIndexLimits{.maxCandidateVisits = 1});
    const auto refused = motrack::model::renderName(graph, twin, small, centerMembers());
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, DependencyErrorCode::CandidateBudgetExceeded);
}

std::string property(std::string_view name, std::string_view text)
{
    return "<property name='" + std::string(name) + "' type='string'><![CDATA[" + std::string(text) + "]]></property>";
}

std::string track(std::string_view type, std::string_view name, std::string_view fields = {})
{
    return "<property name='item'><object class='" + std::string(type) + "'>" + property("name", name) + std::string(fields) +
           "</object></property>";
}

std::string collection(std::string_view name, std::string_view items)
{
    return "<property name='" + std::string(name) + "' type='collection' class='java.util.ArrayList'>" + std::string(items) + "</property>";
}

std::string strings(std::string_view name, std::string_view cells)
{
    return "<property name='" + std::string(name) + "' type='array' class='[Ljava.lang.String;'>" + std::string(cells) + "</property>";
}

TEST(NameBinding, MatchesTheSourceReferenceIndexClassification)
{
    // Duplicates across types, nested dependents, unknown classes and missing names in every typed field.
    const auto tracks =
        track(POINT_MASS, "same") + track(VECTOR, "same") + track(POINT_MASS, "duplicate") + track(POINT_MASS, "duplicate") +
        track("FutureMass", "uncertain") + track(POINT_MASS, "uncertain") + track(DYNAMIC_PARTICLE, "particle") +
        track(POLAR_PARTICLE, "polar") + track(DYNAMIC_PARTICLE, "twin particle") + track(POLAR_PARTICLE, "twin particle") +
        track(CENTER_OF_MASS,
              "center",
              collection("masses",
                         property("item", "same") + property("item", "duplicate") + property("item", "uncertain") +
                             property("item", "center") + property("item", "missing"))) +
        track(
            VECTOR_SUM, "sum", collection("vectors", property("item", "same") + property("item", "uncertain") + property("item", "sum"))) +
        track(DYNAMIC_PARTICLE, "boosted", property("booster", "same")) +
        track(DYNAMIC_PARTICLE, "nested", property("booster", "particle")) +
        track(DYNAMIC_SYSTEM,
              "system",
              strings("particles", property("[0]", "twin particle") + property("[1]", "same")) + property("booster", "duplicate")) +
        track(DYNAMIC_SYSTEM, "pair", strings("particles", property("[0]", "particle") + property("[1]", "polar")));
    const auto xml = "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'><property name='tracks'>" + tracks +
                     "</property>" + property("selectedtrack", "duplicate") + property("referenceframe", "same") + "</object>";
    const auto imported = motrack::parseTrackerProject(xml);
    const auto source = motrack::indexTrackerReferences(imported.source, imported.project);
    ASSERT_TRUE(source.malformedPaths.empty());

    std::vector<NamedObject> objects;
    objects.reserve(source.tracks.size());
    for (const auto& sourceTrack : source.tracks)
    {
        objects.push_back(NamedObject{
            .id = id(sourceTrack.sourceIndex + 1), .label = sourceTrack.originalName.value_or(""), .className = sourceTrack.className});
    }
    NameIndex index(objects);
    const std::map<motrack::TrackerReferenceKind, const CandidateFilter*> filters{
        {motrack::TrackerReferenceKind::SelectedTrack, &panel()},
        {motrack::TrackerReferenceKind::ReferenceFrame, &panel()},
        {motrack::TrackerReferenceKind::CenterOfMassMember, &centerMembers()},
        {motrack::TrackerReferenceKind::VectorSumMember, &sumMembers()},
        {motrack::TrackerReferenceKind::ModelBooster, &boosters()},
        {motrack::TrackerReferenceKind::DynamicSystemMember, &systemMembers()},
    };
    const std::map<motrack::TrackerReferenceResolution, BindingOutcome> outcomes{
        {motrack::TrackerReferenceResolution::Resolved, BindingOutcome::Unique},
        {motrack::TrackerReferenceResolution::Ambiguous, BindingOutcome::Ambiguous},
        {motrack::TrackerReferenceResolution::Missing, BindingOutcome::Missing},
        {motrack::TrackerReferenceResolution::NoTarget, BindingOutcome::NoTarget},
        {motrack::TrackerReferenceResolution::Unclassified, BindingOutcome::Unclassified},
    };
    std::map<BindingOutcome, int> seen;
    ASSERT_EQ(source.references.size(), 17u);
    for (const auto& reference : source.references)
    {
        SCOPED_TRACE(reference.path);
        const auto expectedOutcome = outcomes.at(reference.resolution);
        const auto name =
            expectedOutcome == BindingOutcome::NoTarget ? std::nullopt : std::optional<std::string_view>(reference.originalName);
        const auto result = resolve(index, name, *filters.at(reference.kind));
        std::vector<ObjectId> candidates;
        candidates.reserve(reference.candidateSourceIndices.size());
        for (const auto sourceIndex : reference.candidateSourceIndices)
        {
            candidates.push_back(id(sourceIndex + 1));
        }
        EXPECT_EQ(result.outcome, expectedOutcome);
        EXPECT_EQ(result.candidates, candidates);
        ++seen[result.outcome];
    }
    // Every outcome except an empty slot occurs, so the comparison is not vacuous.
    EXPECT_GT(seen[BindingOutcome::Unique], 0);
    EXPECT_GT(seen[BindingOutcome::Ambiguous], 0);
    EXPECT_GT(seen[BindingOutcome::Missing], 0);
    EXPECT_GT(seen[BindingOutcome::Unclassified], 0);
}

} // namespace
