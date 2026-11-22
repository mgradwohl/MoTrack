#include "model/tracker_document.hpp"

#include "../osp_layout.hpp"
#include "model/codecs.hpp"
#include "model/descriptors.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using motrack::model::AllocationRange;
using motrack::model::Associations;
using motrack::model::COUNTDOWN_START;
using motrack::model::DependencyKind;
using motrack::model::DocumentParts;
using motrack::model::Facet;
using motrack::model::Family;
using motrack::model::FrameSeries;
using motrack::model::Mark;
using motrack::model::ObjectId;
using motrack::model::ObjectSlot;
using motrack::model::ObjectState;
using motrack::model::Role;
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

std::string track(std::string_view type, std::optional<std::string_view> name, std::string_view fields = {})
{
    return "<property name='item'><object class='" + cls(type) + "'>" + (name ? text("name", *name) : std::string()) + std::string(fields) +
           "</object></property>";
}

std::string marks(std::string_view frames)
{
    return "<property name='framedata' type='array' class='[L" + cls("PointMass$FrameData") + ";'>" + std::string(frames) + "</property>";
}

std::string mark(int frame, std::string_view x, std::string_view y)
{
    return "<property name='[" + std::to_string(frame) + "]' type='object'><object class='" + cls("PointMass$FrameData") + "'>" +
           number("x", x) + number("y", y) + "</object></property>";
}

std::string vectorFrame(int frame, std::string_view tail)
{
    return "<property name='[" + std::to_string(frame) + "]' type='object'><object class='" + cls("Vector$FrameData") + "'>" +
           number("xtail", tail) + number("ytail", "2.0") + number("xcomponent", "3.0") + number("ycomponent", "4.0") +
           "</object></property>";
}

std::string coordKey(int frame, std::string_view scale = "2.0")
{
    return "<property name='[" + std::to_string(frame) +
           "]' type='object'><object class='org.opensourcephysics.media.core.ImageCoordSystem$FrameData'>" + number("xorigin", "10.0") +
           number("yorigin", "20.0") + number("angle", "0.0") + number("xscale", scale) + number("yscale", scale) + "</object></property>";
}

std::string coords(std::string_view keys)
{
    return "<property name='coords' type='object'><object class='org.opensourcephysics.media.core.ImageCoordSystem'>" +
           number("fixedorigin", "true", "boolean") +
           "<property name='framedata' type='array' "
           "class='[Lorg.opensourcephysics.media.core.ImageCoordSystem$FrameData;'>" +
           std::string(keys) + "</property></object></property>";
}

std::string collection(std::string_view name, std::string_view items)
{
    return "<property name='" + std::string(name) + "' type='collection' class='java.util.ArrayList'>" + std::string(items) + "</property>";
}

std::string strings(std::string_view name, std::string_view cells)
{
    return "<property name='" + std::string(name) + "' type='array' class='[Ljava.lang.String;'>" + std::string(cells) + "</property>";
}

std::string panel(std::string_view properties)
{
    return motrack::test::ospLayout("<object class='" + cls("TrackerPanel") + "'>" + std::string(properties) + "</object>");
}

std::shared_ptr<const motrack::osp::Source> parse(const std::string& xml)
{
    auto source = motrack::osp::parse(xml);
    if (!source)
    {
        throw std::runtime_error(source.error().message);
    }
    return std::move(*source);
}

TrackerDocument import(const std::string& xml)
{
    auto document = TrackerDocument::importBaseline(parse(xml));
    if (!document)
    {
        throw std::runtime_error(document.error().message);
    }
    return std::move(*document);
}

std::string refusal(const std::string& xml)
{
    const auto document = TrackerDocument::importBaseline(parse(xml));
    return document ? std::string("accepted") : document.error().message;
}

const ObjectSlot& named(const TrackerDocument& document, std::string_view className, std::size_t occurrence = 0)
{
    for (const auto& object : document.objects())
    {
        if (object.className == className && occurrence-- == 0)
        {
            return object;
        }
    }
    throw std::runtime_error("No object of class " + std::string(className));
}

/// A media-free experiment with every typed family, Unknown tracks and every kind of reference-only slot.
std::string experiment()
{
    const auto videoclip = "<property name='videoclip' type='object'><object class='org.opensourcephysics.media.core.VideoClip'>"
                           "<property name='video' type='object'><object class='org.opensourcephysics.media.core.ImageVideo'>" +
                           strings("paths", text("[0]", "frames/a.png") + text("[1]", "frames/b.png")) + text("path", "frames/a.png") +
                           number("delta_t", "40.0") + "</object></property>" + number("video_framecount", "2", "int") +
                           number("startframe", "0", "int") + number("stepsize", "1", "int") + number("stepcount", "2", "int") +
                           number("starttime", "0.0") + "</object></property>";
    const auto clipcontrol =
        "<property name='clipcontrol' type='object'><object class='org.opensourcephysics.media.core.StepperClipControl'>" +
        number("rate", "1.0") + number("delta_t", "40.0") + "</object></property>";
    const auto tracks =
        "<property name='tracks' type='collection' class='java.util.ArrayList'>" +
        track("PointMass",
              "A",
              number("mass", "2.0") + marks(mark(0, "1.0", "2.0") + mark(1, "3.0", "4.0")) +
                  "<property name='keyFrames' type='array' class='[I'><property name='array' type='string'>{0,1}</property></property>") +
        track("FutureTrack", "F") +
        track("Vector",
              "v",
              "<property name='framedata' type='array' class='[L" + cls("Vector$FrameData") + ";'>" + vectorFrame(1, "1.0") +
                  "</property>") +
        track("CenterOfMass", "cm", collection("masses", text("item", "A") + text("item", "missing") + text("item", "F"))) +
        track("TapeMeasure",
              "tape",
              strings("attachments", text("[0]", "A") + "<property name='[1]' type='object'>null</property>" + text("[2]", "v"))) +
        track("DynamicParticle", "model", text("booster", "A")) + "<property name='item' type='object'>null</property>" +
        track("PointMass", "B") + "</property>";
    return panel(text("description", "notes") + text("mass_unit", "g") + videoclip + clipcontrol + coords(coordKey(0)) + tracks);
}

TEST(TrackerDocument, TypesEveryFamilyWithCounterIdentitiesForTypedTracksAndCountdownForTheRest)
{
    const auto document = import(experiment());
    EXPECT_EQ(document.associations(), Associations::Known);
    // Typed tracks take the counter in source order, as B1 assigns track IDs.
    EXPECT_EQ(named(document, cls("PointMass")).id, ObjectId{1});
    EXPECT_EQ(named(document, cls("Vector")).id, ObjectId{2});
    EXPECT_EQ(named(document, cls("PointMass"), 1).id, ObjectId{3});
    EXPECT_EQ(document.nextObjectId(), 4u);
    EXPECT_EQ(std::vector<ObjectId>(document.trackOrder().begin(), document.trackOrder().end()),
              (std::vector<ObjectId>{ObjectId{1}, ObjectId{3}, ObjectId{2}})); // Point masses, then vectors.
    // Everything else counts down from 2^64 - 1 in source order.
    std::vector<std::uint64_t> countdown;
    for (const auto& object : document.objects())
    {
        EXPECT_TRUE(object.binding);
        EXPECT_EQ(object.state, ObjectState::Live);
        if (object.range == AllocationRange::Countdown)
        {
            countdown.push_back(object.id.value);
        }
    }
    ASSERT_EQ(countdown.size(), 9u); // panel, clip, video, control, coords, FutureTrack, cm, tape, model
    for (std::size_t index = 0; index < countdown.size(); ++index)
    {
        EXPECT_EQ(countdown[index], COUNTDOWN_START - index);
    }
    EXPECT_EQ(document.countdownLowWater(), COUNTDOWN_START - 8);
    EXPECT_TRUE(document.hasCounterCapacity());

    EXPECT_EQ(document.singleton(Role::Panel)->family(), Family::Panel);
    EXPECT_EQ(document.singleton(Role::Clip)->family(), Family::Clip);
    EXPECT_EQ(document.singleton(Role::Video)->className, "org.opensourcephysics.media.core.ImageVideo");
    EXPECT_EQ(document.singleton(Role::ClipControl)->family(), Family::ClipControl);
    EXPECT_EQ(document.singleton(Role::Coordinates)->family(), Family::Coordinates);
    EXPECT_EQ(document.unknownTrackClasses(),
              (std::vector<std::string>{cls("FutureTrack"), cls("CenterOfMass"), cls("TapeMeasure"), cls("DynamicParticle")}));
    EXPECT_EQ(document.media().references, (std::vector<std::string>{"frames/a.png", "frames/b.png"}));
    EXPECT_FALSE(document.media().sourceFrameCount);
    EXPECT_EQ(document.object(ObjectId{12345}), nullptr);
    EXPECT_EQ(document.object(ObjectId{2})->family(), Family::Vector);

    const auto& panelState = std::get<motrack::model::PanelState>(*document.singleton(Role::Panel)->value);
    EXPECT_EQ(panelState.massUnit, std::string("g"));
    EXPECT_EQ(panelState.lengthUnit.error(), motrack::osp::DecodeError::Missing);
    const auto& mass = std::get<motrack::model::PointMassState>(*named(document, cls("PointMass")).value);
    EXPECT_EQ(mass.marks.size(), 2u);
    ASSERT_NE(mass.marks.find(1), nullptr);
    EXPECT_EQ(mass.marks.find(1)->value, (Mark{.x = 3.0, .y = 4.0}));
    EXPECT_TRUE(mass.marks.find(1)->node);
    EXPECT_EQ(mass.keyFrames, (std::vector<std::int32_t>{0, 1}));
    const auto& unknown = std::get<motrack::model::UnknownState>(*named(document, cls("FutureTrack")).value);
    EXPECT_EQ(unknown.label, std::optional<std::string>("F"));
}

TEST(TrackerDocument, PopulatesTheGraphFromReferenceOnlyDescriptorsByOriginalName)
{
    const auto document = import(experiment());
    const auto& graph = document.graph();
    EXPECT_EQ(graph.objectCount(), document.objects().size());
    const auto& center = named(document, cls("CenterOfMass"));
    const auto members = graph.dependencies({.object = center.id, .facet = Facet::Geometry});
    ASSERT_EQ(members.size(), 3u);
    EXPECT_EQ(members[0].kind, DependencyKind::MemberOf);
    EXPECT_EQ(members[0].target, ObjectId{1});
    EXPECT_EQ(members[0].savedName, "A");
    EXPECT_EQ(members[1].binding, motrack::model::BindingOutcome::Missing);
    EXPECT_EQ(members[1].savedName, "missing");
    // A same-named track of an unverified class is never chosen: Unclassified, as in the source reference index.
    EXPECT_EQ(members[2].binding, motrack::model::BindingOutcome::Unclassified);

    const auto& tape = named(document, cls("TapeMeasure"));
    const auto attachments = graph.dependencies({.object = tape.id, .facet = Facet::Geometry});
    ASSERT_EQ(attachments.size(), 2u); // The typed null slot has no dependency.
    EXPECT_EQ(attachments[0].kind, DependencyKind::AttachedTo);
    EXPECT_EQ(attachments[0].slot, 0u);
    EXPECT_EQ(attachments[0].onMissing, motrack::model::MissingTarget::FallsBack);
    EXPECT_EQ(attachments[1].slot, 2u);
    EXPECT_EQ(attachments[1].target, ObjectId{2});

    const auto& model = named(document, cls("DynamicParticle"));
    const auto booster = graph.dependencies({.object = model.id, .facet = Facet::Geometry});
    ASSERT_EQ(booster.size(), 1u);
    EXPECT_EQ(booster[0].kind, DependencyKind::BoosterOf);
    EXPECT_EQ(booster[0].targetFacet, Facet::Data);
    EXPECT_EQ(booster[0].target, ObjectId{1});
    EXPECT_EQ(graph.referencesTo(ObjectId{1}).size(), 3u);
    EXPECT_TRUE(document.referenceIssues().empty());
    // Typed tracks are labeled by their names, Unknown ones by their saved names, singletons not at all.
    EXPECT_EQ(graph.object(ObjectId{1})->label, std::optional<std::string>("A"));
    EXPECT_FALSE(graph.object(document.singleton(Role::Panel)->id)->label);
}

TEST(TrackerDocument, RecordsCyclesFromFilesAndReportsMalformedSlots)
{
    const auto document =
        import(panel("<property name='tracks' type='collection' class='java.util.ArrayList'>" +
                     track("TapeMeasure", "one", strings("attachments", text("[0]", "two"))) +
                     track("TapeMeasure", "two", strings("attachments", text("[0]", "one"))) +
                     track("CircleFitter", "circle", text("step_attachment", "one") + strings("attachments", text("[0]", "two"))) +
                     track("CenterOfMass", "no members") + track("Protractor", "bad", strings("attachments", text("[x]", "one"))) +
                     track("DynamicSystem", "system", strings("particles", text("[1]", "one")) + text("booster", "one")) + "</property>" +
                     text("referenceframe", "one")));
    const auto plan = document.graph().plan();
    EXPECT_EQ(plan.cycles.size(), 1u);
    const auto& circle = named(document, cls("CircleFitter"));
    const auto circleSlots = document.graph().dependencies({.object = circle.id, .facet = Facet::Geometry});
    ASSERT_EQ(circleSlots.size(), 2u);
    EXPECT_EQ(circleSlots[0].slot, 0u);
    EXPECT_EQ(circleSlots[1].slot, 0x8000'0000U);
    // The panel's reference frame is a FrameOf edge that falls back to the default frame.
    const auto frame = document.graph().dependencies({.object = document.singleton(Role::Panel)->id, .facet = Facet::Geometry});
    ASSERT_EQ(frame.size(), 1u);
    EXPECT_EQ(frame[0].kind, DependencyKind::FrameOf);
    EXPECT_EQ(frame[0].target, named(document, cls("TapeMeasure")).id);
    std::vector<std::string> issues;
    for (const auto& issue : document.referenceIssues())
    {
        issues.push_back(issue.field + " " + issue.reason);
    }
    EXPECT_EQ(
        issues,
        (std::vector<std::string>{"center-of-mass.masses malformed", "tool.attachments malformed", "dynamic-system.particles malformed"}));
}

TEST(TrackerDocument, PresentsWhatB1CannotReadAsUnknownObjects)
{
    const auto unreadableVector = track("Vector",
                                        "broken",
                                        "<property name='framedata' type='array' class='[L" + cls("Vector$FrameData") + ";'>" +
                                            vectorFrame(0, "NaN") + "</property>");
    for (const auto& [label, xml] : std::vector<std::pair<std::string, std::string>>{
             {"duplicate key", panel(coords(coordKey(0) + coordKey(0, "3.0")))},
             {"no keys", panel(coords(""))},
             {"unreadable key", panel(coords(coordKey(-1)))},
             {"reference frame", panel(coords(coordKey(0)) + text("referenceframe", "origin"))},
             {"other class",
              panel("<property name='coords' type='object'><object class='org.opensourcephysics.cabrillo.tracker.ReferenceFrame'>"
                    "</object></property>")},
         })
    {
        SCOPED_TRACE(label);
        const auto document = import(xml);
        ASSERT_NE(document.singleton(Role::Coordinates), nullptr);
        EXPECT_EQ(document.singleton(Role::Coordinates)->family(), Family::Unknown);
        EXPECT_EQ(document.singleton(Role::Coordinates)->range, AllocationRange::Countdown);
    }
    const auto document =
        import(panel("<property name='tracks' type='collection' class='java.util.ArrayList'>" + unreadableVector + "</property>"));
    const auto& vector = named(document, cls("Vector"));
    EXPECT_EQ(vector.family(), Family::Unknown);
    EXPECT_EQ(vector.range, AllocationRange::Countdown);
    EXPECT_EQ(document.unknownTrackClasses(), std::vector<std::string>{cls("Vector")});
    EXPECT_EQ(document.nextObjectId(), 1u);
    EXPECT_TRUE(document.trackOrder().empty());
}

TEST(TrackerDocument, RefusesWithB1sMessagesWhereB1RefusesTheFile)
{
    EXPECT_EQ(refusal(motrack::test::ospLayout("<object class='org.opensourcephysics.tools.DataToolTab'></object>")),
              "Expected a TrackerPanel project.");
    const auto clip = [](std::string_view fields)
    {
        return panel("<property name='videoclip' type='object'><object class='org.opensourcephysics.media.core.VideoClip'>" +
                     std::string(fields) + "</object></property>");
    };
    EXPECT_EQ(refusal(clip(number("startframe", "-1", "int"))), "Invalid project integer.");
    EXPECT_EQ(refusal(clip(number("stepsize", "0", "int"))), "Invalid project integer.");
    EXPECT_EQ(refusal(clip(number("stepcount", "many", "int"))), "Invalid project integer.");
    // getInt parses string properties too, but Java ignores an int saved with another type, so B1 keeps the default.
    EXPECT_EQ(refusal(clip(text("stepcount", "many"))), "Invalid project integer.");
    EXPECT_EQ(refusal(clip(number("stepcount", "true", "boolean"))), "accepted");
    const auto images = [&](std::string_view paths)
    {
        return clip("<property name='video' type='object'><object class='org.opensourcephysics.media.core.ImageVideo'>" +
                    strings("paths", paths) + "</object></property>");
    };
    EXPECT_EQ(refusal(images(text("[0]", "a.png") + text("[2]", "c.png"))), "Invalid Tracker image sequence.");
    EXPECT_EQ(refusal(images(text("[0]", ""))), "Invalid Tracker image sequence.");
    EXPECT_EQ(refusal(images(number("[0]", "1", "int"))), "Invalid Tracker image sequence.");
    EXPECT_EQ(refusal(images(text("[-1]", "a.png"))), "Invalid Tracker array entry.");
    EXPECT_EQ(refusal(images(text("[1]", "b.png") + text("[0]", "a.png"))), "accepted");
    const auto mass = [](std::string_view frames)
    {
        return panel("<property name='tracks' type='collection' class='java.util.ArrayList'>" + track("PointMass", "m", marks(frames)) +
                     "</property>");
    };
    EXPECT_EQ(refusal(mass(mark(-2, "1.0", "1.0"))), "Invalid Tracker array entry.");
    EXPECT_EQ(refusal(mass(mark(0, "Infinity", "1.0"))), "Invalid Tracker numeric property.");
    EXPECT_EQ(refusal(mass(mark(0, "1.0", "1.0") + mark(0, "2.0", "2.0"))), "Duplicate Tracker point mass frame.");
    EXPECT_EQ(refusal(mass(mark(3, "1.0", "1.0") + mark(1, "2.0", "2.0"))), "accepted");
    EXPECT_FALSE(TrackerDocument::importBaseline(nullptr));
}

/// The parts of a small imported document, for assembly tests.
DocumentParts sampleParts()
{
    return import(panel("<property name='tracks' type='collection' class='java.util.ArrayList'>" +
                        track("PointMass", "A", marks(mark(0, "1.0", "1.0"))) +
                        track("CenterOfMass", "cm", collection("masses", text("item", "A"))) + "</property>"))
        .parts();
}

std::string assembleError(DocumentParts parts)
{
    const auto document = TrackerDocument::assemble(std::move(parts));
    return document ? std::string("accepted") : document.error().message;
}

TEST(TrackerDocument, AssemblyValidatesIdentitiesRolesBindingsAndTheTrackOrder)
{
    EXPECT_EQ(assembleError(sampleParts()), "accepted");
    auto parts = sampleParts();
    parts.objects[1].id = parts.objects[0].id;
    EXPECT_EQ(assembleError(parts), "Invalid or duplicate document identity.");
    parts = sampleParts();
    parts.nextObjectId = 1;
    EXPECT_EQ(assembleError(parts), "Document identity outside its allocation range.");
    parts = sampleParts();
    parts.countdownLowWater = COUNTDOWN_START;
    EXPECT_EQ(assembleError(parts), "Document identity outside its allocation range.");
    parts = sampleParts();
    parts.nextObjectId = 0;
    EXPECT_EQ(assembleError(parts), "Invalid document identity counter.");
    parts = sampleParts();
    parts.objects[0].range = AllocationRange::Counter;
    parts.objects[0].id = ObjectId{1};
    parts.objects[1].id = ObjectId{2};
    parts.nextObjectId = 3;
    parts.trackOrder = {ObjectId{2}};
    EXPECT_EQ(assembleError(parts), "Document singletons take countdown identities.");
    parts = sampleParts();
    parts.objects.push_back(parts.objects[0]);
    parts.objects.back().id = {};
    parts.objects.back().binding.reset();
    EXPECT_EQ(assembleError(parts), "Duplicate document singleton.");
    parts = sampleParts();
    parts.objects[0].value = std::make_shared<const motrack::model::TypedValue>(motrack::model::VectorState{});
    EXPECT_EQ(assembleError(parts), "Document object does not belong to its role.");
    parts = sampleParts();
    parts.objects[0].value = std::make_shared<const motrack::model::TypedValue>(motrack::model::UnknownState{});
    EXPECT_EQ(assembleError(parts), "A document has exactly one panel.");
    parts = sampleParts();
    parts.objects[1].className = cls("Vector");
    EXPECT_EQ(assembleError(parts), "Document source binding names another object.");
    parts = sampleParts();
    parts.objects[2].binding = parts.objects[1].binding;
    EXPECT_EQ(assembleError(parts), "Invalid document source binding.");
    parts = sampleParts();
    parts.associations = Associations::Unknown;
    EXPECT_EQ(assembleError(parts), "Invalid document source binding.");
    parts = sampleParts();
    parts.associations = Associations::None;
    EXPECT_EQ(assembleError(parts), "Document associations do not match its baseline.");
    parts = sampleParts();
    parts.trackOrder.push_back(parts.trackOrder.front());
    EXPECT_EQ(assembleError(parts), "Invalid document track order.");
    parts = sampleParts();
    parts.trackOrder.clear();
    EXPECT_EQ(assembleError(parts), "Invalid document track order.");
    parts = sampleParts();
    parts.trackOrder = {parts.objects[2].id};
    EXPECT_EQ(assembleError(parts), "Invalid document track order.");
}

TEST(TrackerDocument, RemovedObjectsKeepTheirBindingAndReferencesToThemDangle)
{
    auto parts = sampleParts();
    auto& mass = *std::ranges::find(parts.objects, ObjectId{1}, &ObjectSlot::id);
    mass.state = ObjectState::Removed;
    parts.trackOrder.clear();
    const auto document = TrackerDocument::assemble(parts).value();
    EXPECT_FALSE(document.graph().contains(ObjectId{1}));
    EXPECT_TRUE(document.graph().wasRemoved(ObjectId{1}));
    EXPECT_TRUE(document.object(ObjectId{1})->binding);
    const auto plan = document.graph().plan();
    ASSERT_EQ(plan.references.size(), 1u);
    EXPECT_EQ(plan.references[0].state, motrack::model::ReferenceState::Dangling);
    EXPECT_EQ(plan.references[0].dependency.target, ObjectId{1});
    // A removed track is not an Unknown track: unsupportedTrackTypes keeps only the center of mass.
    EXPECT_EQ(document.unknownTrackClasses(), std::vector<std::string>{cls("CenterOfMass")});
}

TEST(TrackerDocument, CountdownIdentitiesSkipCounterValuesAndTheLowWaterMarkNeverRises)
{
    auto parts = TrackerDocument{}.parts();
    for (auto& object : parts.objects)
    {
        object.id = {};
    }
    parts.countdownLowWater = COUNTDOWN_START;
    // Crafted counter identities at the top of the range (a native file can store any value below its counter).
    parts.nextObjectId = COUNTDOWN_START;
    for (const auto id : {COUNTDOWN_START - 1, COUNTDOWN_START - 3})
    {
        parts.objects.push_back({.id = ObjectId{id},
                                 .range = AllocationRange::Counter,
                                 .role = Role::Track,
                                 .className = cls("PointMass"),
                                 .value = std::make_shared<const motrack::model::TypedValue>(motrack::model::PointMassState{})});
        parts.trackOrder.push_back(ObjectId{id});
    }
    const auto document = TrackerDocument::assemble(parts).value();
    std::vector<std::uint64_t> countdown;
    for (const auto& object : document.objects())
    {
        if (object.range == AllocationRange::Countdown)
        {
            countdown.push_back(object.id.value);
        }
    }
    EXPECT_EQ(countdown, (std::vector<std::uint64_t>{COUNTDOWN_START, COUNTDOWN_START - 2, COUNTDOWN_START - 4}));
    EXPECT_EQ(document.countdownLowWater(), COUNTDOWN_START - 4);
    // The counter's capacity ends below the low-water mark; opening never fails for lack of identities.
    EXPECT_FALSE(document.hasCounterCapacity());

    // An object added later continues below the low-water mark instead of reusing a value.
    auto more = document.parts();
    more.objects.push_back({.range = AllocationRange::Countdown,
                            .role = Role::Coordinates,
                            .className = "org.opensourcephysics.media.core.ImageCoordSystem",
                            .value = std::make_shared<const motrack::model::TypedValue>(motrack::model::CoordinatesState{})});
    const auto extended = TrackerDocument::assemble(more).value();
    EXPECT_EQ(extended.objects().back().id.value, COUNTDOWN_START - 5);
    EXPECT_EQ(extended.countdownLowWater(), COUNTDOWN_START - 5);
}

TEST(TrackerDocument, AnEmptyDocumentIsANativeProjectWithoutBaseline)
{
    const TrackerDocument document;
    EXPECT_EQ(document.associations(), Associations::None);
    EXPECT_FALSE(document.baseline());
    EXPECT_EQ(document.objects().size(), 3u);
    EXPECT_EQ(document.singleton(Role::Coordinates), nullptr);
    EXPECT_EQ(document.singleton(Role::Video), nullptr);
    EXPECT_EQ(document.nextObjectId(), 1u);
    EXPECT_EQ(document.countdownLowWater(), COUNTDOWN_START - 2);
    EXPECT_TRUE(document.unknownTrackClasses().empty());
    EXPECT_EQ(document, TrackerDocument{});
}

TEST(TrackerDocument, CopiesShareValuesAndEqualityComparesBaselineBytesNotTheGraph)
{
    const auto xml = experiment();
    const auto first = import(xml);
    const auto second = import(xml);
    EXPECT_NE(first.baseline(), second.baseline());
    EXPECT_EQ(first, second);
    const auto copy = first; // NOLINT(performance-unnecessary-copy-initialization): the test checks what a copy shares
    EXPECT_EQ(copy.sharedGraph(), first.sharedGraph());
    EXPECT_EQ(copy.objects()[0].value, first.objects()[0].value);
    EXPECT_NE(first, import(panel(text("description", "other"))));
    auto parts = first.parts();
    parts.media.sourceFrameCount = 2;
    EXPECT_NE(TrackerDocument::assemble(parts).value(), first);
    parts = first.parts();
    parts.objects[0].value = std::make_shared<const motrack::model::TypedValue>(motrack::model::PanelState{});
    EXPECT_NE(TrackerDocument::assemble(parts).value(), first);
}

TEST(TrackerDocument, FrameSeriesCopiesOneChunkPerEdit)
{
    FrameSeries<Mark> series;
    for (std::int32_t frame = 0; frame < 1000; ++frame)
    {
        series.set(frame, {.value = {.x = frame * 1.0, .y = 0.0}});
    }
    EXPECT_EQ(series.size(), 1000u);
    EXPECT_EQ(series.chunkCount(), 4u);
    EXPECT_EQ(series.range(), (std::pair<std::int32_t, std::int32_t>{0, 999}));
    auto edited = series;
    EXPECT_TRUE(edited.sharesChunksWith(series));
    edited.set(300, {.value = {.x = -1.0, .y = -1.0}});
    EXPECT_EQ(edited.sharedChunkCount(series), 3u);
    EXPECT_NE(edited, series);
    EXPECT_EQ(series.find(300)->value.x, 300.0);
    EXPECT_TRUE(edited.erase(300));
    EXPECT_FALSE(edited.erase(300));
    EXPECT_FALSE(edited.erase(5000));
    EXPECT_EQ(edited.find(300), nullptr);
    EXPECT_EQ(edited.size(), 999u);
    edited.set(300, {.value = {.x = 300.0, .y = 0.0}});
    EXPECT_EQ(edited, series); // Equal values in separate chunks.
    FrameSeries<Mark> sparse;
    sparse.set(7, {});
    EXPECT_TRUE(sparse.erase(7));
    EXPECT_EQ(sparse.chunkCount(), 0u);
    EXPECT_FALSE(sparse.range());
    sparse.set(-1, {});
    sparse.set(-300, {});
    EXPECT_EQ(sparse.entries().front().first, -300);
    EXPECT_EQ(sparse.chunkCount(), 2u);
}

TEST(TrackerDocument, DescriptorsNameEveryTypedFieldAndItsSaverTree)
{
    using motrack::model::fieldDescriptors;
    using motrack::model::FieldKind;
    using motrack::model::findField;
    EXPECT_TRUE(fieldDescriptors(Family::Unknown).empty());
    for (const auto family :
         {Family::Panel, Family::Clip, Family::Video, Family::ClipControl, Family::Coordinates, Family::PointMass, Family::Vector})
    {
        EXPECT_FALSE(fieldDescriptors(family).empty()) << motrack::model::familyName(family);
    }
    EXPECT_EQ(motrack::model::familyName(Family::Unknown), "unknown");
    EXPECT_EQ(motrack::model::roleName(Role::Track), "track");
    const auto* mass = findField(Family::PointMass, "mass");
    ASSERT_NE(mass, nullptr);
    EXPECT_TRUE(mass->commandEdits);
    EXPECT_EQ(mass->facet, Facet::Definition);
    EXPECT_EQ(mass->loaderDefault, "1.0");
    EXPECT_TRUE(findField(Family::Vector, "name")->label);
    EXPECT_EQ(findField(Family::Coordinates, "framedata")->kind, FieldKind::FrameKeyed);
    EXPECT_EQ(findField(Family::Panel, "length_unit")->loaderDefault, "m");
    EXPECT_FALSE(findField(Family::Clip, "video_framecount")->commandEdits);
    EXPECT_EQ(findField(Family::Panel, "missing"), nullptr);

    const auto document = import(experiment());
    const auto value = [&](Role role, std::string_view field)
    {
        return motrack::model::scalarValue(*document.singleton(role)->value, *findField(document.singleton(role)->family(), field));
    };
    EXPECT_EQ(value(Role::Panel, "description"), motrack::model::FieldValue(std::string("notes")));
    EXPECT_EQ(value(Role::Panel, "length_unit"), motrack::model::FieldValue{});
    EXPECT_EQ(value(Role::Clip, "stepcount"), motrack::model::FieldValue(2));
    EXPECT_EQ(value(Role::Clip, "starttime"), motrack::model::FieldValue(0.0));
    EXPECT_EQ(value(Role::Video, "paths"), motrack::model::FieldValue(std::vector<std::string>{"frames/a.png", "frames/b.png"}));
    EXPECT_EQ(value(Role::ClipControl, "rate"), motrack::model::FieldValue(1.0));
    EXPECT_EQ(value(Role::ClipControl, "looping"), motrack::model::FieldValue{});
    EXPECT_EQ(value(Role::Coordinates, "fixedorigin"), motrack::model::FieldValue(true));
    EXPECT_EQ(value(Role::Coordinates, "framedata"), motrack::model::FieldValue{});

    // Saver trees hold what Tracker's loaded objects hold, in its saver order.
    const auto names = [](const std::vector<motrack::osp::PropertyValue>& properties)
    {
        std::vector<std::string> result;
        result.reserve(properties.size());
        for (const auto& property : properties)
        {
            result.push_back(property.name.value_or(""));
        }
        return result;
    };
    const auto pointMass = motrack::model::saverProperties(*named(document, cls("PointMass")).value);
    EXPECT_EQ(names(pointMass), (std::vector<std::string>{"mass", "name", "framedata", "keyFrames"}));
    EXPECT_EQ(pointMass[0].content, "2.0");
    ASSERT_EQ(pointMass[2].elements.size(), 2u);
    EXPECT_EQ(pointMass[2].elements[1].name, "[1]");
    EXPECT_EQ(pointMass[2].className, "[L" + cls("PointMass$FrameData") + ";");
    const auto other = motrack::model::saverProperties(*named(document, cls("PointMass"), 1).value);
    EXPECT_EQ(names(other), (std::vector<std::string>{"mass", "name", "framedata", "keyFrames"}));
    EXPECT_EQ(other[0].content, "1.0");
    EXPECT_EQ(names(motrack::model::saverProperties(*named(document, cls("Vector")).value)),
              (std::vector<std::string>{"name", "framedata"}));
    EXPECT_EQ(names(motrack::model::saverProperties(*document.singleton(Role::Coordinates)->value)),
              (std::vector<std::string>{"fixedorigin", "framedata"}));
    EXPECT_EQ(names(motrack::model::saverProperties(*document.singleton(Role::Video)->value)),
              (std::vector<std::string>{"paths", "path", "delta_t"}));
    EXPECT_EQ(names(motrack::model::saverProperties(*document.singleton(Role::Panel)->value)),
              (std::vector<std::string>{"description", "mass_unit"}));
    EXPECT_TRUE(motrack::model::saverProperties(*named(document, cls("FutureTrack")).value).empty());
}

TEST(TrackerDocument, BaselinePathsListOrdinalsAndPropertyNames)
{
    const auto document = import(experiment());
    const auto& source = *document.baseline();
    EXPECT_TRUE(motrack::model::baselinePath(source, source.root()).empty());
    const auto binding = named(document, cls("Vector")).binding;
    if (!binding)
    {
        FAIL() << "The vector is not bound.";
    }
    const auto path = motrack::model::baselinePath(source, *binding);
    ASSERT_EQ(path.size(), 3u);
    EXPECT_EQ(path[0].name, "tracks");
    EXPECT_EQ(path[1], (motrack::model::PathStep{.ordinal = 2, .name = "item"}));
    EXPECT_EQ(path[2].ordinal, 0u);
}

TEST(TrackerDocument, CodecsKeepTheirRules)
{
    using motrack::model::isTrackerTrackName;
    using motrack::model::isTrackerUnitLabel;
    EXPECT_TRUE(isTrackerUnitLabel("m"));
    EXPECT_TRUE(isTrackerUnitLabel("\xC2\xB5s"));
    for (const auto* unit : {"", " m", "m ", "m2", "\x01", "a\xE2\x80\x8B" /* zero-width space */, "\xFF"})
    {
        EXPECT_FALSE(isTrackerUnitLabel(unit)) << unit;
    }
    EXPECT_FALSE(isTrackerUnitLabel(std::string(257, 'u')));
    EXPECT_TRUE(isTrackerUnitLabel(std::string(256, 'u')));
    EXPECT_TRUE(isTrackerTrackName("mass A"));
    EXPECT_TRUE(isTrackerTrackName("caf\xC3\xA9 2"));
    for (const auto* name : {"", " A", "null", "a,b", "a;b", "a]]>b", "\x7F", "\xC3"})
    {
        EXPECT_FALSE(isTrackerTrackName(name)) << name;
    }
    EXPECT_TRUE(isTrackerTrackName(std::string(4096, 'n')));
    EXPECT_FALSE(isTrackerTrackName(std::string(4097, 'n')));
}

TEST(TrackerDocument, TimingGuardAllowsOnlyMediaFreeExperimentsWithoutTimeProviders)
{
    using motrack::model::supportsConstantClipTimingEdits;
    const std::vector<std::string> none;
    EXPECT_TRUE(supportsConstantClipTimingEdits(none, none));
    EXPECT_TRUE(supportsConstantClipTimingEdits(none, std::vector<std::string>{cls("CoordAxes"), cls("Protractor")}));
    EXPECT_FALSE(supportsConstantClipTimingEdits(none, std::vector<std::string>{cls("DynamicParticle")}));
    EXPECT_FALSE(supportsConstantClipTimingEdits(std::vector<std::string>{"movie.mp4"}, none));
}

} // namespace
