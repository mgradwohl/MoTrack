#include "model/commands.hpp"

#include "../corpus_documents.hpp"
#include "../osp_layout.hpp"
#include "command_checks.hpp"
#include "document_dump.hpp"
#include "legacy_view.hpp"
#include "model/document_history.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using motrack::model::AllocationRange;
using motrack::model::ApplyMode;
using motrack::model::Batch;
using motrack::model::Command;
using motrack::model::CommandErrorCode;
using motrack::model::CommandValue;
using motrack::model::DependencyKind;
using motrack::model::DocumentHistory;
using motrack::model::Facet;
using motrack::model::Family;
using motrack::model::FrameElement;
using motrack::model::Insert;
using motrack::model::Mark;
using motrack::model::ObjectId;
using motrack::model::ObjectSlot;
using motrack::model::ObjectState;
using motrack::model::Remove;
using motrack::model::Revision;
using motrack::model::Role;
using motrack::model::SetField;
using motrack::model::TrackerDocument;

constexpr std::string_view TRACKER = "org.opensourcephysics.cabrillo.tracker.";

std::string cls(std::string_view name)
{
    return std::string(TRACKER) + std::string(name);
}

std::string text(std::string_view name, std::string_view value)
{
    return "<property name='" + std::string(name) + "' type='string'><![CDATA[" + std::string(value) + "]]></property>";
}

std::string number(std::string_view name, std::string_view value, std::string_view type = "double")
{
    return "<property name='" + std::string(name) + "' type='" + std::string(type) + "'>" + std::string(value) + "</property>";
}

std::string track(std::string_view type, std::string_view name, std::string_view fields = {})
{
    return "<property name='item'><object class='" + cls(type) + "'>" + text("name", name) + std::string(fields) + "</object></property>";
}

std::string mark(int frame, std::string_view x, std::string_view y, std::string_view extra = {})
{
    return "<property name='[" + std::to_string(frame) + "]' type='object'><object class='" + cls("PointMass$FrameData") + "'>" +
           number("x", x) + number("y", y) + std::string(extra) + "</object></property>";
}

std::string marks(std::string_view frames)
{
    return "<property name='framedata' type='array' class='[L" + cls("PointMass$FrameData") + ";'>" + std::string(frames) + "</property>";
}

std::string strings(std::string_view name, std::string_view cells)
{
    return "<property name='" + std::string(name) + "' type='array' class='[Ljava.lang.String;'>" + std::string(cells) + "</property>";
}

std::string coordKey(int frame)
{
    return "<property name='[" + std::to_string(frame) +
           "]' type='object'><object class='org.opensourcephysics.media.core.ImageCoordSystem$FrameData'>" + number("xorigin", "10.0") +
           number("yorigin", "20.0") + number("angle", "0.0") + number("xscale", "2.0") + number("yscale", "2.0") + "</object></property>";
}

std::string panel(std::string_view properties)
{
    return motrack::test::ospLayout("<object class='" + cls("TrackerPanel") + "'>" + std::string(properties) + "</object>");
}

/// A media-free experiment: a bound point mass with saved key frames and an unknown frame property, one without marks,
/// a vector, and untyped owners of every reference-only slot kind, including a malformed one.
std::string experiment(bool coordinates = true)
{
    const auto clip = "<property name='videoclip' type='object'><object class='org.opensourcephysics.media.core.VideoClip'>" +
                      number("startframe", "0", "int") + number("stepsize", "1", "int") + number("stepcount", "10", "int") +
                      number("starttime", "0.0") + "</object></property>";
    const auto control = "<property name='clipcontrol' type='object'><object class='org.opensourcephysics.media.core.StepperClipControl'>" +
                         number("delta_t", "40.0") + "</object></property>";
    const auto coords = "<property name='coords' type='object'><object class='org.opensourcephysics.media.core.ImageCoordSystem'>" +
                        std::string("<property name='framedata' type='array' "
                                    "class='[Lorg.opensourcephysics.media.core.ImageCoordSystem$FrameData;'>") +
                        coordKey(0) + "</property></object></property>";
    const auto keys = "<property name='keyFrames' type='array' class='[I'><property name='array' type='string'>{0,1}</property></property>";
    const auto tracks =
        "<property name='tracks' type='collection' class='java.util.ArrayList'>" +
        track(
            "PointMass", "A", number("mass", "2.0") + marks(mark(0, "1.0", "2.0") + mark(1, "3.0", "4.0", number("extra", "7.0"))) + keys) +
        track("PointMass", "B") +
        track("Vector",
              "v",
              "<property name='framedata' type='array' class='[L" + cls("Vector$FrameData") +
                  ";'><property name='[1]' type='object'><object class='" + cls("Vector$FrameData") + "'>" + number("xtail", "1.0") +
                  number("ytail", "2.0") + number("xcomponent", "3.0") + number("ycomponent", "4.0") + "</object></property></property>") +
        track("CenterOfMass",
              "cm",
              "<property name='masses' type='collection' class='java.util.ArrayList'>" + text("item", "A") + text("item", "missing") +
                  "</property>") +
        track("TapeMeasure", "tape", strings("attachments", text("[0]", "A") + text("[1]", "v"))) +
        track("DynamicParticle", "model", text("booster", "A")) + track("Protractor", "bad", strings("attachments", text("[x]", "A"))) +
        "</property>";
    return panel(text("mass_unit", "g") + clip + control + (coordinates ? coords : std::string()) + tracks);
}

TrackerDocument import(const std::string& xml)
{
    auto source = motrack::osp::parse(xml);
    if (!source)
    {
        throw std::runtime_error(source.error().message);
    }
    auto document = TrackerDocument::importBaseline(std::move(*source));
    if (!document)
    {
        throw std::runtime_error(document.error().message);
    }
    return std::move(*document);
}

const ObjectSlot& named(const TrackerDocument& document, std::string_view className)
{
    for (const auto& object : document.objects())
    {
        if (object.className == className)
        {
            return object;
        }
    }
    throw std::runtime_error("No object of class " + std::string(className));
}

template <typename State> const State& stateOf(const TrackerDocument& document, ObjectId id)
{
    return std::get<State>(*document.object(id)->value);
}

using motrack::test::required;

constexpr ObjectId A{1};
constexpr ObjectId B{2};
constexpr ObjectId V{3};

CommandValue element(Mark value)
{
    return FrameElement<Mark>{.value = value};
}

/// Everything a refused command must leave unchanged.
struct Snapshot
{
    std::string document;
    std::string graph;
    std::uint64_t revision;
    std::uint64_t documentRevision;
    bool canUndo;
    bool canRedo;
    bool modified;
    bool trackerModified;

    bool operator==(const Snapshot&) const = default;
};

Snapshot snapshot(const TrackerDocument& document, const DocumentHistory& history)
{
    return {.document = motrack::test::documentDumpText(document),
            .graph = motrack::test::graphText(document),
            .revision = history.revision(),
            .documentRevision = history.documentRevision(),
            .canUndo = history.canUndo(),
            .canRedo = history.canRedo(),
            .modified = history.isModified(),
            .trackerModified = history.isModified(motrack::model::SaveTarget::Tracker)};
}

CommandErrorCode refusal(TrackerDocument& document, DocumentHistory& history, const Command& command)
{
    const auto before = snapshot(document, history);
    const auto result = history.apply(document, command);
    EXPECT_FALSE(result);
    EXPECT_EQ(snapshot(document, history), before);
    return result ? CommandErrorCode::NothingToUndo : result.error().code;
}

TEST(Commands, SetFieldEditsOneFieldAndRecordsItsExactInverse)
{
    const auto original = import(experiment());
    auto document = original;
    DocumentHistory history;
    const auto center = named(document, cls("CenterOfMass")).id;
    const auto computed = document.graph().revision();

    ASSERT_TRUE(history.apply(document, SetField{.object = A, .field = "mass", .before = 2.0, .after = 3.5}));
    EXPECT_EQ(stateOf<motrack::model::PointMassState>(document, A).mass, motrack::model::Read<double>(3.5));
    // A mass is a Definition: the mass, its own positions and the center of mass that weights them are invalidated.
    EXPECT_FALSE(document.graph().isCurrent({.object = A, .facet = Facet::Definition}, computed));
    EXPECT_FALSE(document.graph().isCurrent({.object = center, .facet = Facet::Geometry}, computed));
    EXPECT_TRUE(document.graph().isCurrent({.object = B, .facet = Facet::Definition}, computed));
    // The original value shares nothing that changed.
    EXPECT_EQ(stateOf<motrack::model::PointMassState>(original, A).mass, motrack::model::Read<double>(2.0));
    EXPECT_EQ(original.graph().revision(), computed);

    // A rename changes the label only.
    const auto renamed = document.graph().revision();
    ASSERT_TRUE(history.apply(document, SetField{.object = A, .field = "name", .before = std::string("A"), .after = std::string("Z")}));
    EXPECT_EQ(document.graph().object(A)->label, std::optional<std::string>("Z"));
    EXPECT_TRUE(document.graph().planSince(renamed).order.empty());
    // Unit labels change no value and leave the graph alone.
    const auto labels = document.graph().revision();
    const auto shared = document.sharedGraph();
    const auto panel = document.singleton(Role::Panel)->id;
    ASSERT_TRUE(
        history.apply(document, SetField{.object = panel, .field = "mass_unit", .before = std::string("g"), .after = std::string("kg")}));
    EXPECT_EQ(document.graph().revision(), labels);
    EXPECT_EQ(document.sharedGraph(), shared);

    while (history.canUndo())
    {
        ASSERT_TRUE(history.undo(document));
    }
    EXPECT_EQ(document, original);
    EXPECT_EQ(motrack::test::documentDumpText(document), motrack::test::documentDumpText(original));
    EXPECT_TRUE(motrack::test::graphMatchesRebuild(document));
    EXPECT_GT(document.graph().revision(), labels);
    while (history.canRedo())
    {
        ASSERT_TRUE(history.redo(document));
    }
    EXPECT_EQ(motrack::legacyProject(document).massUnit, "kg");
    EXPECT_EQ(motrack::legacyProject(document).pointMasses.front().name, "Z");
}

TEST(Commands, RefusesInvalidCommandsWithoutChangingTheDocumentHistoryRevisionsOrSaveHeads)
{
    auto document = import(experiment());
    DocumentHistory history;
    ASSERT_TRUE(history.apply(document, SetField{.object = A, .field = "mass", .before = 2.0, .after = 3.0}));
    history.markSaved(motrack::model::SaveTarget::Tracker);
    ASSERT_TRUE(history.undo(document));
    const auto panel = document.singleton(Role::Panel)->id;
    const auto clip = document.singleton(Role::Clip)->id;
    const auto control = document.singleton(Role::ClipControl)->id;
    const auto coords = document.singleton(Role::Coordinates)->id;
    const auto center = named(document, cls("CenterOfMass")).id;
    const auto mass = [](CommandValue before, CommandValue after)
    {
        return SetField{.object = A, .field = "mass", .before = std::move(before), .after = std::move(after)};
    };
    const std::vector<std::pair<Command, CommandErrorCode>> cases{
        {SetField{.object = ObjectId{999}, .field = "mass", .before = 2.0, .after = 3.0}, CommandErrorCode::UnknownObject},
        {SetField{.object = A, .field = "color", .before = 2.0, .after = 3.0}, CommandErrorCode::UnknownField},
        {SetField{.object = panel, .field = "description", .before = motrack::osp::DecodeError::Missing, .after = std::string("x")},
         CommandErrorCode::UnknownField},
        {SetField{.object = center, .field = "name", .before = std::string("cm"), .after = std::string("x")},
         CommandErrorCode::UnknownField},
        {SetField{.object = A, .field = "framedata", .before = {}, .after = element({.x = 1, .y = 2})}, CommandErrorCode::InvalidElement},
        {SetField{.object = A, .field = "mass", .frame = 3, .before = 2.0, .after = 3.0}, CommandErrorCode::InvalidElement},
        {SetField{.object = A, .field = "framedata", .frame = -1, .before = {}, .after = element({.x = 1, .y = 2})},
         CommandErrorCode::InvalidElement},
        {mass(9.0, 3.0), CommandErrorCode::StaleValue},
        {mass(2.0, std::numeric_limits<double>::quiet_NaN()), CommandErrorCode::InvalidValue},
        {mass(2.0, std::numeric_limits<double>::infinity()), CommandErrorCode::InvalidValue},
        {mass(2.0, 0.0), CommandErrorCode::InvalidValue},
        {mass(2.0, std::string("heavy")), CommandErrorCode::InvalidValue},
        {mass(2.0, motrack::osp::DecodeError::Missing), CommandErrorCode::InvalidValue},
        {mass(2.0, 2.0), CommandErrorCode::NoChange},
        {SetField{.object = A, .field = "name", .before = std::string("A"), .after = std::string("a,b")}, CommandErrorCode::InvalidValue},
        {SetField{.object = A, .field = "name", .before = std::string("A"), .after = std::string()}, CommandErrorCode::InvalidValue},
        {SetField{.object = panel, .field = "mass_unit", .before = std::string("g"), .after = std::string("kg2")},
         CommandErrorCode::InvalidValue},
        {SetField{.object = clip, .field = "startframe", .before = 0, .after = -1}, CommandErrorCode::InvalidValue},
        {SetField{.object = clip, .field = "stepsize", .before = 1, .after = 0}, CommandErrorCode::InvalidValue},
        {SetField{.object = control, .field = "delta_t", .before = 40.0, .after = 0.0}, CommandErrorCode::InvalidValue},
        {SetField{.object = A, .field = "framedata", .frame = 5, .before = {}, .after = element({.x = std::nan(""), .y = 1})},
         CommandErrorCode::InvalidValue},
        {SetField{.object = A,
                  .field = "framedata",
                  .frame = 5,
                  .before = {},
                  .after = FrameElement<Mark>{.value = {.x = 1, .y = 1}, .node = motrack::osp::NodeId{3}}},
         CommandErrorCode::InvalidValue},
        {SetField{.object = A, .field = "framedata", .frame = 5, .before = {}, .after = FrameElement<motrack::model::VectorSample>{}},
         CommandErrorCode::InvalidValue},
        {SetField{.object = V,
                  .field = "framedata",
                  .frame = 2,
                  .before = {},
                  .after = FrameElement<motrack::model::VectorSample>{.value = {.xTail = std::numeric_limits<double>::max(),
                                                                                .yTail = 0,
                                                                                .xComponent = std::numeric_limits<double>::max(),
                                                                                .yComponent = 0}}},
         CommandErrorCode::InvalidValue},
        {SetField{.object = coords,
                  .field = "framedata",
                  .frame = 3,
                  .before = {},
                  .after =
                      FrameElement<motrack::model::CoordKey>{.value = {.xOrigin = 1, .yOrigin = 2, .angle = 0, .xScale = 0, .yScale = 1}}},
         CommandErrorCode::InvalidValue},
        {SetField{.object = A, .field = "keyFrames", .before = std::vector<std::int32_t>{0, 1}, .after = std::vector<std::int32_t>{1, 0}},
         CommandErrorCode::InvalidValue},
        {SetField{.object = A, .field = "framedata", .frame = 9, .before = {}, .after = {}}, CommandErrorCode::NoChange},
        {Batch{}, CommandErrorCode::NoChange},
        // A batch commits all of its members or none.
        {Batch{.commands = {mass(2.0, 3.0),
                            SetField{.object = A, .field = "name", .before = std::string("stale"), .after = std::string("x")}}},
         CommandErrorCode::StaleValue},
        {Remove{.object = panel}, CommandErrorCode::NotRemovable},
        {Remove{.object = clip}, CommandErrorCode::NotRemovable},
        {Remove{.object = ObjectId{999}}, CommandErrorCode::UnknownObject},
    };
    for (const auto& [command, code] : cases)
    {
        SCOPED_TRACE(motrack::model::commandErrorName(code));
        EXPECT_EQ(refusal(document, history, command), code);
    }
    // The history kept its redo entry and its saved head through every refusal.
    EXPECT_TRUE(history.canRedo());
    EXPECT_TRUE(history.isModified(motrack::model::SaveTarget::Tracker));
    ASSERT_TRUE(history.redo(document));
    EXPECT_FALSE(history.isModified(motrack::model::SaveTarget::Tracker));
}

TEST(Commands, InsertionTakesFreshIdentitiesAndUndoNeverRewindsTheAllocators)
{
    const auto original = import(experiment(false));
    auto document = original;
    DocumentHistory history;
    const auto next = document.nextObjectId();
    auto created = motrack::model::createTrack(document, Family::PointMass, "C");
    ASSERT_TRUE(created);
    EXPECT_EQ(created->object.id, ObjectId{next});
    EXPECT_EQ(created->trackPosition, std::optional<std::size_t>(2)); // After the point masses, before the vector.
    const auto before = snapshot(document, history);
    history.markSaved();
    ASSERT_TRUE(history.apply(document, *created));
    const auto afterCreation = motrack::test::documentDumpText(document);
    EXPECT_EQ(document.nextObjectId(), next + 1);
    EXPECT_EQ(document.trackOrder()[2], ObjectId{next});
    EXPECT_EQ(document.graph().object(ObjectId{next})->label, std::optional<std::string>("C"));
    EXPECT_EQ(motrack::legacyProject(document).pointMasses.back().name, "C");
    EXPECT_EQ(motrack::legacyProject(document).pointMasses.back().mass, 1.0);

    // Undo removes the created object from the document; the counter and the graph keep the identity from reuse.
    ASSERT_TRUE(history.undo(document));
    EXPECT_EQ(document.object(ObjectId{next}), nullptr);
    EXPECT_EQ(document.nextObjectId(), next + 1);
    EXPECT_TRUE(document.graph().wasRemoved(ObjectId{next}));
    EXPECT_TRUE(history.isModified()); // The advanced counter is unsaved.
    EXPECT_NE(snapshot(document, history).document, before.document);
    // A fresh creation cannot reuse the identity, and redo brings back the same object.
    EXPECT_EQ(motrack::model::freshIdentity(document, AllocationRange::Counter), ObjectId{next + 1});
    EXPECT_EQ(refusal(document, history, *created), CommandErrorCode::InvalidIdentity);
    ASSERT_TRUE(history.redo(document));
    EXPECT_EQ(motrack::test::documentDumpText(document), afterCreation);
    EXPECT_TRUE(motrack::test::graphMatchesRebuild(document));

    // Countdown identities: an inserted coordinate system takes the value below the low-water mark, which never rises.
    const auto lowWater = document.countdownLowWater();
    auto coordinates = motrack::model::createSingleton(
        document,
        Role::Coordinates,
        motrack::model::CoordinatesState{
            .fixedOrigin = true, .fixedAngle = true, .fixedScale = true, .locked = motrack::model::missing<bool>(), .keys = {}});
    ASSERT_TRUE(coordinates);
    EXPECT_EQ(coordinates->object.id, ObjectId{lowWater - 1});
    history.markSaved();
    ASSERT_TRUE(history.apply(document, *coordinates));
    EXPECT_EQ(document.countdownLowWater(), lowWater - 1);
    EXPECT_EQ(refusal(document,
                      history,
                      required(motrack::model::createSingleton(document, Role::Coordinates, motrack::model::CoordinatesState{}))),
              CommandErrorCode::InvalidObject); // One typed coordinate system.
    ASSERT_TRUE(history.undo(document));
    EXPECT_FALSE(history.isModified()); // The low-water mark is not saved before native version 7.
    EXPECT_EQ(document.countdownLowWater(), lowWater - 1);
    EXPECT_EQ(motrack::model::freshIdentity(document, AllocationRange::Countdown), ObjectId{lowWater - 2});
    ASSERT_TRUE(history.redo(document));
    EXPECT_EQ(document.singleton(Role::Coordinates)->id, ObjectId{lowWater - 1});

    // Malformed insertions.
    auto insert = required(motrack::model::createTrack(document, Family::Vector, "w"));
    const auto expectRefused = [&](const std::function<void(Insert&)>& change, CommandErrorCode code)
    {
        auto changed = insert;
        change(changed);
        EXPECT_EQ(refusal(document, history, changed), code);
    };
    expectRefused([](Insert& item) { item.object.binding = motrack::osp::NodeId{1}; }, CommandErrorCode::InvalidObject);
    expectRefused([](Insert& item) { item.object.className = cls("PointMass"); }, CommandErrorCode::InvalidObject);
    expectRefused(
        [](Insert& item)
        {
            item.object.value = std::make_shared<const motrack::model::TypedValue>(motrack::model::UnknownState{});
            item.trackPosition.reset();
        },
        CommandErrorCode::InvalidObject);
    expectRefused([](Insert& item) { item.object.role = Role::Panel; }, CommandErrorCode::InvalidObject);
    expectRefused([](Insert& item) { item.object.state = ObjectState::Removed; }, CommandErrorCode::InvalidObject);
    expectRefused([](Insert& item) { item.object.id = ObjectId{item.object.id.value + 1}; }, CommandErrorCode::InvalidIdentity);
    expectRefused([](Insert& item) { item.object.id = A; }, CommandErrorCode::InvalidIdentity);
    expectRefused([](Insert& item) { item.object.range = AllocationRange::Countdown; }, CommandErrorCode::InvalidIdentity);
    expectRefused([](Insert& item) { item.position = 1000; }, CommandErrorCode::InvalidPosition);
    expectRefused([](Insert& item) { item.trackPosition.reset(); }, CommandErrorCode::InvalidPosition);
    expectRefused([](Insert& item) { item.trackPosition = 1000; }, CommandErrorCode::InvalidPosition);
    expectRefused([](Insert& item) { item.restores = motrack::model::RemovedObject{.id = item.object.id}; },
                  CommandErrorCode::InvalidObject);
    expectRefused(
        [](Insert& item)
        {
            item.object.value =
                std::make_shared<const motrack::model::TypedValue>(motrack::model::VectorState{.name = std::string("bad;name")});
        },
        CommandErrorCode::InvalidValue);
    expectRefused(
        [](Insert& item)
        {
            motrack::model::VectorState state{.name = std::string("w")};
            state.samples.set(-1, {});
            item.object.value = std::make_shared<const motrack::model::TypedValue>(state);
        },
        CommandErrorCode::InvalidValue);
}

TEST(Commands, CreationStopsAtTheCountdownLowWaterMark)
{
    auto parts = TrackerDocument().parts();
    parts.nextObjectId = parts.countdownLowWater - 1;
    auto document = *TrackerDocument::assemble(parts);
    DocumentHistory history;
    const auto last = motrack::model::createTrack(document, Family::PointMass, "last");
    ASSERT_TRUE(last);
    ASSERT_TRUE(history.apply(document, *last));
    EXPECT_FALSE(document.hasCounterCapacity());
    EXPECT_FALSE(motrack::model::createTrack(document, Family::Vector, "overflow"));
    EXPECT_FALSE(motrack::model::freshIdentity(document, AllocationRange::Countdown));
    EXPECT_FALSE(motrack::model::createSingleton(document, Role::Coordinates, motrack::model::CoordinatesState{}));
    EXPECT_FALSE(motrack::model::createTrack(TrackerDocument(), Family::Unknown, "x"));
    EXPECT_FALSE(motrack::model::createSingleton(TrackerDocument(), Role::Track, motrack::model::PointMassState{}));
    ASSERT_TRUE(history.undo(document));
    EXPECT_FALSE(motrack::model::createTrack(document, Family::Vector, "reused"));
    ASSERT_TRUE(history.redo(document));
    EXPECT_EQ(motrack::legacyProject(document).pointMasses.front().id, parts.countdownLowWater - 1);
}

TEST(Commands, RemovalKeepsBoundSlotsWhileReferencesDangleAndUndoBindsThemAgain)
{
    const auto original = import(experiment());
    auto document = original;
    DocumentHistory history;
    const auto references = original.graph().referencesTo(A);
    ASSERT_EQ(references.size(), 3u); // Center-of-mass member, tape attachment, model booster.
    const auto rendered = motrack::test::renderedReferences(original);

    ASSERT_TRUE(history.apply(document, Remove{.object = A}));
    const auto* removed = document.object(A);
    ASSERT_NE(removed, nullptr);
    EXPECT_EQ(removed->state, ObjectState::Removed);
    EXPECT_EQ(removed->binding, original.object(A)->binding);
    EXPECT_EQ(std::ranges::count(document.trackOrder(), A), 0);
    EXPECT_FALSE(document.graph().contains(A));
    EXPECT_TRUE(document.graph().wasRemoved(A));
    for (const auto& reference : references)
    {
        const auto* dependency = document.graph().dependency(reference);
        ASSERT_NE(dependency, nullptr);
        EXPECT_EQ(dependency->target, A);
    }
    const auto plan = document.graph().plan();
    EXPECT_EQ(std::ranges::count(plan.references, motrack::model::ReferenceState::Dangling, &motrack::model::ReferenceDiagnostic::state),
              3);
    std::size_t dangling = 0;
    for (const auto& line : motrack::test::renderedReferences(document))
    {
        dangling += line.ends_with("status " + std::to_string(static_cast<int>(motrack::model::RenderStatus::Dangling))) ? 1 : 0;
    }
    EXPECT_EQ(dangling, 3u);
    EXPECT_TRUE(motrack::test::graphMatchesRebuild(document));
    EXPECT_EQ(motrack::legacyProject(document).pointMasses.size(), 1u);
    EXPECT_EQ(refusal(document, history, SetField{.object = A, .field = "mass", .before = 2.0, .after = 3.0}),
              CommandErrorCode::UnknownObject);

    ASSERT_TRUE(history.undo(document));
    EXPECT_EQ(motrack::test::documentDumpText(document), motrack::test::documentDumpText(original));
    EXPECT_EQ(motrack::test::renderedReferences(document), rendered);
    EXPECT_EQ(document, original);

    // An owner takes its own dependencies and reference issues with it, and brings them back in document order.
    const auto protractor = named(document, cls("Protractor")).id;
    const auto tape = named(document, cls("TapeMeasure")).id;
    ASSERT_EQ(document.referenceIssues().size(), 1u);
    ASSERT_TRUE(history.apply(document, Batch{.commands = {Remove{.object = protractor}, Remove{.object = tape}}}));
    EXPECT_TRUE(document.referenceIssues().empty());
    EXPECT_TRUE(document.graph().dependencies({.object = tape, .facet = Facet::Geometry}).empty());
    EXPECT_EQ(document.graph().referencesTo(A).size(), 2u);
    EXPECT_TRUE(motrack::test::graphMatchesRebuild(document));
    ASSERT_TRUE(history.undo(document));
    EXPECT_EQ(motrack::test::documentDumpText(document), motrack::test::documentDumpText(original));
    EXPECT_TRUE(motrack::test::graphMatchesRebuild(document));

    // A created object leaves the document entirely; the counter keeps its identity.
    auto created = required(motrack::model::createTrack(document, Family::PointMass, "C"));
    ASSERT_TRUE(history.apply(document, created));
    ASSERT_TRUE(history.apply(document, Remove{.object = created.object.id}));
    EXPECT_EQ(document.object(created.object.id), nullptr);
    ASSERT_TRUE(history.undo(document));
    EXPECT_EQ(document.object(created.object.id)->state, ObjectState::Live);
    EXPECT_TRUE(motrack::test::graphMatchesRebuild(document));
}

TEST(Commands, MarksKeepTheirFrameObjectsAndTheKeyFramesB1Writes)
{
    const auto original = import(experiment());
    auto document = original;
    DocumentHistory history;
    const auto& baseline = stateOf<motrack::model::PointMassState>(original, A);
    const auto node = baseline.marks.find(1)->node;
    ASSERT_TRUE(node);
    const auto keys = [&](ObjectId id)
    {
        return stateOf<motrack::model::PointMassState>(document, id).keyFrames;
    };
    const auto apply = [&](ObjectId id, std::int32_t frame, std::optional<Mark> value)
    {
        auto command = motrack::model::setMark(document, id, frame, value);
        ASSERT_TRUE(command);
        ASSERT_TRUE(history.apply(document, *command));
    };

    // Changing a saved key frame's mark keeps the keys and the frame object with its unknown property.
    auto changed = motrack::model::setMark(document, A, 1, Mark{.x = 5, .y = 6});
    ASSERT_TRUE(changed);
    EXPECT_TRUE(std::holds_alternative<SetField>(*changed));
    ASSERT_TRUE(history.apply(document, *changed));
    EXPECT_EQ(stateOf<motrack::model::PointMassState>(document, A).marks.find(1)->node, node);
    EXPECT_EQ(keys(A), baseline.keyFrames);
    // A new mark is a key frame; removing a saved mark drops its key.
    apply(A, 4, Mark{.x = 1, .y = 1});
    EXPECT_EQ(keys(A), motrack::model::Read<std::vector<std::int32_t>>(std::vector<std::int32_t>{0, 1, 4}));
    apply(A, 0, std::nullopt);
    EXPECT_EQ(keys(A), motrack::model::Read<std::vector<std::int32_t>>(std::vector<std::int32_t>{1, 4}));
    // Returning to the baseline's marks restores the baseline's own reading, not an equal edit.
    apply(A, 4, std::nullopt);
    apply(A, 0, Mark{.x = 1, .y = 2});
    EXPECT_EQ(keys(A), baseline.keyFrames);
    apply(A, 1, Mark{.x = 3, .y = 4});
    EXPECT_EQ(keys(A), baseline.keyFrames);
    // A mark added again after its removal is a new frame object.
    EXPECT_EQ(stateOf<motrack::model::PointMassState>(document, A).marks.find(0)->value, baseline.marks.find(0)->value);
    EXPECT_FALSE(stateOf<motrack::model::PointMassState>(document, A).marks.find(0)->node);

    // A track whose file saves no key frames reads every mark as a key, and keeps its unsaved reading when unchanged.
    apply(B, 2, Mark{.x = 1, .y = 1});
    EXPECT_EQ(keys(B), motrack::model::Read<std::vector<std::int32_t>>(std::vector<std::int32_t>{2}));
    apply(B, 2, std::nullopt);
    EXPECT_EQ(keys(B), motrack::model::missing<std::vector<std::int32_t>>());

    // A created point mass keys every mark, and saves none without marks.
    auto created = required(motrack::model::createTrack(document, Family::PointMass, "C"));
    ASSERT_TRUE(history.apply(document, created));
    const auto id = created.object.id;
    apply(id, 3, Mark{.x = 1, .y = 1});
    apply(id, 1, Mark{.x = 2, .y = 2});
    EXPECT_EQ(keys(id), motrack::model::Read<std::vector<std::int32_t>>(std::vector<std::int32_t>{1, 3}));
    apply(id, 1, std::nullopt);
    apply(id, 3, std::nullopt);
    EXPECT_EQ(keys(id), motrack::model::missing<std::vector<std::int32_t>>());

    // Removing a mark and undoing brings back its frame object, so export can write the original element again.
    apply(A, 1, std::nullopt);
    EXPECT_EQ(stateOf<motrack::model::PointMassState>(document, A).marks.find(1), nullptr);
    ASSERT_TRUE(history.undo(document));
    EXPECT_EQ(stateOf<motrack::model::PointMassState>(document, A).marks.find(1)->node, node);
    while (history.canUndo())
    {
        ASSERT_TRUE(history.undo(document));
    }
    const auto restored = motrack::test::documentDumpText(document);
    auto expected = original.parts();
    expected.nextObjectId = document.nextObjectId();
    EXPECT_EQ(restored, motrack::test::documentDumpText(*TrackerDocument::assemble(expected)));
    EXPECT_FALSE(motrack::model::setMark(document, ObjectId{999}, 0, Mark{}));
    EXPECT_FALSE(motrack::model::setMark(document, V, 0, Mark{}));
}

TEST(Commands, BatchesCommitOnceAndUndoTheirMembersInReverseOrder)
{
    const auto original = import(experiment(false));
    auto document = original;
    DocumentHistory history;
    const auto created = required(motrack::model::createTrack(document, Family::Vector, "w"));
    const Batch batch{
        .commands = {SetField{.object = A, .field = "mass", .before = 2.0, .after = 3.0},
                     Batch{.commands = {SetField{.object = A, .field = "name", .before = std::string("A"), .after = std::string("Z")}}},
                     created,
                     SetField{.object = created.object.id,
                              .field = "framedata",
                              .frame = 0,
                              .before = {},
                              .after = FrameElement<motrack::model::VectorSample>{
                                  .value = {.xTail = 1, .yTail = 2, .xComponent = 3, .yComponent = 4}}}}};
    const auto prepared = motrack::model::prepare(document, batch);
    ASSERT_TRUE(prepared);
    const auto& inverse = std::get<Batch>(prepared->inverse);
    ASSERT_EQ(inverse.commands.size(), 4u);
    EXPECT_TRUE(std::holds_alternative<SetField>(inverse.commands[0]));
    EXPECT_EQ(std::get<Remove>(inverse.commands[1]).object, created.object.id);
    EXPECT_TRUE(std::holds_alternative<Batch>(inverse.commands[2]));
    EXPECT_EQ(std::get<SetField>(inverse.commands[3]).field, "mass");
    EXPECT_EQ(document, original); // Preparing changes nothing.

    const auto revision = history.revision();
    ASSERT_TRUE(history.apply(document, batch));
    EXPECT_EQ(history.revision(), revision + 1);
    EXPECT_EQ(motrack::legacyProject(document).vectors.back().samples.size(), 1u);
    ASSERT_TRUE(history.undo(document));
    EXPECT_FALSE(history.canUndo());
    auto expected = original.parts();
    expected.nextObjectId = document.nextObjectId();
    EXPECT_EQ(motrack::test::documentDumpText(document), motrack::test::documentDumpText(*TrackerDocument::assemble(expected)));
    ASSERT_TRUE(history.redo(document));
    EXPECT_EQ(motrack::legacyProject(document).pointMasses.front().name, "Z");
    EXPECT_TRUE(motrack::test::graphMatchesRebuild(document));
}

TEST(Commands, InsertedDependenciesRefuseCyclesWithTheirPath)
{
    auto document = import(experiment(false));
    DocumentHistory history;
    auto member = required(motrack::model::createTrack(document, Family::PointMass, "C"));
    member.dependencies = {
        {.facet = Facet::Geometry, .dependency = motrack::model::boundDependency(DependencyKind::MemberOf, 0, A, Facet::Geometry)}};
    auto cyclic = member;
    cyclic.dependencies = {{.facet = Facet::Geometry,
                            .dependency = motrack::model::boundDependency(DependencyKind::MemberOf, 0, member.object.id, Facet::Data)}};
    const auto before = snapshot(document, history);
    const auto refused = history.apply(document, cyclic);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, CommandErrorCode::GraphRefused);
    EXPECT_NE(refused.error().message.find("cycle"), std::string::npos);
    EXPECT_EQ(snapshot(document, history), before);
    ASSERT_TRUE(history.apply(document, member));
    EXPECT_EQ(document.graph().referencesTo(A).size(), 4u);
    // Undo and redo restore the object with its dependencies.
    ASSERT_TRUE(history.undo(document));
    EXPECT_EQ(document.graph().referencesTo(A).size(), 3u);
    ASSERT_TRUE(history.redo(document));
    EXPECT_EQ(document.graph().dependencies({.object = member.object.id, .facet = Facet::Geometry}).size(), 1u);
}

TEST(Commands, LabelChangesAndCreationsRenderEveryReferenceAgain)
{
    auto document = import(experiment());
    DocumentHistory history;
    const auto status = [&](std::string_view needle)
    {
        for (const auto& line : motrack::test::renderedReferences(document))
        {
            if (line.contains(needle))
            {
                return line.substr(line.rfind("status ") + 7);
            }
        }
        return std::string("none");
    };
    const auto faithful = std::to_string(static_cast<int>(motrack::model::RenderStatus::Faithful));
    EXPECT_EQ(status("member-of 0 -> 1 "), faithful);
    ASSERT_TRUE(history.apply(document, SetField{.object = A, .field = "name", .before = std::string("A"), .after = std::string("Z")}));
    EXPECT_EQ(status("member-of 0 -> 1 "), faithful);
    EXPECT_TRUE(status("booster-of").contains(faithful));
    // A label another point mass has makes a reload ambiguous.
    ASSERT_TRUE(history.apply(document, SetField{.object = A, .field = "name", .before = std::string("Z"), .after = std::string("B")}));
    EXPECT_EQ(status("member-of 0 -> 1 "), std::to_string(static_cast<int>(motrack::model::RenderStatus::Ambiguous)));
    ASSERT_TRUE(history.undo(document));
    EXPECT_EQ(status("member-of 0 -> 1 "), faithful);
    // A created track named like an unresolved saved name would capture it on reload.
    EXPECT_EQ(status("'missing' binding"), faithful);
    ASSERT_TRUE(history.apply(document, required(motrack::model::createTrack(document, Family::PointMass, "missing"))));
    EXPECT_EQ(status("'missing' binding"), std::to_string(static_cast<int>(motrack::model::RenderStatus::Rebinds)));
    ASSERT_TRUE(history.undo(document));
    EXPECT_EQ(status("'missing' binding"), faithful);
}

/// A media-free, timed experiment with one point mass and no other time provider.
std::string timed()
{
    const auto clip = "<property name='videoclip' type='object'><object class='org.opensourcephysics.media.core.VideoClip'>" +
                      number("startframe", "0", "int") + number("stepsize", "1", "int") + number("stepcount", "10", "int") +
                      number("starttime", "0.0") + "</object></property>";
    const auto control = "<property name='clipcontrol' type='object'><object class='org.opensourcephysics.media.core.StepperClipControl'>" +
                         number("delta_t", "40.0") + "</object></property>";
    return panel(clip + control + "<property name='tracks' type='collection' class='java.util.ArrayList'>" + track("PointMass", "A") +
                 "</property>");
}

TEST(Commands, ClipCommandsKeepConstantTimingAndTheTimingProviderGuard)
{
    auto document = import(timed());
    DocumentHistory history;
    const auto clip = document.singleton(Role::Clip)->id;
    const auto control = document.singleton(Role::ClipControl)->id;
    const auto duration = [&](double value)
    {
        auto parts = document.parts();
        auto& slot = *std::ranges::find(parts.objects, control, &ObjectSlot::id);
        auto state = std::get<motrack::model::ClipControlState>(*slot.value);
        state.deltaT = value;
        slot.value = std::make_shared<const motrack::model::TypedValue>(state);
        history.update(document, *TrackerDocument::assemble(parts));
    };
    ASSERT_TRUE(history.apply(document, SetField{.object = control, .field = "delta_t", .before = 40.0, .after = 50.0}));
    EXPECT_EQ(motrack::model::clipTimeline(document).frameDurationMs, std::optional<double>(50.0));

    // A model can supply its own time, so constant timing edits are refused while one is present.
    auto parts = document.parts();
    parts.objects.push_back({.id = {},
                             .role = Role::Track,
                             .className = cls("DynamicParticle"),
                             .value = std::make_shared<const motrack::model::TypedValue>(motrack::model::UnknownState{})});
    auto guarded = *TrackerDocument::assemble(parts);
    DocumentHistory other;
    EXPECT_EQ(refusal(guarded, other, SetField{.object = control, .field = "delta_t", .before = 50.0, .after = 60.0}),
              CommandErrorCode::TimingInvariant);
    // So are media.
    parts = document.parts();
    parts.media.references = {"movie.mp4"};
    auto media = *TrackerDocument::assemble(parts);
    EXPECT_EQ(refusal(media, other, SetField{.object = control, .field = "delta_t", .before = 50.0, .after = 60.0}),
              CommandErrorCode::TimingInvariant);

    // A clip whose step times overflow, or that ends past the source frames, is refused.
    EXPECT_EQ(refusal(document, history, SetField{.object = control, .field = "delta_t", .before = 50.0, .after = 1e308}),
              CommandErrorCode::TimingInvariant);
    parts = document.parts();
    parts.media.sourceFrameCount = 5;
    history.update(document, *TrackerDocument::assemble(parts));
    EXPECT_EQ(refusal(document, history, SetField{.object = clip, .field = "stepcount", .before = 10, .after = 6}),
              CommandErrorCode::TimingInvariant);
    parts.media.sourceFrameCount.reset();
    history.update(document, *TrackerDocument::assemble(parts));

    // A legacy duration whose step times overflow: shortening the clip repairs it, and undo restores it exactly, but
    // only while the duration is still the one the command started from.
    duration(1e308);
    ASSERT_TRUE(history.apply(document, SetField{.object = clip, .field = "stepcount", .before = 10, .after = 1}));
    duration(9e307);
    EXPECT_EQ(history.undo(document).error().code, CommandErrorCode::TimingInvariant);
    duration(1e308);
    ASSERT_TRUE(history.undo(document));
    EXPECT_EQ(motrack::model::clipTimeline(document).stepCount, std::optional<std::int32_t>(10));
    // Redo always checks: the shortened clip is valid with this duration.
    ASSERT_TRUE(history.redo(document));
    EXPECT_TRUE(motrack::test::graphMatchesRebuild(document));
}

TEST(Commands, ValueAccessorsReachEveryTypedField)
{
    using namespace motrack::model;
    for (const auto family :
         {Family::Panel, Family::Clip, Family::Video, Family::ClipControl, Family::Coordinates, Family::PointMass, Family::Vector})
    {
        SCOPED_TRACE(std::string(familyName(family)));
        TypedValue value = UnknownState{};
        switch (family)
        {
        case Family::Panel:
            value = PanelState{};
            break;
        case Family::Clip:
            value = ClipState{};
            break;
        case Family::Video:
            value = VideoState{};
            break;
        case Family::ClipControl:
            value = ClipControlState{};
            break;
        case Family::Coordinates:
            value = CoordinatesState{};
            break;
        case Family::PointMass:
            value = PointMassState{};
            break;
        default:
            value = VectorState{};
            break;
        }
        for (const auto& field : fieldDescriptors(family))
        {
            SCOPED_TRACE(std::string(field.id));
            if (field.kind == FieldKind::FrameKeyed)
            {
                EXPECT_EQ(fieldValue(value, field.id, 2), std::optional<CommandValue>(CommandValue{}));
                EXPECT_FALSE(fieldValue(value, field.id));
                EXPECT_FALSE(setFieldValue(value, field.id, 2, 1.0));
                continue;
            }
            EXPECT_EQ(fieldValue(value, field.id), std::optional<CommandValue>(motrack::osp::DecodeError::Missing));
            EXPECT_FALSE(fieldValue(value, field.id, 2));
            if (field.id == "paths")
            {
                continue; // An optional array rather than an accessor result; checked below.
            }
            EXPECT_FALSE(setFieldValue(value, field.id, 2, motrack::osp::DecodeError::Malformed));
            ASSERT_TRUE(setFieldValue(value, field.id, std::nullopt, motrack::osp::DecodeError::Malformed));
            EXPECT_EQ(fieldValue(value, field.id), std::optional<CommandValue>(motrack::osp::DecodeError::Malformed));
            EXPECT_FALSE(setFieldValue(value, field.id, std::nullopt, FrameElement<Mark>{}));
        }
        EXPECT_FALSE(fieldValue(value, "nothing"));
        EXPECT_FALSE(setFieldValue(value, "nothing", std::nullopt, 1.0));
    }
    TypedValue video = VideoState{};
    ASSERT_TRUE(setFieldValue(video, "paths", std::nullopt, std::vector<std::string>{"a.png"}));
    EXPECT_EQ(fieldValue(video, "paths"), std::optional<CommandValue>(std::vector<std::string>{"a.png"}));
    ASSERT_TRUE(setFieldValue(video, "paths", std::nullopt, motrack::osp::DecodeError::Missing));
    EXPECT_FALSE(std::get<VideoState>(video).paths);
    EXPECT_FALSE(setFieldValue(video, "paths", std::nullopt, 1.0));
    EXPECT_FALSE(fieldValue(TypedValue(UnknownState{}), "name"));
    EXPECT_TRUE(sameValue(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()));
    EXPECT_FALSE(sameValue(1.0, 1));
    EXPECT_TRUE(sameValue(FrameElement<Mark>{.value = {.x = 1, .y = 2}, .node = motrack::osp::NodeId{4}},
                          FrameElement<Mark>{.value = {.x = 1, .y = 2}}));
    for (auto code = 0; code <= static_cast<int>(CommandErrorCode::NothingToUndo); ++code)
    {
        EXPECT_FALSE(commandErrorName(static_cast<CommandErrorCode>(code)).empty());
    }
}

TEST(DocumentHistory, AssignmentNeverLowersARevisionAndKeepsTheRuntimeIdentity)
{
    auto document = import(experiment());
    DocumentHistory history;
    const DocumentHistory other;
    EXPECT_NE(history.runtimeIdentity(), other.runtimeIdentity());
    for (const auto mass : {3.0, 4.0, 5.0})
    {
        const auto before = std::get<double>(required(motrack::model::fieldValue(*document.object(A)->value, "mass")));
        ASSERT_TRUE(history.apply(document, SetField{.object = A, .field = "mass", .before = before, .after = mass}));
    }
    const auto saved = std::make_shared<const TrackerDocument>(document);
    const auto computed = document.graph().revision();
    const auto revision = history.revision();
    const auto documentRevision = history.documentRevision();
    const auto identity = history.runtimeIdentity();

    // A replacement whose graph revisions are all lower than the live graph's.
    const auto incoming = import(experiment());
    ASSERT_LT(incoming.graph().revision(), computed);
    history.assign(document, incoming);
    EXPECT_GT(document.graph().revision(), computed);
    for (const auto& object : document.objects())
    {
        for (const auto facet : {Facet::Definition, Facet::Geometry, Facet::Data})
        {
            EXPECT_FALSE(document.graph().isCurrent({.object = object.id, .facet = facet}, computed));
        }
    }
    EXPECT_EQ(document, incoming);
    EXPECT_LT(incoming.graph().revision(), computed); // The incoming value's own graph is shared, not changed.
    EXPECT_FALSE(history.canUndo());
    EXPECT_FALSE(history.isModified());
    EXPECT_GT(history.revision(), revision);
    EXPECT_GT(history.documentRevision(), documentRevision);
    EXPECT_EQ(history.runtimeIdentity(), identity);

    // Revert to a value whose graph revision is higher still: the floor follows it.
    const auto live = document.graph().revision();
    history.assign(document, *saved);
    EXPECT_EQ(document.graph().revision(), Revision{std::max(saved->graph().revision().value, live.value) + 1});

    // An update outside history keeps the entries and save heads and advances only the document revision.
    ASSERT_TRUE(history.apply(document, SetField{.object = A, .field = "mass", .before = 5.0, .after = 6.0}));
    history.markSaved();
    const auto beforeUpdate = history.revision();
    const auto documentBeforeUpdate = history.documentRevision();
    const auto graphBefore = document.graph().revision();
    auto parts = document.parts();
    parts.media.sourceFrameCount = 100;
    history.update(document, *TrackerDocument::assemble(parts));
    EXPECT_EQ(history.revision(), beforeUpdate);
    EXPECT_EQ(history.documentRevision(), documentBeforeUpdate + 1);
    EXPECT_GT(document.graph().revision(), graphBefore);
    EXPECT_FALSE(history.isModified());
    EXPECT_TRUE(history.canUndo());
    ASSERT_TRUE(history.undo(document));
    EXPECT_EQ(document.media().sourceFrameCount, std::optional<std::int32_t>(100));

    TrackerDocument empty;
    DocumentHistory fresh;
    EXPECT_EQ(fresh.undo(empty).error().code, CommandErrorCode::NothingToUndo);
    EXPECT_EQ(fresh.redo(empty).error().code, CommandErrorCode::NothingToUndo);
}

TEST(DocumentHistory, UndoAndRedoCheckTheCurrentValueAndKeepTheHeadWhenRefused)
{
    auto document = import(experiment());
    DocumentHistory history;
    ASSERT_TRUE(history.apply(document, SetField{.object = A, .field = "mass", .before = 2.0, .after = 3.0}));
    auto parts = document.parts();
    auto& slot = *std::ranges::find(parts.objects, A, &ObjectSlot::id);
    auto state = std::get<motrack::model::PointMassState>(*slot.value);
    state.mass = 9.0;
    slot.value = std::make_shared<const motrack::model::TypedValue>(state);
    history.update(document, *TrackerDocument::assemble(parts));
    const auto revision = history.revision();
    const auto undone = history.undo(document);
    ASSERT_FALSE(undone);
    EXPECT_EQ(undone.error().code, CommandErrorCode::StaleValue);
    EXPECT_EQ(history.revision(), revision);
    EXPECT_TRUE(history.canUndo());
    EXPECT_FALSE(history.canRedo());
}

TEST(DocumentHistory, RestorationsSurviveAGraphRebuiltByAnUpdate)
{
    auto document = import(experiment(false));
    DocumentHistory history;
    const auto created = required(motrack::model::createTrack(document, Family::Vector, "w"));
    ASSERT_TRUE(history.apply(document, created));
    ASSERT_TRUE(history.undo(document));
    ASSERT_TRUE(history.apply(document, Remove{.object = B}));
    // An update with a graph derived from the document again, which no longer lists the created identity.
    history.update(document, *TrackerDocument::assemble(document.parts()));
    EXPECT_FALSE(document.graph().wasRemoved(created.object.id));
    ASSERT_TRUE(history.undo(document));
    EXPECT_TRUE(document.graph().contains(B));
    // The entry before is the undone creation's redo, which the new branch discarded.
    EXPECT_FALSE(history.canUndo());

    DocumentHistory second;
    ASSERT_TRUE(second.apply(document, required(motrack::model::createTrack(document, Family::Vector, "x"))));
    const auto id = document.trackOrder().back();
    ASSERT_TRUE(second.undo(document));
    second.update(document, *TrackerDocument::assemble(document.parts()));
    ASSERT_TRUE(second.redo(document));
    EXPECT_TRUE(document.graph().contains(id));
    EXPECT_TRUE(motrack::test::graphMatchesRebuild(document));
}

/// Exit criterion of B2.2: deleting and restoring members, attachment targets and boosters (and their owners) in the
/// corpus keeps every reference bound or explicitly dangling, and undo restores each one exactly.
TEST(Commands, DeletingAndRestoringReferencedObjectsInTheCorpusKeepsTheirReferences)
{
    std::set<DependencyKind> kinds;
    std::size_t targets = 0;
    std::size_t owners = 0;
    for (const auto& corpus : motrack::test::corpusDocuments())
    {
        std::optional<TrackerDocument> imported;
        try
        {
            imported = motrack::importTypedTracker(corpus.bytes);
        }
        catch (const motrack::ProjectError&)
        {
            continue; // B1 refuses the file; the typed document refuses it too (legacy view parity).
        }
        for (const auto& object : imported->objects())
        {
            const auto& graph = imported->graph();
            const auto references = graph.referencesTo(object.id);
            bool owns = false;
            for (const auto facet : {Facet::Definition, Facet::Geometry, Facet::Data})
            {
                owns = owns || !graph.dependencies({.object = object.id, .facet = facet}).empty();
            }
            if (object.role != Role::Track || (references.empty() && !owns))
            {
                continue;
            }
            SCOPED_TRACE(corpus.label + " #" + std::to_string(object.id.value));
            auto document = *imported;
            DocumentHistory history;
            const auto dump = motrack::test::documentDumpText(document);
            const auto rendered = motrack::test::renderedReferences(document);
            ASSERT_TRUE(history.apply(document, Remove{.object = object.id}));
            for (const auto& key : references)
            {
                const auto* dependency = document.graph().dependency(key);
                ASSERT_NE(dependency, nullptr);
                EXPECT_EQ(dependency->target, object.id);
                kinds.insert(key.kind);
            }
            EXPECT_FALSE(document.graph().contains(object.id));
            EXPECT_TRUE(motrack::test::graphMatchesRebuild(document));
            const auto removed = motrack::test::documentDumpText(document);
            ASSERT_TRUE(history.undo(document));
            EXPECT_EQ(motrack::test::documentDumpText(document), dump);
            EXPECT_EQ(motrack::test::renderedReferences(document), rendered);
            ASSERT_TRUE(history.redo(document));
            EXPECT_EQ(motrack::test::documentDumpText(document), removed);
            ASSERT_TRUE(history.undo(document));
            EXPECT_TRUE(motrack::test::graphMatchesRebuild(document));
            targets += references.empty() ? 0 : 1;
            owners += owns ? 1 : 0;
        }
    }
    EXPECT_TRUE(kinds.contains(DependencyKind::MemberOf));
    EXPECT_TRUE(kinds.contains(DependencyKind::AttachedTo));
    EXPECT_TRUE(kinds.contains(DependencyKind::BoosterOf));
    EXPECT_GE(targets, 5u);
    EXPECT_GE(owners, 5u);
}

} // namespace
