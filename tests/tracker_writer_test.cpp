#include "tracker_writer.hpp"

#include "track_edits.hpp"
#include "workspace_document.hpp"

#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <string_view>

namespace
{

std::string_view sourceXml()
{
    return "<?xml version='1.0' encoding='ISO-8859-1'?>\r\n"
           "<!-- untouched comment -->\r\n"
           "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>"
           "<property name='description'>caf\xE9</property>"
           "<property name='future'><![CDATA[<unknown>]]></property>"
           "<property name='tracks'>"
           "<property name='[10]'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>"
           "<property name='name'>same</property><property name='mass' type='double'>1<!-- mass comment --><![CDATA[.0]]></property>"
           "</object></property>"
           "<property name='[20]'><object class='FutureTrack'><property name='link'>same</property></object></property>"
           "<property name='[30]'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>"
           "<property name='name'>same</property></object></property>"
           "<property name='[40]'><object class='org.opensourcephysics.cabrillo.tracker.Vector'>"
           "<property name='name'>vector</property></object></property>"
           "</property></object>\r\n";
}

std::string readBytes(const QString& path)
{
    QFile input(path);
    return input.open(QIODevice::ReadOnly) ? input.readAll().toStdString() : std::string{};
}

TEST(TrackerWriter, PatchesTheBoundMassAfterNativeReopenAndKeepsUnknownContent)
{
    QTemporaryDir temporary;
    auto imported = motrack::parseTrackerProject(std::string(sourceXml()));
    auto source = imported.source;
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = source};
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 2.5));
    ASSERT_TRUE(edits.setPointMassValue(document.project, 1, 0.12345678901234567));
    ASSERT_TRUE(edits.createPointMass(document.project, "temporary"));
    ASSERT_TRUE(edits.undo(document.project)); // The native counter may advance without an exported track.
    std::ranges::reverse(document.project.pointMasses);
    const auto native = temporary.filePath("saved.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = {};
    auto reopened = motrack::readWorkspaceDocument(native);
    if (!reopened.trackerSource || !reopened.trackerSource->bindings)
    {
        FAIL() << "Expected persisted XML associations.";
    }
    std::ranges::reverse(*reopened.trackerSource->bindings);
    const auto output = temporary.filePath(QString::fromUtf8("élève.trk"));
    motrack::writeTrackerProject(output, reopened.project, *reopened.trackerSource);
    const auto bytes = readBytes(output);
    EXPECT_NE(bytes.find("encoding=\"UTF-8\""), std::string::npos);
    EXPECT_NE(bytes.find("<!-- untouched comment -->"), std::string::npos);
    EXPECT_NE(bytes.find("<!-- mass comment -->"), std::string::npos);
    EXPECT_NE(bytes.find("<![CDATA[<unknown>]]>"), std::string::npos);
    EXPECT_NE(bytes.find("FutureTrack"), std::string::npos);
    EXPECT_EQ(reopened.trackerSource->xml, source.xml);
    const auto result = motrack::parseLegacyProject(bytes);
    ASSERT_EQ(result.pointMasses.size(), 2u);
    EXPECT_EQ(result.pointMasses[0].mass, 2.5);
    EXPECT_EQ(result.pointMasses[1].mass, 0.12345678901234567);
    EXPECT_EQ(result.description, "caf\xC3\xA9");
    EXPECT_EQ(result.unsupportedTrackTypes, imported.project.unsupportedTrackTypes);
    EXPECT_EQ(result.vectors, imported.project.vectors);
}

TEST(TrackerWriter, LeavesAnUnchangedSourceByteExact)
{
    const auto imported = motrack::parseTrackerProject(std::string(sourceXml()));
    EXPECT_EQ(motrack::renderTrackerProject(imported.project, imported.source), sourceXml());
    const auto unknown = motrack::parseTrackerProject(
        "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'><property name='future'>keep</property></object>");
    EXPECT_EQ(motrack::renderTrackerProject(unknown.project, unknown.source), unknown.source.xml);
}

TEST(TrackerWriter, RejectsUnsupportedOrInvalidEditsBeforeReplacingTheDestination)
{
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    const auto imported = motrack::parseTrackerProject(std::string(sourceXml()));
    motrack::writeTrackerProject(output, imported.project, imported.source);
    const std::vector<std::function<void(motrack::LegacyProject&)>> changes{
        [](auto& project) { project.pointMasses[0].name = "renamed"; },
        [](auto& project) { project.pointMasses.pop_back(); },
        [](auto& project) { project.vectors[0].name = "renamed"; },
        [](auto& project) { project.clip.firstFrame = 2; },
        [](auto& project) { project.description = "edited"; },
        [](auto& project) { project.lengthUnit = "cm"; },
        [](auto& project) { project.massUnit = "g"; },
        [](auto& project) { project.calibration = motrack::Calibration{}; },
        [](auto& project) { project.pointMasses[0].mass = std::numeric_limits<double>::quiet_NaN(); },
        [](auto& project)
        {
            project.pointMasses[0].mass = 0;
        }};
    for (const auto& change : changes)
    {
        auto project = imported.project;
        change(project);
        EXPECT_THROW(motrack::writeTrackerProject(output, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), imported.source.xml);
    }
    auto missing = imported.source;
    missing.bindings.reset();
    EXPECT_THROW(motrack::writeTrackerProject(output, imported.project, missing), motrack::ProjectError);
    EXPECT_EQ(readBytes(output), imported.source.xml);
    EXPECT_THROW(motrack::writeTrackerProject(temporary.filePath("bad.trz"), imported.project, imported.source), motrack::ProjectError);
    EXPECT_THROW(motrack::writeTrackerProject(temporary.filePath("missing/child.trk"), imported.project, imported.source),
                 motrack::ProjectError);
}

TEST(TrackerWriter, RejectsAmbiguousOrStructuredMassProperties)
{
    for (const auto* mass : {"<property name='mass' type='double'>1</property><property name='mass' type='double'>2</property>",
                             "<property name='mass' type='object'><object/></property>"})
    {
        auto imported = motrack::parseTrackerProject(
            std::string("<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'><property name='tracks'>") +
            "<property name='item'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>" + mass +
            "</object></property></property></object>");
        imported.project.pointMasses.front().mass = 2.5;
        EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(imported.project, imported.source)), motrack::ProjectError);
    }
}

TEST(TrackerWriter, RejectsMediaUntilRelocationAndPackagingAreSupported)
{
    const auto document = motrack::importTrackerDocument(QString::fromUtf8(MOTRACK_FIXTURES) + "/image-sequence.trk");
    if (!document.trackerSource)
    {
        FAIL() << "Expected import source.";
    }
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(document.project, *document.trackerSource)), motrack::ProjectError);
}

std::string markedXml(const std::string& keys = "{0}")
{
    return "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'><property name='tracks'>"
           "<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>"
           "<property name='name' type='string'>mass</property>"
           "<property name='framedata' type='array' class='[Lorg.opensourcephysics.cabrillo.tracker.PointMass$FrameData;'>"
           "<property name='[0]' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass$FrameData'>"
           "<property name='x' type='double'>1</property><property name='y' type='double'>2</property></object></property>"
           "<property name='[2]' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass$FrameData'>"
           "<property name='x' type='double'>3</property><property name='y' type='double'>4</property>"
           "<property name='future'>keep-step</property></object></property>"
           "<property name='[3]' type='object'>null<!-- keep placeholder comment --></property>"
           "</property><property name='keyFrames' type='array' class='[I'><property name='array' type='string'>" +
           keys + "</property></property></object></property></property></object>";
}

TEST(TrackerWriter, UpdatesAddsAndDeletesMarksWithManualKeysAndUnknownStepProperties)
{
    auto imported = motrack::parseTrackerProject(markedXml());
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.markPointMass(imported.project, 0, {.frame = 2, .x = 30.125, .y = -40.5}));
    ASSERT_TRUE(edits.markPointMass(imported.project, 0, {.frame = 3, .x = 50, .y = 60}));
    ASSERT_TRUE(edits.markPointMass(imported.project, 0, {.frame = 1, .x = 10, .y = 20}));
    ASSERT_TRUE(edits.markPointMass(imported.project, 0, {.frame = 5, .x = 70, .y = 80}));
    ASSERT_TRUE(edits.removeMark(imported.project, 0, 0));
    const auto output = motrack::renderTrackerProject(imported.project, imported.source);
    const auto reopened = motrack::parseLegacyProject(output);
    EXPECT_EQ(reopened.pointMasses.front().marks, imported.project.pointMasses.front().marks);
    EXPECT_NE(output.find("{1,2,3,5}"), std::string::npos);
    EXPECT_NE(output.find("keep-step"), std::string::npos);
    EXPECT_NE(output.find("keep placeholder comment"), std::string::npos);
    ASSERT_TRUE(edits.undo(imported.project));
    const auto undone = motrack::renderTrackerProject(imported.project, imported.source);
    EXPECT_NE(undone.find("{0,1,2,3,5}"), std::string::npos);
    ASSERT_TRUE(edits.redo(imported.project));
    EXPECT_EQ(motrack::renderTrackerProject(imported.project, imported.source), output);
}

TEST(TrackerWriter, RetainsUnchangedAutomaticMarkKeysAndSupportsEmptyOrNewFrameArrays)
{
    auto imported = motrack::parseTrackerProject(markedXml());
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.markPointMass(imported.project, 0, {.frame = 0, .x = 12, .y = 24}));
    const auto output = motrack::renderTrackerProject(imported.project, imported.source);
    EXPECT_NE(output.find("{0}"), std::string::npos);
    EXPECT_NE(output.find("keep-step"), std::string::npos);
    ASSERT_TRUE(edits.removeMark(imported.project, 0, 0));
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(imported.project, imported.source)), motrack::ProjectError);
    ASSERT_TRUE(edits.removeMark(imported.project, 0, 2));
    const auto empty = motrack::renderTrackerProject(imported.project, imported.source);
    EXPECT_TRUE(motrack::parseLegacyProject(empty).pointMasses.front().marks.empty());
    EXPECT_NE(empty.find("{}"), std::string::npos);

    imported = motrack::parseTrackerProject(std::string(sourceXml()));
    edits.clear();
    ASSERT_TRUE(edits.markPointMass(imported.project, 0, {.frame = 2, .x = 1.25, .y = 2.5}));
    const auto created = motrack::renderTrackerProject(imported.project, imported.source);
    EXPECT_EQ(motrack::parseLegacyProject(created).pointMasses.front().marks, imported.project.pointMasses.front().marks);
    EXPECT_NE(created.find("{2}"), std::string::npos);
}

TEST(TrackerWriter, TreatsAnEmptyKeyArrayAsManualMarksAndRejectsInvalidMarkData)
{
    auto imported = motrack::parseTrackerProject(markedXml("{}"));
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.markPointMass(imported.project, 0, {.frame = 3, .x = 1, .y = 2}));
    EXPECT_NE(motrack::renderTrackerProject(imported.project, imported.source).find("{0,2,3}"), std::string::npos);
    for (const auto* keys : {"0", "{oops}", "{-1}", "{0,,2}"})
    {
        imported = motrack::parseTrackerProject(markedXml(keys));
        imported.project.pointMasses.front().marks.front().x += 1;
        EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(imported.project, imported.source)), motrack::ProjectError);
    }
    imported = motrack::parseTrackerProject(markedXml());
    imported.project.pointMasses.front().marks.front().x = std::numeric_limits<double>::infinity();
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(imported.project, imported.source)), motrack::ProjectError);
    imported = motrack::parseTrackerProject(markedXml());
    std::ranges::reverse(imported.project.pointMasses.front().marks);
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(imported.project, imported.source)), motrack::ProjectError);
}

} // namespace
