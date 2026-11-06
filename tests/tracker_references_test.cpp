#include "tracker_references.hpp"

#include "track_edits.hpp"
#include "workspace_document.hpp"

#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <algorithm>

namespace
{

std::string property(std::string_view name, std::string_view text)
{
    return "<property name='" + std::string(name) + "' type='string'><![CDATA[" + std::string(text) + "]]></property>";
}

std::string track(std::string_view type, std::string_view name)
{
    return "<property name='item'><object class='" + std::string(type) + "'>" + property("name", name) + "</object></property>";
}

std::string point(std::string_view name)
{
    return track("org.opensourcephysics.cabrillo.tracker.PointMass", name);
}

std::string project(std::string_view tracks, std::string_view fields = {})
{
    return "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'><property name='tracks'>" + std::string(tracks) +
           "</property>" + std::string(fields) + "</object>";
}

motrack::TrackerReferenceIndex index(const std::string& xml)
{
    const auto imported = motrack::parseTrackerProject(xml);
    return motrack::indexTrackerReferences(imported.source, imported.project);
}

using Kind = motrack::TrackerReferenceKind;
using Resolution = motrack::TrackerReferenceResolution;

TEST(TrackerReferences, ResolvesSourceOrdinalsIncludingUnsupportedTracksAndKeepsBytes)
{
    const auto xml =
        project(point("mass A") + track("UnknownTrack", "unknown") + point("mass B"),
                property("description", "mass B") + property("selectedtrack", "mass B") + property("referenceframe", "unknown"));
    const auto imported = motrack::parseTrackerProject(xml);
    const auto result = motrack::indexTrackerReferences(imported.source, imported.project);
    EXPECT_EQ(imported.source.xml, xml);
    EXPECT_TRUE(result.malformedPaths.empty());
    ASSERT_EQ(result.tracks.size(), 3u);
    EXPECT_EQ(result.tracks[1].sourceIndex, 1u);
    EXPECT_EQ(result.tracks[1].className, "UnknownTrack");
    EXPECT_EQ(result.tracks[1].originalName, "unknown");
    EXPECT_FALSE(result.tracks[1].nativeId);
    EXPECT_EQ(result.tracks[2].nativeId, imported.project.pointMasses[1].id);
    ASSERT_EQ(result.references.size(), 2u); // The identical description is not a reference.
    EXPECT_EQ(result.references[0].kind, Kind::SelectedTrack);
    EXPECT_EQ(result.references[0].path, "/selectedtrack");
    EXPECT_EQ(result.references[0].originalName, "mass B");
    EXPECT_EQ(result.references[0].resolution, Resolution::Resolved);
    EXPECT_EQ(result.references[0].candidateSourceIndices, (std::vector<std::size_t>{2}));
    EXPECT_EQ(result.references[1].kind, Kind::ReferenceFrame);
    EXPECT_EQ(result.references[1].candidateSourceIndices, (std::vector<std::size_t>{1}));
}

TEST(TrackerReferences, ReportsMissingAndAmbiguousTargetsAcrossTrackTypes)
{
    const auto result =
        index(project(point("same") + track("FutureTrack", "same") + track("org.opensourcephysics.cabrillo.tracker.Vector", "same"),
                      property("selectedtrack", "same") + property("referenceframe", "absent")));
    ASSERT_EQ(result.references.size(), 2u);
    EXPECT_EQ(result.references[0].resolution, Resolution::Ambiguous);
    EXPECT_EQ(result.references[0].candidateSourceIndices, (std::vector<std::size_t>{0, 1, 2}));
    EXPECT_EQ(result.references[1].resolution, Resolution::Missing);
    EXPECT_TRUE(result.references[1].candidateSourceIndices.empty());
}

TEST(TrackerReferences, DecodesFourPlotTablePairsWithoutTreatingDirectNullNamesAsEmpty)
{
    const auto result = index(
        project(point("mass B") + point("null") + point("élève"),
                property("selectedtrack", "null") + property("selected_track_views", "mass B,null;null,mass B;élève,missing;null,null")));
    EXPECT_TRUE(result.malformedPaths.empty());
    ASSERT_EQ(result.references.size(), 9u);
    EXPECT_EQ(result.references[0].resolution, Resolution::Resolved);
    EXPECT_EQ(result.references[0].candidateSourceIndices, (std::vector<std::size_t>{1}));
    EXPECT_EQ(result.references[1].kind, Kind::PlotSelection);
    EXPECT_EQ(result.references[1].path, "/selected_track_views/0/plot");
    EXPECT_EQ(result.references[1].candidateSourceIndices, (std::vector<std::size_t>{0}));
    EXPECT_EQ(result.references[2].resolution, Resolution::NoTarget);
    EXPECT_TRUE(result.references[2].candidateSourceIndices.empty());
    EXPECT_EQ(result.references[4].kind, Kind::TableSelection);
    EXPECT_EQ(result.references[4].path, "/selected_track_views/1/table");
    EXPECT_EQ(result.references[4].candidateSourceIndices, (std::vector<std::size_t>{0}));
    EXPECT_EQ(result.references[5].candidateSourceIndices, (std::vector<std::size_t>{2}));
    EXPECT_EQ(result.references[6].resolution, Resolution::Missing);
    EXPECT_EQ(result.references[8].resolution, Resolution::NoTarget);
}

TEST(TrackerReferences, OriginalBindingsSurviveRenameReorderDeletionUndoAndNativeReopen)
{
    auto imported = motrack::parseTrackerProject(project(point("original") + point("other"), property("selectedtrack", "original")));
    const auto originalId = imported.project.pointMasses.front().id;
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.renamePointMass(document.project, 0, "renamed"));
    std::ranges::reverse(document.project.pointMasses);
    ASSERT_TRUE(edits.removePointMass(document.project, 1));
    const auto removed = motrack::indexTrackerReferences(imported.source, document.project);
    ASSERT_EQ(removed.tracks.size(), 2u);
    EXPECT_EQ(removed.tracks[0].nativeId, originalId);
    EXPECT_EQ(removed.tracks[0].originalName, "original");
    EXPECT_EQ(removed.references[0].candidateSourceIndices, (std::vector<std::size_t>{0}));
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    QTemporaryDir temporary;
    const auto path = temporary.filePath("references.motrack");
    motrack::writeWorkspaceDocument(path, document);
    const auto reopened = motrack::readWorkspaceDocument(path);
    if (!reopened.trackerSource)
    {
        FAIL() << "Expected preserved source";
    }
    const auto restored = motrack::indexTrackerReferences(*reopened.trackerSource, reopened.project);
    EXPECT_EQ(restored.tracks[0].nativeId, originalId);
    EXPECT_EQ(restored.references[0].candidateSourceIndices, (std::vector<std::size_t>{0}));
    auto unbound = *reopened.trackerSource;
    unbound.bindings.reset();
    const auto oldNative = motrack::indexTrackerReferences(unbound, reopened.project);
    EXPECT_FALSE(oldNative.tracks[0].nativeId);
    EXPECT_EQ(oldNative.references[0].resolution, Resolution::Resolved);
    EXPECT_EQ(oldNative.references[0].candidateSourceIndices, (std::vector<std::size_t>{0}));
}

TEST(TrackerReferences, RejectsInvalidBindingsAndXmlBeforeIndexing)
{
    auto imported = motrack::parseTrackerProject(project(point("A")));
    if (!imported.source.bindings)
    {
        FAIL() << "Expected known binding";
    }
    imported.source.bindings->front().sourceIndex = 1;
    EXPECT_THROW((void)motrack::indexTrackerReferences(imported.source, imported.project), motrack::ProjectError);
    imported.source.xml = "invalid XML";
    EXPECT_THROW((void)motrack::indexTrackerReferences(imported.source, imported.project), motrack::ProjectError);
}

TEST(TrackerReferences, ReportsMalformedNamesObjectsAndDuplicateContainers)
{
    const std::vector<std::string> entries{
        "<property name='item'>null</property>",
        "<property name='item'><object/></property>",
        "<property name='item'><other/><object class='Future'/></property>",
        "<property name='item'><object class='Future'/><object class='Future'/></property>",
        "<property name='item' type='string'><object class='Future'>" + property("name", "A") + "</object></property>",
        "<property name='item'>garbage<object class='Future'>" + property("name", "A") + "</object></property>",
        "<property name='item'><object class='Future'/></property>",
        "<property name='item'><object class='Future'>" + property("name", "A") + property("name", "B") + "</object></property>",
        "<property name='item'><object class='Future'><property name='name' type='string'><object/></property></object></property>"};
    for (const auto& entry : entries)
    {
        const auto result = index(project(entry, property("selectedtrack", "A")));
        ASSERT_EQ(result.tracks.size(), 1u);
        EXPECT_FALSE(result.tracks[0].originalName);
        EXPECT_EQ(result.malformedPaths.size(), 1u);
        EXPECT_EQ(result.references[0].resolution, Resolution::Missing);
    }
    const auto duplicate = index(project(point("A"), "<property name='tracks'/>" + property("selectedtrack", "A")));
    EXPECT_TRUE(duplicate.tracks.empty());
    EXPECT_EQ(duplicate.malformedPaths, (std::vector<std::string>{"/tracks"}));
    EXPECT_EQ(duplicate.references[0].resolution, Resolution::Missing);
}

TEST(TrackerReferences, DistinguishesMalformedContainersFromAbsentOrEmptyCollections)
{
    for (const auto& container : {"<property name='tracks' type='string'>garbage</property>",
                                  "<property name='tracks' type='string'/>",
                                  "<property name='tracks'><object class='Future'/></property>",
                                  "<property name='tracks'><unexpected/></property>",
                                  "<property name='tracks'><![CDATA[garbage]]></property>",
                                  "<property name='tracks'>\xE2\x80\x83</property>"})
    {
        const auto result =
            index("<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>" + std::string(container) + "</object>");
        EXPECT_TRUE(result.tracks.empty());
        EXPECT_EQ(result.malformedPaths, (std::vector<std::string>{"/tracks"}));
    }
    const auto mixed = index(project("garbage" + point("A"), property("selectedtrack", "A")));
    EXPECT_TRUE(mixed.tracks.empty());
    EXPECT_EQ(mixed.malformedPaths, (std::vector<std::string>{"/tracks"}));
    EXPECT_EQ(mixed.references[0].resolution, Resolution::Missing);
    const auto valid = index(project(" \t\r\n<!-- retained --><?note retained?><![CDATA[\n]]>" + point("A")));
    EXPECT_TRUE(valid.malformedPaths.empty());
    EXPECT_EQ(valid.tracks.size(), 1u);
    const auto empty = index("<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>"
                             "<property name='tracks' type='collection' class='java.util.ArrayList'> \n</property></object>");
    EXPECT_TRUE(empty.tracks.empty());
    EXPECT_TRUE(empty.malformedPaths.empty());
}

TEST(TrackerReferences, RejectsMalformedPackedStringsWithoutPartialReferenceResults)
{
    const std::vector<std::string> values{"",
                                          "A,null",
                                          "A,null;null,A;null,null;null,null;null,null",
                                          "A,B,C;null,A;null,null;null,null",
                                          "A,null;null,A;null,null;null,",
                                          "A,null;null,A;null,null;,null",
                                          "A,null;null,A;null,null;" + std::string(4097, 'x') + ",null",
                                          std::string(65537, 'x')};
    for (const auto& value : values)
    {
        const auto result = index(project(point("A"), property("selected_track_views", value)));
        EXPECT_TRUE(result.references.empty());
        EXPECT_EQ(result.malformedPaths, (std::vector<std::string>{"/selected_track_views"}));
    }
    for (const auto& field : {"selectedtrack", "referenceframe", "selected_track_views"})
    {
        for (const auto& invalid : {property(field, "A") + property(field, "B"),
                                    "<property name='" + std::string(field) + "' type='int'>1</property>",
                                    "<property name='" + std::string(field) + "' type='string'>A<!-- unknown --></property>"})
        {
            const auto result = index(project(point("A"), invalid));
            EXPECT_TRUE(result.references.empty());
            EXPECT_EQ(result.malformedPaths, (std::vector<std::string>{"/" + std::string(field)}));
        }
    }
}

TEST(TrackerReferences, HandlesAbsentFieldsExactNamesAndBoundedTrackInventories)
{
    const auto empty = index("<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'/>");
    EXPECT_TRUE(empty.tracks.empty());
    EXPECT_TRUE(empty.references.empty());
    EXPECT_TRUE(empty.malformedPaths.empty());
    const std::string longest(4096, 'x');
    const auto result =
        index(project(point(longest) + point(" A ") + point(""), property("selectedtrack", longest) + property("referenceframe", "")));
    EXPECT_TRUE(result.malformedPaths.empty());
    EXPECT_EQ(result.references[0].resolution, Resolution::Resolved);
    EXPECT_EQ(result.references[1].candidateSourceIndices, (std::vector<std::size_t>{2}));
    EXPECT_EQ(result.tracks[1].originalName, " A ");
    const auto tooLong = index(project(point(longest + "x"), property("selectedtrack", longest + "x")));
    EXPECT_EQ(tooLong.malformedPaths, (std::vector<std::string>{"/tracks/0/name", "/selectedtrack"}));
    std::string many;
    for (int number = 0; number < 10000; ++number)
    {
        many += track("Future", "name");
    }
    EXPECT_EQ(index(project(many)).tracks.size(), 10000u);
    many += track("Future", "name");
    EXPECT_THROW((void)index(project(many)), motrack::ProjectError);
}

} // namespace
