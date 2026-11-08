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

std::string arrayXml(std::string_view name, std::string_view type, std::string_view children)
{
    return "<property name='" + std::string(name) + "' type='array' class='" + std::string(type) + "'>" + std::string(children) +
           "</property>";
}

std::string viewXml(std::string_view name, std::string_view fields, std::string_view slot = "[1]")
{
    return "<property name='" + std::string(slot) + "' type='object'><object class='org.opensourcephysics.cabrillo.tracker." +
           std::string(name) + "'>" + std::string(fields) + "</object></property>";
}

std::string savedViews(std::string_view views, std::string_view chooser = "[1]")
{
    return arrayXml("views",
                    "[[Lorg.opensourcephysics.cabrillo.tracker.TView;",
                    arrayXml(chooser, "[Lorg.opensourcephysics.cabrillo.tracker.TView;", views));
}

std::string stringRow(std::string_view name, std::string_view cells)
{
    return arrayXml(name, "[Ljava.lang.String;", cells);
}

std::string membersXml(std::string_view field, std::string_view members)
{
    return "<property name='" + std::string(field) + "' type='collection' class='java.util.ArrayList'>" + std::string(members) +
           "</property>";
}

std::string dependent(std::string_view type, std::string_view fields)
{
    return viewXml(type, property("name", "dependent") + std::string(fields), "item");
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
    auto imported = motrack::parseTrackerProject(
        project(point("original") + point("other") + dependent("CenterOfMass", membersXml("masses", property("item", "original"))),
                property("selectedtrack", "original") + savedViews(viewXml("TableTView", property("selected_track", "original")))));
    const auto originalId = imported.project.pointMasses.front().id;
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.renamePointMass(document.project, 0, "renamed"));
    std::ranges::reverse(document.project.pointMasses);
    ASSERT_TRUE(edits.removePointMass(document.project, 1));
    const auto removed = motrack::indexTrackerReferences(imported.source, document.project);
    ASSERT_EQ(removed.tracks.size(), 3u);
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
    ASSERT_EQ(restored.references.size(), 3u);
    EXPECT_EQ(restored.references[2].kind, Kind::CenterOfMassMember);
    EXPECT_EQ(restored.references[2].candidateSourceIndices, (std::vector<std::size_t>{0}));
    EXPECT_EQ(restored.references[1].candidateSourceIndices, (std::vector<std::size_t>{0}));
    EXPECT_EQ(restored.references[1].originalName, "original");
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

TEST(TrackerReferences, IndexesNestedTableOwnersWithoutTreatingColumnNamesOrFormatsAsReferences)
{
    const auto columns = arrayXml("track_columns",
                                  "[[Ljava.lang.String;",
                                  stringRow("[4]", property("[0]", "unknown") + property("[1]", "null") + property("[2]", "missing")));
    const auto formats =
        arrayXml("column_formats",
                 "[[[Ljava.lang.String;",
                 arrayXml("[0]",
                          "[[Ljava.lang.String;",
                          stringRow("[0]", property("[0]", "unknown") + property("[1]", "t") + property("[2]", "null")) +
                              stringRow("[1]", property("[0]", "unknown") + property("[1]", "frame") + property("[2]", "0"))));
    const auto xml = project(
        point("null") + track("Future", "unknown"),
        savedViews(viewXml("TableTView", property("selected_track", "null") + columns + formats + property("description", "unknown"))));
    const auto result = index(xml);
    EXPECT_TRUE(result.malformedPaths.empty());
    ASSERT_EQ(result.references.size(), 4u);
    EXPECT_EQ(result.references[0].kind, Kind::TableSelection);
    EXPECT_EQ(result.references[0].path, "/views/1/1/selected_track");
    EXPECT_EQ(result.references[0].resolution, Resolution::Resolved);
    EXPECT_EQ(result.references[0].candidateSourceIndices, (std::vector<std::size_t>{0}));
    EXPECT_EQ(result.references[1].kind, Kind::TableColumnOwner);
    EXPECT_EQ(result.references[1].path, "/views/1/1/track_columns/4/0");
    EXPECT_EQ(result.references[1].candidateSourceIndices, (std::vector<std::size_t>{1}));
    EXPECT_EQ(result.references[2].kind, Kind::TableFormatOwner);
    EXPECT_EQ(result.references[2].path, "/views/1/1/column_formats/0/0/0");
    EXPECT_EQ(result.references[3].path, "/views/1/1/column_formats/0/1/0");
    EXPECT_EQ(result.references[3].candidateSourceIndices, (std::vector<std::size_t>{1}));
    EXPECT_FALSE(result.tracks[1].nativeId);
}

TEST(TrackerReferences, ResolvesPlotOwnersIncludingMissingAndAmbiguousNames)
{
    const auto owners = "<property name='track_views' type='collection' class='java.util.ArrayList'>" +
                        viewXml("PlotTrackView", property("track", "same") + property("x_var", "missing"), "item") +
                        viewXml("PlotTrackView", property("track", "missing"), "item") + "</property>";
    const auto result = index(project(point("same") + track("Future", "same"),
                                      savedViews(viewXml("PlotTView", property("selected_track", "same") + owners, "[0]"), "[3]")));
    EXPECT_TRUE(result.malformedPaths.empty());
    ASSERT_EQ(result.references.size(), 3u);
    EXPECT_EQ(result.references[0].kind, Kind::PlotSelection);
    EXPECT_EQ(result.references[1].kind, Kind::PlotOwner);
    EXPECT_EQ(result.references[1].path, "/views/3/0/track_views/0/track");
    EXPECT_EQ(result.references[1].resolution, Resolution::Ambiguous);
    EXPECT_EQ(result.references[1].candidateSourceIndices, (std::vector<std::size_t>{0, 1}));
    EXPECT_EQ(result.references[2].path, "/views/3/0/track_views/1/track");
    EXPECT_EQ(result.references[2].resolution, Resolution::Missing);
    EXPECT_TRUE(index(project(point("A"), savedViews(viewXml("PlotTView", {}, "[0]")))).malformedPaths.empty());
    EXPECT_TRUE(index(project(point("A"), savedViews(viewXml("FutureView", property("track", "A"), "[2]")))).references.empty());
}

TEST(TrackerReferences, IndexesOrderedPlotGuestsAndKeepsDataStringsSeparate)
{
    const auto plots = viewXml("TrackPlottingPanel",
                               stringRow("guests", property("[0]", "C") + property("[1]", "B") + property("[2]", "B")) +
                                   property("x_var", "A") + property("description", "C"),
                               "plot0") +
                       viewXml("TrackPlottingPanel", stringRow("guests", property("[0]", "null")), "plot2");
    const auto owners = membersXml("track_views", viewXml("PlotTrackView", property("track", "A") + plots, "item"));
    const auto result = index(
        project(point("A") + point("B") + track("Future", "C") + point("null"), savedViews(viewXml("PlotTView", owners, "[0]"), "[0]")));
    EXPECT_TRUE(result.malformedPaths.empty());
    ASSERT_EQ(result.references.size(), 5u);
    EXPECT_EQ(result.references[0].kind, Kind::PlotOwner);
    for (std::size_t i = 1; i < result.references.size(); ++i)
    {
        EXPECT_EQ(result.references[i].kind, Kind::PlotGuest);
        EXPECT_EQ(result.references[i].resolution, Resolution::Resolved);
    }
    EXPECT_EQ(result.references[1].candidateSourceIndices, (std::vector<std::size_t>{2}));
    EXPECT_EQ(result.references[2].candidateSourceIndices, (std::vector<std::size_t>{1}));
    EXPECT_EQ(result.references[3].candidateSourceIndices, (std::vector<std::size_t>{1}));
    EXPECT_EQ(result.references[4].candidateSourceIndices, (std::vector<std::size_t>{3}));
    EXPECT_EQ(result.references[1].path, "/views/0/0/track_views/0/plot0/guests/0");
    EXPECT_EQ(result.references[3].path, "/views/0/0/track_views/0/plot0/guests/2");
    EXPECT_EQ(result.references[4].path, "/views/0/0/track_views/0/plot2/guests/0");
}

TEST(TrackerReferences, IndexesLegacyPlotGuestSlotsWithoutClaimingActiveLoaderState)
{
    const auto fields = property("selected_track", "A") +
                        viewXml("TrackPlottingPanel", stringRow("guests", property("[0]", "same") + property("[1]", "missing")), "plot0") +
                        viewXml("TrackPlottingPanel", "", "plot1") + viewXml("TrackPlottingPanel", stringRow("guests", ""), "plot2");
    const auto result =
        index(project(point("A") + point("same") + track("Future", "same"), savedViews(viewXml("PlotTView", fields, "[0]"))));
    EXPECT_TRUE(result.malformedPaths.empty());
    ASSERT_EQ(result.references.size(), 3u);
    EXPECT_EQ(result.references[1].path, "/views/1/0/plot0/guests/0");
    EXPECT_EQ(result.references[1].resolution, Resolution::Ambiguous);
    EXPECT_EQ(result.references[1].candidateSourceIndices, (std::vector<std::size_t>{1, 2}));
    EXPECT_EQ(result.references[2].resolution, Resolution::Missing);
    // A track_views collection selects the modern representation. Inactive legacy fields are ignored.
    const auto modern = index(project(point("A"), savedViews(viewXml("PlotTView", fields + membersXml("track_views", ""), "[0]"))));
    ASSERT_EQ(modern.references.size(), 1u);
}

TEST(TrackerReferences, RejectsMalformedPlotObjectsAndGuestArraysWithoutPartialReferences)
{
    const auto valid = stringRow("guests", property("[0]", "A"));
    const std::string oversized(4097, 'x');
    for (const auto& invalid :
         {property("plot0", "A"),
          viewXml("FuturePlot", valid, "plot0"),
          viewXml("TrackPlottingPanel", valid, "plot0") + viewXml("TrackPlottingPanel", valid, "plot0"),
          viewXml("TrackPlottingPanel", valid + valid, "plot0"),
          viewXml("TrackPlottingPanel", property("guests", "A"), "plot0"),
          viewXml("TrackPlottingPanel", arrayXml("guests", "[Ljava.lang.Object;", property("[0]", "A")), "plot0"),
          viewXml("TrackPlottingPanel", stringRow("guests", property("[1]", "A")), "plot0"),
          viewXml("TrackPlottingPanel", stringRow("guests", property("[0]", "A") + property("[2]", "A")), "plot0"),
          viewXml("TrackPlottingPanel", stringRow("guests", property("[0]", "A") + property("[0]", "A")), "plot0"),
          viewXml("TrackPlottingPanel", stringRow("guests", property("[0]", "A") + property("[1]", oversized)), "plot0"),
          viewXml("TrackPlottingPanel",
                  stringRow("guests", property("[0]", "A") + "<property name='[1]' type='object'>null</property>"),
                  "plot0")})
    {
        SCOPED_TRACE(invalid);
        const auto result = index(project(point("A"), savedViews(viewXml("PlotTView", invalid, "[0]"))));
        EXPECT_FALSE(result.malformedPaths.empty());
        EXPECT_TRUE(result.references.empty());
    }
}

TEST(TrackerReferences, PlotGuestsShareNestedEntryAndCandidateBudgets)
{
    std::string many;
    for (int i = 0; i < 9998; ++i)
    {
        many += property("[" + std::to_string(i) + "]", "A");
    }
    const auto huge = viewXml("TrackPlottingPanel", stringRow("guests", many), "plot0");
    EXPECT_THROW((void)index(project(point("A"), savedViews(viewXml("PlotTView", huge, "[0]")))), motrack::ProjectError);
    std::string tracks;
    for (int i = 0; i < 400; ++i)
    {
        tracks += point("same");
    }
    std::string guests;
    for (int i = 0; i < 251; ++i)
    {
        guests += property("[" + std::to_string(i) + "]", "same");
    }
    const auto dense = viewXml("TrackPlottingPanel", stringRow("guests", guests), "plot0");
    EXPECT_THROW((void)index(project(tracks, savedViews(viewXml("PlotTView", dense, "[0]")))), motrack::ProjectError);
}

TEST(TrackerReferences, PlotGuestIdentitySurvivesNativeRenameDeleteUndoAndReopen)
{
    const auto fields = viewXml("TrackPlottingPanel", stringRow("guests", property("[0]", "original")), "plot0");
    auto imported =
        motrack::parseTrackerProject(project(point("original") + point("other"), savedViews(viewXml("PlotTView", fields, "[0]"))));
    const auto id = imported.project.pointMasses[0].id;
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.renamePointMass(document.project, 0, "new name"));
    std::ranges::reverse(document.project.pointMasses);
    ASSERT_TRUE(edits.removePointMass(document.project, 1));
    const auto deleted = motrack::indexTrackerReferences(imported.source, document.project);
    ASSERT_EQ(deleted.references.size(), 1u);
    EXPECT_EQ(deleted.tracks[0].nativeId, id);
    EXPECT_EQ(deleted.references[0].candidateSourceIndices, (std::vector<std::size_t>{0}));
    ASSERT_TRUE(edits.undo(document.project));
    QTemporaryDir temporary;
    const auto file = temporary.filePath("plot-guests.motrack");
    motrack::writeWorkspaceDocument(file, document);
    const auto reopened = motrack::readWorkspaceDocument(file);
    if (!reopened.trackerSource)
    {
        FAIL() << "Expected original source";
    }
    const auto restored = motrack::indexTrackerReferences(*reopened.trackerSource, reopened.project);
    ASSERT_EQ(restored.references.size(), 1u);
    EXPECT_EQ(restored.tracks[0].nativeId, id);
    EXPECT_EQ(restored.references[0].originalName, "original");
    EXPECT_EQ(restored.references[0].candidateSourceIndices, (std::vector<std::size_t>{0}));
    EXPECT_EQ(reopened.trackerSource->xml, imported.source.xml);
}

TEST(TrackerReferences, IndexesTableDatasetOwnersWithoutInterpretingSettingsAsNames)
{
    const auto datasets = arrayXml("dataset_indices",
                                   "[[Ljava.lang.String;",
                                   stringRow("[0]", property("[0]", "profile") + property("[1]", "2")) +
                                       stringRow("[1]", property("[0]", "null") + property("[1]", "0")) +
                                       stringRow("[2]", property("[0]", "profile") + property("[1]", "2147483647")) +
                                       stringRow("[3]", property("[0]", "élève") + property("[1]", "1")));
    const auto result =
        index(project(track("org.opensourcephysics.cabrillo.tracker.LineProfile", "profile") + point("null") + point("2") + point("élève"),
                      savedViews(viewXml("TableTView", datasets))));
    EXPECT_TRUE(result.malformedPaths.empty());
    ASSERT_EQ(result.references.size(), 4u);
    const std::vector<std::size_t> targets{0, 1, 0, 3};
    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        EXPECT_EQ(result.references[i].kind, Kind::TableDatasetOwner);
        EXPECT_EQ(result.references[i].resolution, Resolution::Resolved);
        EXPECT_EQ(result.references[i].path, "/views/1/1/dataset_indices/" + std::to_string(i) + "/0");
        EXPECT_EQ(result.references[i].candidateSourceIndices, (std::vector<std::size_t>{targets[i]}));
    }
    EXPECT_FALSE(result.tracks[0].nativeId);
    // Serialized references are retained even without track_columns, which gates the Java loader.
    // They do not establish that a live table or the requested dataset exists.
    EXPECT_TRUE(index(project({}, savedViews(viewXml("TableTView", arrayXml("dataset_indices", "[[Ljava.lang.String;", {})))))
                    .malformedPaths.empty());
}

TEST(TrackerReferences, TableDatasetsKeepMissingAndAmbiguousOwnersExplicit)
{
    const auto datasets = arrayXml("dataset_indices",
                                   "[[Ljava.lang.String;",
                                   stringRow("[0]", property("[0]", "same") + property("[1]", "0")) +
                                       stringRow("[1]", property("[0]", "missing") + property("[1]", "1")));
    const auto result = index(project(point("same") + track("Future", "same"), savedViews(viewXml("TableTView", datasets))));
    EXPECT_TRUE(result.malformedPaths.empty());
    ASSERT_EQ(result.references.size(), 2u);
    EXPECT_EQ(result.references[0].resolution, Resolution::Ambiguous);
    EXPECT_EQ(result.references[0].candidateSourceIndices, (std::vector<std::size_t>{0, 1}));
    EXPECT_EQ(result.references[1].resolution, Resolution::Missing);
}

TEST(TrackerReferences, RejectsMalformedDatasetRowsWithoutEmittingPartialOwners)
{
    const auto row = stringRow("[0]", property("[0]", "A") + property("[1]", "2"));
    const auto valid = arrayXml("dataset_indices", "[[Ljava.lang.String;", row);
    for (const auto& invalid :
         {property("dataset_indices", "A"),
          valid + valid,
          arrayXml("dataset_indices", "wrong", row),
          arrayXml("dataset_indices", "[[Ljava.lang.String;", stringRow("[1]", property("[0]", "A") + property("[1]", "2"))),
          arrayXml("dataset_indices", "[[Ljava.lang.String;", row + row),
          arrayXml("dataset_indices", "[[Ljava.lang.String;", row + property("[1]", "null")),
          arrayXml("dataset_indices", "[[Ljava.lang.String;", row + stringRow("[1]", property("[0]", "A"))),
          arrayXml("dataset_indices", "[[Ljava.lang.String;", row + stringRow("[1]", property("[0]", "A") + property("[2]", "2"))),
          arrayXml("dataset_indices",
                   "[[Ljava.lang.String;",
                   row + stringRow("[1]", property("[0]", "A") + property("[1]", "2") + property("[2]", "3"))),
          arrayXml("dataset_indices",
                   "[[Ljava.lang.String;",
                   row + stringRow("[1]", property("[0]", "A") + "<property name='[1]' type='int'>2</property>")),
          arrayXml("dataset_indices",
                   "[[Ljava.lang.String;",
                   row + stringRow("[1]", property("[0]", std::string(4097, 'a')) + property("[1]", "2")))})
    {
        SCOPED_TRACE(invalid);
        const auto result = index(project(point("A"), savedViews(viewXml("TableTView", invalid))));
        EXPECT_TRUE(result.references.empty());
        EXPECT_FALSE(result.malformedPaths.empty());
    }
    for (const auto* setting : {"", "-1", "+1", "01", " 1", "1 ", "1.0", "2147483648", "4294967296", "A", "１"})
    {
        SCOPED_TRACE(setting);
        const auto datasets =
            arrayXml("dataset_indices", "[[Ljava.lang.String;", row + stringRow("[1]", property("[0]", "A") + property("[1]", setting)));
        const auto result = index(project(point("A"), savedViews(viewXml("TableTView", datasets))));
        EXPECT_TRUE(result.references.empty());
        EXPECT_EQ(result.malformedPaths, (std::vector<std::string>{"/views/1/1/dataset_indices/1/1"}));
    }
}

TEST(TrackerReferences, TableDatasetsShareNestedEntryAndCandidateBudgets)
{
    std::string rows;
    for (unsigned i = 0; i < 3333; ++i)
    {
        rows += stringRow("[" + std::to_string(i) + "]", property("[0]", "A") + property("[1]", "0"));
    }
    EXPECT_THROW(
        (void)index(project(point("A"), savedViews(viewXml("TableTView", arrayXml("dataset_indices", "[[Ljava.lang.String;", rows))))),
        motrack::ProjectError);
    std::string tracks;
    for (unsigned i = 0; i < 400; ++i)
        tracks += point("same");
    rows.clear();
    for (unsigned i = 0; i < 251; ++i)
    {
        rows += stringRow("[" + std::to_string(i) + "]", property("[0]", "same") + property("[1]", "0"));
    }
    EXPECT_THROW((void)index(project(tracks, savedViews(viewXml("TableTView", arrayXml("dataset_indices", "[[Ljava.lang.String;", rows))))),
                 motrack::ProjectError);
}

TEST(TrackerReferences, TableDatasetIdentitySurvivesNativeRenameDeleteUndoRedoAndReopen)
{
    const auto fields =
        arrayXml("dataset_indices", "[[Ljava.lang.String;", stringRow("[0]", property("[0]", "original") + property("[1]", "0")));
    auto imported = motrack::parseTrackerProject(project(point("original") + point("other"), savedViews(viewXml("TableTView", fields))));
    const auto id = imported.project.pointMasses[0].id;
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.renamePointMass(document.project, 0, "new name"));
    std::ranges::reverse(document.project.pointMasses);
    ASSERT_TRUE(edits.removePointMass(document.project, 1));
    const auto deleted = motrack::indexTrackerReferences(imported.source, document.project);
    ASSERT_EQ(deleted.references.size(), 1u);
    EXPECT_EQ(deleted.tracks[0].nativeId, id);
    EXPECT_EQ(deleted.references[0].candidateSourceIndices, (std::vector<std::size_t>{0}));
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    ASSERT_TRUE(edits.undo(document.project));
    QTemporaryDir temporary;
    const auto file = temporary.filePath("table-datasets.motrack");
    motrack::writeWorkspaceDocument(file, document);
    const auto reopened = motrack::readWorkspaceDocument(file);
    if (!reopened.trackerSource)
    {
        FAIL() << "Expected original source";
    }
    const auto restored = motrack::indexTrackerReferences(*reopened.trackerSource, reopened.project);
    ASSERT_EQ(restored.references.size(), 1u);
    EXPECT_EQ(restored.tracks[0].nativeId, id);
    EXPECT_EQ(restored.references[0].originalName, "original");
    EXPECT_EQ(restored.references[0].candidateSourceIndices, (std::vector<std::size_t>{0}));
    EXPECT_EQ(reopened.trackerSource->xml, imported.source.xml);
}

TEST(TrackerReferences, RejectsMalformedViewContainersIndicesAndObjects)
{
    const auto valid = savedViews(viewXml("TableTView", property("selected_track", "A")));
    for (const auto& field : {property("views", "A"),
                              valid + valid,
                              arrayXml("views", "wrong", ""),
                              arrayXml("views", "[[Lorg.opensourcephysics.cabrillo.tracker.TView;", "<other/>"),
                              savedViews(viewXml("TableTView", {}), "[4]"),
                              savedViews(viewXml("TableTView", {}), "[01]"),
                              savedViews(viewXml("TableTView", {}), "[-1]"),
                              savedViews(viewXml("TableTView", {}), "garbage"),
                              savedViews(viewXml("TableTView", {}, "[0]")),
                              savedViews(viewXml("PlotTView", {}, "[1]")),
                              savedViews("<property name='[1]' type='object'><object class='Future'/><object class='Future'/></property>"),
                              savedViews(viewXml("TableTView", {}) + viewXml("TableTView", {})),
                              arrayXml("views", "[[Lorg.opensourcephysics.cabrillo.tracker.TView;", property("[0]", "A")),
                              savedViews("<property name='[0]' type='object'>text<object class='Future'/></property>")})
    {
        SCOPED_TRACE(field);
        const auto result = index(project(point("A"), field));
        EXPECT_TRUE(result.references.empty());
        EXPECT_FALSE(result.malformedPaths.empty());
    }
    EXPECT_TRUE(index(project({}, arrayXml("views", "[[Lorg.opensourcephysics.cabrillo.tracker.TView;", {}))).malformedPaths.empty());
}

TEST(TrackerReferences, RejectsMalformedTableRowsWithoutInventingPartialOwners)
{
    const auto good = stringRow("[0]", property("[0]", "A") + property("[1]", "t"));
    for (const auto& row : {stringRow("[1]", {}),
                            stringRow("[1]", property("[1]", "A")),
                            stringRow("[1]", property("[0]", "A") + property("[2]", "x")),
                            stringRow("[1]", property("[0]", "A") + property("[0]", "A")),
                            stringRow("[1]", property("[0]", "A") + "<property name='[1]' type='int'>1</property>"),
                            stringRow("[1]", property("[0]", std::string(4097, 'x'))),
                            property("[1]", "A")})
    {
        const auto fields = arrayXml("track_columns", "[[Ljava.lang.String;", good + row);
        const auto result = index(project(point("A"), savedViews(viewXml("TableTView", fields))));
        EXPECT_TRUE(result.references.empty());
        EXPECT_FALSE(result.malformedPaths.empty());
    }
    const auto group = [](std::string_view rows)
    {
        return arrayXml("column_formats", "[[[Ljava.lang.String;", arrayXml("[0]", "[[Ljava.lang.String;", rows));
    };
    const auto first = stringRow("[0]", property("[0]", "A") + property("[1]", "t") + property("[2]", "0.00"));
    for (const auto& fields :
         {group({}),
          group(stringRow("[2]", property("[0]", "A"))),
          group(stringRow("[0]", property("[0]", "A"))),
          group(first + stringRow("[1]", property("[0]", "B") + property("[1]", "frame") + property("[2]", "0"))),
          arrayXml("column_formats", "[[[Ljava.lang.String;", arrayXml("[1]", "[[Ljava.lang.String;", first)),
          group(first + stringRow("[2]", property("[0]", "A") + property("[1]", "frame") + property("[2]", "0"))),
          property("column_formats", "A"),
          property("track_columns", "A"),
          arrayXml("track_columns", "[[Ljava.lang.String;", good) + arrayXml("track_columns", "[[Ljava.lang.String;", good)})
    {
        const auto result = index(project(point("A") + point("B"), savedViews(viewXml("TableTView", fields))));
        EXPECT_TRUE(result.references.empty());
        EXPECT_FALSE(result.malformedPaths.empty());
    }
}

TEST(TrackerReferences, RejectsMalformedPlotOwnerCollectionsAndRequiredNames)
{
    for (const auto& fields :
         {property("track_views", "A"),
          std::string("<property name='track_views' type='collection' class='java.util.ArrayList'><other/></property>"),
          std::string("<property name='track_views' type='collection' class='wrong'/>"),
          std::string("<property name='track_views' type='collection' class='java.util.ArrayList'>") +
              viewXml("PlotTrackView", {}, "item") + "</property>",
          std::string("<property name='track_views' type='collection' class='java.util.ArrayList'>") +
              viewXml("Future", property("track", "A"), "item") + "</property>"})
    {
        const auto result = index(project(point("A"), savedViews(viewXml("PlotTView", fields, "[0]"))));
        EXPECT_TRUE(result.references.empty());
        EXPECT_FALSE(result.malformedPaths.empty());
    }
}

TEST(TrackerReferences, BoundsTotalNestedEntriesWithoutAllocatingSparseArrays)
{
    const auto fields = arrayXml("track_columns", "[[Ljava.lang.String;", stringRow("[9999]", property("[0]", "A")));
    const auto valid = index(project(point("A"), savedViews(viewXml("TableTView", fields))));
    EXPECT_TRUE(valid.malformedPaths.empty());
    ASSERT_EQ(valid.references.size(), 1u);
    EXPECT_EQ(valid.references[0].path, "/views/1/1/track_columns/9999/0");
    const auto tooFar = arrayXml("track_columns", "[[Ljava.lang.String;", stringRow("[10000]", property("[0]", "A")));
    EXPECT_FALSE(index(project(point("A"), savedViews(viewXml("TableTView", tooFar)))).malformedPaths.empty());
    std::string many;
    for (unsigned row = 0; row < 5000; ++row)
    {
        many += stringRow("[" + std::to_string(row) + "]", property("[0]", "A"));
    }
    EXPECT_THROW(
        (void)index(project(point("A"), savedViews(viewXml("TableTView", arrayXml("track_columns", "[[Ljava.lang.String;", many))))),
        motrack::ProjectError);
}

TEST(TrackerReferences, BoundsAggregateAmbiguousCandidatesBeforeCopying)
{
    std::string tracks;
    for (unsigned source = 0; source < 1000; ++source)
    {
        tracks += track("Future", "same");
    }
    std::string owners;
    for (unsigned owner = 0; owner < 100; ++owner)
    {
        owners += viewXml("PlotTrackView", property("track", "same"), "item");
    }
    const auto views = savedViews(viewXml(
        "PlotTView", "<property name='track_views' type='collection' class='java.util.ArrayList'>" + owners + "</property>", "[0]"));
    const auto exact = index(project(tracks, views));
    ASSERT_EQ(exact.references.size(), 100u);
    for (const auto& reference : exact.references)
    {
        EXPECT_EQ(reference.resolution, Resolution::Ambiguous);
        ASSERT_EQ(reference.candidateSourceIndices.size(), 1000u);
        EXPECT_EQ(reference.candidateSourceIndices.back(), 999u);
    }
    // A panel reference shares the same budget; one extra ambiguous owner must reject the index.
    EXPECT_THROW((void)index(project(tracks, property("selectedtrack", "same") + views)), motrack::ProjectError);
}

TEST(TrackerReferences, IndexesTypedConstituentsAndPreservesCollectionOrdinals)
{
    const auto tracks =
        point("same") + track("org.opensourcephysics.cabrillo.tracker.Vector", "same") + point("null") +
        dependent("CenterOfMass",
                  membersXml("masses", property("[900]", "same") + property("item", "null") + property("item", "missing"))) +
        dependent("VectorSum", membersXml("vectors", property("item", "same") + property("item", "null") + property("item", "same")));
    const auto result = index(project(tracks));
    EXPECT_TRUE(result.malformedPaths.empty());
    ASSERT_EQ(result.references.size(), 6u);
    EXPECT_EQ(result.references[0].kind, Kind::CenterOfMassMember);
    EXPECT_EQ(result.references[0].path, "/tracks/3/masses/0");
    EXPECT_EQ(result.references[0].resolution, Resolution::Resolved);
    EXPECT_EQ(result.references[0].candidateSourceIndices, (std::vector<std::size_t>{0}));
    EXPECT_EQ(result.references[1].resolution, Resolution::Resolved); // Direct null is a real name.
    EXPECT_EQ(result.references[1].candidateSourceIndices, (std::vector<std::size_t>{2}));
    EXPECT_EQ(result.references[2].resolution, Resolution::Missing);
    EXPECT_EQ(result.references[3].kind, Kind::VectorSumMember);
    EXPECT_EQ(result.references[3].path, "/tracks/4/vectors/0");
    EXPECT_EQ(result.references[3].resolution, Resolution::Resolved);
    EXPECT_EQ(result.references[3].candidateSourceIndices, (std::vector<std::size_t>{1}));
    EXPECT_EQ(result.references[4].resolution, Resolution::Missing); // Same name exists only on wrong base type.
    EXPECT_TRUE(result.references[4].candidateSourceIndices.empty());
    EXPECT_EQ(result.references[5].path, "/tracks/4/vectors/2"); // Repeated serialized slots stay explicit.
    EXPECT_EQ(result.references[5].candidateSourceIndices, (std::vector<std::size_t>{1}));
    EXPECT_TRUE(index(project(dependent("VectorSum", membersXml("vectors", "")))).references.empty());
    EXPECT_TRUE(index(project(dependent("FutureSum", membersXml("vectors", property("item", "same"))))).references.empty());
}

TEST(TrackerReferences, DoesNotGuessDuplicateOrSubclassConstituentSemantics)
{
    const auto result = index(project(
        point("duplicate") + point("duplicate") + track("org.opensourcephysics.cabrillo.tracker.Vector", "duplicate") +
        track("FutureMass", "uncertain") + point("uncertain") +
        dependent("CenterOfMass",
                  membersXml("masses", property("item", "duplicate") + property("item", "uncertain") + property("item", "dependent"))) +
        dependent("VectorSum", membersXml("vectors", property("item", "duplicate") + property("item", "uncertain")))));
    ASSERT_EQ(result.references.size(), 5u);
    EXPECT_EQ(result.references[0].resolution, Resolution::Ambiguous);
    EXPECT_EQ(result.references[0].candidateSourceIndices, (std::vector<std::size_t>{0, 1}));
    EXPECT_EQ(result.references[1].resolution, Resolution::Unclassified);
    EXPECT_EQ(result.references[1].candidateSourceIndices, (std::vector<std::size_t>{3, 4}));
    EXPECT_EQ(result.references[2].resolution, Resolution::Unclassified); // Includes self/nested dependencies; never evaluate them.
    EXPECT_EQ(result.references[2].candidateSourceIndices, (std::vector<std::size_t>{5, 6}));
    EXPECT_EQ(result.references[3].resolution, Resolution::Resolved);
    EXPECT_EQ(result.references[3].candidateSourceIndices, (std::vector<std::size_t>{2}));
    EXPECT_EQ(result.references[4].resolution, Resolution::Unclassified);
    EXPECT_EQ(result.references[4].candidateSourceIndices, (std::vector<std::size_t>{3}));
}

TEST(TrackerReferences, RejectsMalformedConstituentListsWithoutPartialMembers)
{
    const auto good = membersXml("vectors", property("item", "A"));
    for (const auto& fields : {std::string{},
                               property("vectors", "A"),
                               good + good,
                               std::string("<property name='vectors' type='collection' class='java.util.LinkedList'/>"),
                               membersXml("vectors", "unexpected"),
                               membersXml("vectors", property("item", "A") + "<property name='item' type='object'>null</property>"),
                               membersXml("vectors", property("item", "A") + "<property name='item' type='string'><object/></property>"),
                               membersXml("vectors", property("item", std::string(4097, 'a')))})
    {
        const auto result = index(project(track("org.opensourcephysics.cabrillo.tracker.Vector", "A") + dependent("VectorSum", fields)));
        EXPECT_TRUE(result.references.empty());
        ASSERT_EQ(result.malformedPaths.size(), 1u);
        EXPECT_TRUE(result.malformedPaths[0].starts_with("/tracks/1/vectors"));
    }
    const auto missingMasses = index(project(dependent("CenterOfMass", "")));
    EXPECT_EQ(missingMasses.malformedPaths, (std::vector<std::string>{"/tracks/0/masses"}));
}

TEST(TrackerReferences, BoundsConstituentsTogetherWithViewsAndWrongTypeCandidateVisits)
{
    std::string members;
    for (int i = 0; i < 5000; ++i)
    {
        members += property("item", "missing");
    }
    const auto tracks = dependent("CenterOfMass", membersXml("masses", members)) + dependent("VectorSum", membersXml("vectors", members));
    EXPECT_EQ(index(project(tracks)).references.size(), 10000u);
    EXPECT_THROW((void)index(project(tracks, savedViews(viewXml("TableTView", "")))), motrack::ProjectError);

    std::string duplicates;
    for (int i = 0; i < 1000; ++i)
    {
        duplicates += point("same");
    }
    members.clear();
    for (int i = 0; i < 100; ++i)
    {
        members += property("item", "same");
    }
    const auto exact = index(project(duplicates + dependent("VectorSum", membersXml("vectors", members))));
    ASSERT_EQ(exact.references.size(), 100u);
    for (const auto& reference : exact.references)
    {
        EXPECT_EQ(reference.resolution, Resolution::Missing);
        EXPECT_TRUE(reference.candidateSourceIndices.empty());
    }
    EXPECT_THROW((void)index(project(duplicates + dependent("VectorSum", membersXml("vectors", members + property("item", "same"))))),
                 motrack::ProjectError);
}

} // namespace
