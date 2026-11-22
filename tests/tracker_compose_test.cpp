// libarchive's Windows headers, which corpus_documents.hpp includes, would otherwise define min and max macros.
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "tracker_compose.hpp"

#include "compose_scenarios.hpp"
#include "corpus_documents.hpp"
#include "legacy_view.hpp"
#include "model/tracker_loaders.hpp"
#include "track_edits.hpp"
#include "tracker_writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using motrack::LegacyProject;
using motrack::ProjectError;
using motrack::TrackerSource;
using motrack::detail::CompositionOutcome;
using motrack::detail::CompositionRefusal;

/// What the observer saw: every production composition of this test binary, compared with the descriptor composition.
struct Tally
{
    std::atomic<std::size_t> compared{0};
    std::atomic<std::size_t> exported{0};
    std::atomic<std::size_t> refused{0};
    std::mutex mutex;
    /// Refusals for which the two compositions give different reasons (the decision is the same).
    std::set<std::string> differentReasons;
};

Tally& tally()
{
    static Tally instance;
    return instance;
}

void record(const motrack::detail::CompositionComparison& comparison)
{
    auto& counts = tally();
    ++counts.compared;
    ++(std::holds_alternative<std::string>(comparison.legacy) ? counts.exported : counts.refused);
    if (!comparison.equivalent())
    {
        ADD_FAILURE() << "The descriptor composition differs from B1's: " << comparison.describe();
    }
    else if (!comparison.identical())
    {
        const std::scoped_lock lock(counts.mutex);
        counts.differentReasons.insert(comparison.describe());
    }
}

/// B2.3's exit criterion on every writer test: each production (B1) composition is composed again with the descriptors
/// and must make the same decision, with the same bytes where both export.
void compareWithDescriptors(const LegacyProject& project,
                            const TrackerSource& source,
                            const motrack::ExportMedia& media,
                            const CompositionOutcome& outcome)
{
    record(motrack::detail::compareWithDescriptorComposition(project, source, media, outcome));
}

class CompositionComparison : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        motrack::detail::setCompositionObserver(&compareWithDescriptors);
    }

    void TearDown() override
    {
        motrack::detail::setCompositionObserver(nullptr);
        auto& counts = tally();
        std::cout << "B2.3 comparison with B1: " << counts.compared << " compositions, " << counts.exported << " exported, "
                  << counts.refused << " refused, " << counts.differentReasons.size() << " refusals with another reason\n";
        for (const auto& difference : counts.differentReasons)
        {
            std::cout << "  " << difference << '\n';
        }
    }
};

// GoogleTest owns registered environments.
[[maybe_unused]] const auto* const COMPARISON = ::testing::AddGlobalTestEnvironment(new CompositionComparison); // NOLINT

std::string readFixture(std::string_view name)
{
    return motrack::test::readCorpusFile(motrack::utf8Path(MOTRACK_FIXTURES) / std::string(name));
}

/// The outcome of B1's composition, without the observer.
CompositionOutcome legacyOutcome(const LegacyProject& project, const TrackerSource& source, const motrack::ExportMedia& media)
{
    return motrack::test::legacyComposition(project, source, media);
}

/// The descriptor composition of a typed document, or its refusal.
CompositionOutcome composed(const motrack::model::TrackerDocument& document, const motrack::ExportMedia& media = {})
{
    try
    {
        return motrack::composeTrackerExport(document, media);
    }
    catch (const ProjectError& error)
    {
        return CompositionRefusal{.message = error.what()};
    }
}

std::string refusal(const CompositionOutcome& outcome)
{
    const auto* refused = std::get_if<CompositionRefusal>(&outcome);
    return refused ? refused->message : std::string{};
}

TEST(TrackerCompose, EveryCorpusFileAndFixtureComposesAsB1ForEveryEdit)
{
    std::size_t documents = 0;
    std::size_t edited = 0;
    std::size_t exported = 0;
    std::size_t refused = 0;
    for (const auto& document : motrack::test::corpusDocuments())
    {
        SCOPED_TRACE(document.label);
        std::optional<motrack::ImportedTrackerProject> imported;
        try
        {
            imported = motrack::parseTrackerProject(document.bytes);
        }
        catch (const ProjectError&)
        {
            continue; // B1 refuses to import it; the legacy view parity test covers the refusal.
        }
        ++documents;
        for (const auto& [scenario, route, comparison] :
             motrack::test::compareScenarios(*imported, motrack::importTypedTracker(document.bytes)))
        {
            SCOPED_TRACE(scenario);
            record(comparison);
            ++edited;
            ++(std::holds_alternative<std::string>(comparison.legacy) ? exported : refused);
        }
    }
    // The comparison covers the corpus, not a vacuous subset: experiments, edits and both decisions.
    EXPECT_GT(documents, 50u) << documents;
    EXPECT_GT(edited, 500u) << edited;
    EXPECT_GT(exported, 200u) << exported;
    EXPECT_GT(refused, 200u) << refused;
    RecordProperty("documents", static_cast<int>(documents));
    RecordProperty("exported", static_cast<int>(exported));
    RecordProperty("refused", static_cast<int>(refused));
}

TEST(TrackerCompose, AnUneditedDocumentExportsItsBaselineBytes)
{
    const auto xml = readFixture("native/baseline.trk");
    EXPECT_EQ(composed(motrack::importTypedTracker(xml)), CompositionOutcome(xml));
    // A linked document exports through its media route; the media-free one refuses it, as B1 does.
    const auto linked = readFixture("point-mass.trk");
    const auto document = motrack::importTypedTracker(linked);
    EXPECT_EQ(refusal(composed(document)), "This Tracker export cannot preserve all edits or source associations.");
    const auto imported = motrack::parseTrackerProject(linked);
    EXPECT_EQ(legacyOutcome(imported.project, imported.source, {}), composed(document));
}

TEST(TrackerCompose, CreatedTracksUseTheirFamilysSaver)
{
    const auto xml = readFixture("native/baseline.trk");
    auto imported = motrack::parseTrackerProject(xml);
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.removeVector(imported.project, 0));
    ASSERT_TRUE(edits.createPointMass(imported.project, "created"));
    ASSERT_TRUE(edits.markPointMass(imported.project, 2, {.frame = 2, .x = 5.0, .y = 6.5}));
    ASSERT_TRUE(edits.createPointMass(imported.project, "empty"));
    const auto document = motrack::typedDocumentFromReduced(imported.project, imported.source);
    const auto output = composed(document);
    ASSERT_TRUE(std::holds_alternative<std::string>(output)) << refusal(output);
    const auto& text = std::get<std::string>(output);
    // PointMass's saver: mass, then TTrack's name and appearance, then framedata and keyFrames (O7); none without marks.
    // OSP's writer gives an object its property's indentation.
    EXPECT_NE(text.find("        <property name=\"item\" type=\"object\">\n"
                        "        <object class=\"org.opensourcephysics.cabrillo.tracker.PointMass\">\n"
                        "            <property name=\"mass\" type=\"double\">1.0</property>\n"
                        "            <property name=\"name\" type=\"string\">created</property>\n"
                        "            <property name=\"color\" type=\"object\">\n"
                        "            <object class=\"java.awt.Color\">\n"
                        "                <property name=\"red\" type=\"int\">255</property>\n"
                        "                <property name=\"green\" type=\"int\">0</property>\n"
                        "                <property name=\"blue\" type=\"int\">0</property>\n"
                        "                <property name=\"alpha\" type=\"int\">255</property>\n"
                        "            </object>\n"
                        "            </property>\n"
                        "            <property name=\"footprint\" type=\"string\">Footprint.Diamond</property>\n"
                        "            <property name=\"visible\" type=\"boolean\">true</property>\n"
                        "            <property name=\"trail\" type=\"boolean\">true</property>\n"
                        "            <property name=\"framedata\" type=\"array\" "
                        "class=\"[Lorg.opensourcephysics.cabrillo.tracker.PointMass$FrameData;\">\n"
                        "                <property name=\"[2]\" type=\"object\">\n"
                        "                <object class=\"org.opensourcephysics.cabrillo.tracker.PointMass$FrameData\">\n"
                        "                    <property name=\"x\" type=\"double\">5.0</property>\n"
                        "                    <property name=\"y\" type=\"double\">6.5</property>\n"
                        "                </object>\n"
                        "                </property>\n"
                        "            </property>\n"
                        "            <property name=\"keyFrames\" type=\"array\" class=\"[I\">\n"
                        "                <property name=\"array\" type=\"string\">{2}</property>\n"
                        "            </property>\n"
                        "        </object>\n"
                        "        </property>\n"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("<property name=\"name\" type=\"string\">empty</property>\n"
                        "            <property name=\"color\" type=\"object\">"),
              std::string::npos);
    EXPECT_EQ(text.find("vector v"), std::string::npos);
    // B1 composes the same bytes.
    EXPECT_EQ(legacyOutcome(imported.project, imported.source, {}), output);
}

TEST(TrackerCompose, TracksExportInB1sOrder)
{
    auto imported = motrack::parseTrackerProject(readFixture("native/baseline.trk"));
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.removeVector(imported.project, 0));
    ASSERT_TRUE(edits.createPointMass(imported.project, "first created"));
    ASSERT_TRUE(edits.createPointMass(imported.project, "second created"));
    const auto document = motrack::typedDocumentFromReduced(imported.project, imported.source);
    // Bound objects in baseline order (the Unknown axes included, the removed vector not), then created tracks by
    // identity; the track list's presentation order is separate.
    std::vector<std::string> names;
    for (const auto id : motrack::exportOrder(document))
    {
        const auto* object = document.object(id);
        ASSERT_NE(object, nullptr);
        if (const auto* mass = std::get_if<motrack::model::PointMassState>(object->value.get()))
            names.push_back(motrack::model::loadedName(mass->name));
        else
            names.push_back(std::get<motrack::model::UnknownState>(*object->value).label.value_or("?"));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"mass A", "axes", "mass B", "first created", "second created"}));
    const auto output = composed(document);
    ASSERT_TRUE(std::holds_alternative<std::string>(output)) << refusal(output);
    EXPECT_EQ(legacyOutcome(imported.project, imported.source, {}), output);
    const auto reimported = motrack::importTypedTracker(std::get<std::string>(output));
    std::vector<std::string> exported;
    for (const auto& object : reimported.objects())
    {
        if (object.role == motrack::model::Role::Track)
            exported.push_back(object.family() == motrack::model::Family::PointMass
                                   ? motrack::model::loadedName(std::get<motrack::model::PointMassState>(*object.value).name)
                                   : std::get<motrack::model::UnknownState>(*object.value).label.value_or("?"));
    }
    EXPECT_EQ(exported, names);
}

TEST(TrackerCompose, RefusesDocumentsWithoutKnownAssociationsOrBaselines)
{
    auto imported = motrack::parseTrackerProject(readFixture("native/baseline.trk"));
    auto unbound = imported.source;
    unbound.bindings.reset();
    EXPECT_EQ(refusal(composed(motrack::typedDocumentFromReduced(imported.project, unbound))),
              "Tracker export requires original object associations.");
    EXPECT_EQ(refusal(composed(motrack::model::TrackerDocument{})), "Tracker export requires its original source.");
}

TEST(TrackerCompose, RefusesDifferencesNoDescriptorComposes)
{
    auto imported = motrack::parseTrackerProject(readFixture("native/baseline.trk"));
    // The description is typed but no command edits it, and B1 refuses it too.
    imported.project.description = "changed";
    const auto document = motrack::typedDocumentFromReduced(imported.project, imported.source);
    EXPECT_EQ(refusal(composed(document)), "This Tracker export cannot preserve all edits or source associations.");
    EXPECT_EQ(refusal(legacyOutcome(imported.project, imported.source, {})), refusal(composed(document)));
}

/// The native baseline with an Unknown center of mass whose members are saved by name.
std::string centerOfMass(std::string_view members)
{
    auto xml = readFixture("native/baseline.trk");
    const std::string anchor = "    </property>\n    <property name=\"selectedtrack\"";
    xml.insert(xml.find(anchor),
               "        <property name=\"item\" type=\"object\">\n"
               "            <object class=\"org.opensourcephysics.cabrillo.tracker.CenterOfMass\">\n"
               "                <property name=\"name\" type=\"string\">center</property>\n"
               "                <property name=\"masses\" type=\"collection\" class=\"java.util.ArrayList\">\n" +
                   std::string(members) +
                   "                </property>\n"
                   "            </object>\n"
                   "        </property>\n");
    return xml;
}

TEST(TrackerCompose, EveryReferenceMustRenderFaithfullyAfterAMembershipOrLabelChange)
{
    const auto member = [](std::string_view name)
    {
        return R"(                    <property name="item" type="string">)" + std::string(name) + "</property>\n";
    };
    // An unresolved saved name that a created track would capture rebinds on reload.
    auto imported = motrack::parseTrackerProject(centerOfMass(member("C")));
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createPointMass(imported.project, "C"));
    auto document = motrack::typedDocumentFromReduced(imported.project, imported.source);
    try
    {
        motrack::detail::requireFaithfulReferences(document);
        ADD_FAILURE() << "A captured reference was accepted.";
    }
    catch (const ProjectError& error)
    {
        EXPECT_EQ(std::string(error.what()),
                  "Tracker export cannot keep the reference to 'C' in center-of-mass.masses of 'center': it would bind another track.");
    }
    // B1's unique-name schema guard refuses the same export, so the two compositions agree.
    EXPECT_FALSE(std::holds_alternative<std::string>(composed(document)));
    EXPECT_FALSE(std::holds_alternative<std::string>(legacyOutcome(imported.project, imported.source, {})));

    // A removed member leaves its reference dangling.
    imported = motrack::parseTrackerProject(centerOfMass(member("mass A") + member("mass B")));
    motrack::TrackEdits removal;
    ASSERT_TRUE(removal.removePointMass(imported.project, 1));
    document = motrack::typedDocumentFromReduced(imported.project, imported.source);
    EXPECT_THROW(motrack::detail::requireFaithfulReferences(document), ProjectError);

    // A renamed member renders its new name, which binds it again.
    imported = motrack::parseTrackerProject(centerOfMass(member("mass A") + member("mass B")));
    motrack::TrackEdits rename;
    ASSERT_TRUE(rename.renamePointMass(imported.project, 0, "mass A2"));
    document = motrack::typedDocumentFromReduced(imported.project, imported.source);
    EXPECT_NO_THROW(motrack::detail::requireFaithfulReferences(document));
    const auto output = composed(document);
    ASSERT_TRUE(std::holds_alternative<std::string>(output)) << refusal(output);
    EXPECT_NE(std::get<std::string>(output).find("<property name=\"item\" type=\"string\">mass A2</property>"), std::string::npos);
    EXPECT_EQ(legacyOutcome(imported.project, imported.source, {}), output);
}

TEST(TrackerCompose, UneditedUnresolvedReferencesAcrossBothIdentityRangesRenderFaithfully)
{
    // #350: an Unknown track (a countdown identity) before a point mass (counter identity 1) of the same name. The center
    // of mass's member is Unclassified and the tape's attachment Ambiguous, each with both as candidates in source order,
    // which the current index lists the other way round.
    const auto document =
        motrack::importTypedTracker("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                    "<object class=\"org.opensourcephysics.cabrillo.tracker.TrackerPanel\">\n"
                                    "    <property name=\"tracks\" type=\"collection\" class=\"java.util.ArrayList\">\n"
                                    "        <property name=\"item\" type=\"object\">\n"
                                    "            <object class=\"org.opensourcephysics.cabrillo.tracker.FutureTrack\">\n"
                                    "                <property name=\"name\" type=\"string\">dup</property>\n"
                                    "            </object>\n"
                                    "        </property>\n"
                                    "        <property name=\"item\" type=\"object\">\n"
                                    "            <object class=\"org.opensourcephysics.cabrillo.tracker.PointMass\">\n"
                                    "                <property name=\"name\" type=\"string\">dup</property>\n"
                                    "            </object>\n"
                                    "        </property>\n"
                                    "        <property name=\"item\" type=\"object\">\n"
                                    "            <object class=\"org.opensourcephysics.cabrillo.tracker.CenterOfMass\">\n"
                                    "                <property name=\"name\" type=\"string\">center</property>\n"
                                    "                <property name=\"masses\" type=\"collection\" class=\"java.util.ArrayList\">\n"
                                    "                    <property name=\"item\" type=\"string\">dup</property>\n"
                                    "                </property>\n"
                                    "            </object>\n"
                                    "        </property>\n"
                                    "        <property name=\"item\" type=\"object\">\n"
                                    "            <object class=\"org.opensourcephysics.cabrillo.tracker.TapeMeasure\">\n"
                                    "                <property name=\"name\" type=\"string\">tape</property>\n"
                                    "                <property name=\"attachments\" type=\"array\" class=\"[Ljava.lang.String;\">\n"
                                    "                    <property name=\"[0]\" type=\"string\">dup</property>\n"
                                    "                </property>\n"
                                    "            </object>\n"
                                    "        </property>\n"
                                    "    </property>\n"
                                    "</object>\n");
    const auto& graph = document.graph();
    std::vector<motrack::model::BindingOutcome> outcomes;
    for (const auto owner : graph.objects())
    {
        for (const auto facet : {motrack::model::Facet::Definition, motrack::model::Facet::Geometry, motrack::model::Facet::Data})
        {
            for (const auto& dependency : graph.dependencies({.object = owner, .facet = facet}))
            {
                outcomes.push_back(dependency.binding);
                ASSERT_EQ(dependency.candidates.size(), 2u);
                EXPECT_GT(dependency.candidates.front(), dependency.candidates.back()) << "Source order, not identity order.";
            }
        }
    }
    std::ranges::sort(outcomes);
    EXPECT_EQ(outcomes, (std::vector{motrack::model::BindingOutcome::Ambiguous, motrack::model::BindingOutcome::Unclassified}));
    EXPECT_NO_THROW(motrack::detail::requireFaithfulReferences(document));
}

TEST(TrackerCompose, ComparisonsDescribeEachDifference)
{
    using motrack::detail::CompositionComparison;
    const CompositionComparison same{.legacy = std::string("abc"), .descriptor = std::string("abc")};
    EXPECT_TRUE(same.identical());
    EXPECT_EQ(same.describe(), "both export the same 3 bytes");
    const CompositionComparison bytes{.legacy = std::string("abc"), .descriptor = std::string("abd")};
    EXPECT_TRUE(bytes.sameDecision());
    EXPECT_FALSE(bytes.equivalent());
    EXPECT_EQ(bytes.describe(), "the exports differ from byte 2 (B1 3 bytes, descriptors 3)");
    const CompositionComparison refused{.legacy = std::string("abc"), .descriptor = CompositionRefusal{.message = "no"}};
    EXPECT_FALSE(refused.sameDecision());
    EXPECT_EQ(refused.describe(), "B1 exports, the descriptors refuse: no");
    const CompositionComparison accepted{.legacy = CompositionRefusal{.message = "no"}, .descriptor = std::string("abc")};
    EXPECT_EQ(accepted.describe(), "B1 refuses (no), the descriptors export");
    const CompositionComparison reasons{.legacy = CompositionRefusal{.message = "a"}, .descriptor = CompositionRefusal{.message = "b"}};
    EXPECT_TRUE(reasons.equivalent());
    EXPECT_FALSE(reasons.identical());
    EXPECT_EQ(reasons.describe(), "both refuse, B1: a Descriptors: b");
    const CompositionComparison agreed{.legacy = CompositionRefusal{.message = "a"}, .descriptor = CompositionRefusal{.message = "a"}};
    EXPECT_EQ(agreed.describe(), "both refuse: a");
}

} // namespace
