#include "tracker_writer.hpp"

#include "track_edits.hpp"
#include "workspace_document.hpp"

#include <QFile>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
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

TEST(TrackerWriter, PreservesUntouchedScalarMixedAndExplicitWhitespace)
{
    std::string xml(sourceXml());
    const auto position = xml.find("<property name='future'>");
    ASSERT_NE(position, std::string::npos);
    xml.insert(position,
               "<property name='preserved'><object xml:space='preserve'><property name='nested'>value</property></object></property>"
               "<property name='comment-first'><!-- keep -->  keep  </property>"
               "<property name='mixed'> before <object/> after </property>"
               "<property name='carriage' custom='left&#13;right'>left&#13;right</property>");
    auto imported = motrack::parseTrackerProject(xml);
    imported.project.pointMasses.front().mass = 2.5;
    QXmlStreamReader result(QByteArray::fromStdString(motrack::renderTrackerProject(imported.project, imported.source)));
    ASSERT_TRUE(result.readNextStartElement());
    std::map<QString, QString> properties;
    QString custom;
    while (result.readNextStartElement())
    {
        const auto name = result.attributes().value("name").toString();
        if (name == "carriage")
        {
            custom = result.attributes().value("custom").toString();
        }
        properties.emplace(name, result.readElementText(QXmlStreamReader::IncludeChildElements));
    }
    EXPECT_FALSE(result.hasError());
    EXPECT_EQ(properties.at("preserved"), "value");
    EXPECT_EQ(properties.at("comment-first"), "  keep  ");
    EXPECT_EQ(properties.at("mixed"), " before  after ");
    EXPECT_EQ(properties.at("carriage"), "left\rright");
    EXPECT_EQ(custom, "left\rright");
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
    try
    {
        motrack::writeTrackerProject(temporary.filePath("missing/child.trk"), imported.project, imported.source);
        FAIL() << "Expected a file error for the missing parent directory.";
    }
    catch (const motrack::ProjectError& error)
    {
        EXPECT_TRUE(std::string_view(error.what()).starts_with("Could not write Tracker project:"));
    }
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
    const auto keyValue = keys.empty() ? std::string{} : "<property name='array' type='string'>" + keys + "</property>";
    return "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'><property name='tracks'>"
           "<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>"
           "<property name='name' type='string'>mass</property>"
           "<property name='framedata' type='array' class='[Lorg.opensourcephysics.cabrillo.tracker.PointMass$FrameData;'>"
           "<property name='[0]' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass$FrameData'>"
           "<property name='x' type='double'>1</property><property name='y' type='double'>2</property></object></property>"
           "<property name='[2]' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass$FrameData'>"
           "<property name='x' type='double'>3</property><property name='y' type='double'>4</property>"
           "<property name='future'>keep-step</property></object></property>"
           "<property name='[3]' type='object'>null</property>"
           "</property><property name='keyFrames' type='array' class='[I'>" +
           keyValue + "</property></object></property></property></object>";
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
    EXPECT_EQ(empty.find("{}"), std::string::npos);
    auto emptyImport = motrack::parseTrackerProject(empty);
    edits.clear();
    ASSERT_TRUE(edits.markPointMass(emptyImport.project, 0, {.frame = 4, .x = 11.25, .y = 22.5}));
    EXPECT_NE(motrack::renderTrackerProject(emptyImport.project, emptyImport.source).find("{4}"), std::string::npos);

    imported = motrack::parseTrackerProject(std::string(sourceXml()));
    edits.clear();
    ASSERT_TRUE(edits.markPointMass(imported.project, 0, {.frame = 2, .x = 1.25, .y = 2.5}));
    const auto created = motrack::renderTrackerProject(imported.project, imported.source);
    EXPECT_EQ(motrack::parseLegacyProject(created).pointMasses.front().marks, imported.project.pointMasses.front().marks);
    EXPECT_NE(created.find("{2}"), std::string::npos);
}

TEST(TrackerWriter, TreatsAnEmptyKeyArrayAsManualMarksAndRejectsInvalidMarkData)
{
    auto imported = motrack::parseTrackerProject(markedXml(""));
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.markPointMass(imported.project, 0, {.frame = 3, .x = 1, .y = 2}));
    EXPECT_NE(motrack::renderTrackerProject(imported.project, imported.source).find("{0,2,3}"), std::string::npos);
    for (const auto* keys : {"0", "{oops}", "{-1}", "{0,,2}", "{}"})
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

TEST(TrackerWriter, RejectsFrameFormattingThatCannotPreserveContentAndOSPReading)
{
    for (const auto& [before, after] : std::vector<std::pair<std::string, std::string>>{
             {"name='framedata'", "xml:space='preserve' name='framedata'"},
             {"<property name='[0]'", "<!-- retained but not readable inside an OSP array --><property name='[0]'"},
             {">null</property>", ">null<!-- retained placeholder --></property>"}})
    {
        auto xml = markedXml();
        xml.replace(xml.find(before), before.size(), after);
        auto imported = motrack::parseTrackerProject(xml);
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.markPointMass(imported.project, 0, {.frame = 3, .x = 1, .y = 2}));
        EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(imported.project, imported.source)), motrack::ProjectError);
    }
}

TEST(TrackerWriter, ExportsBoundVectorEditsAfterNativeReopenAndUndoRedo)
{
    auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk");
    const auto position = xml.find("<property name=\"xtail\"");
    ASSERT_NE(position, std::string::npos);
    xml.insert(position, "<property name='future' type='string'><!-- retained -->  keep  </property>\n");
    auto imported = motrack::parseTrackerProject(xml);
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.markVector(document.project, 0, {.frame = 0, .tailX = 22.5, .tailY = 16.75, .componentX = -6.5, .componentY = 8.25}));
    ASSERT_TRUE(edits.markVector(document.project, 0, {.frame = 3, .tailX = 100.125, .tailY = 200.5, .componentX = 0, .componentY = -2.5}));
    ASSERT_TRUE(edits.removeVectorSample(document.project, 0, 5));
    ASSERT_TRUE(edits.removeVectorSample(document.project, 1, 2));
    ASSERT_TRUE(edits.markVector(document.project, 1, {.frame = 7, .tailX = 50, .tailY = 60, .componentX = -10, .componentY = 20}));
    const auto output = motrack::renderTrackerProject(document.project, *document.trackerSource);
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    EXPECT_EQ(motrack::renderTrackerProject(document.project, *document.trackerSource), output);
    const auto expected = document.project.vectors;
    std::ranges::reverse(document.project.vectors);
    QTemporaryDir temporary;
    const auto native = temporary.filePath("vectors.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = motrack::readWorkspaceDocument(native);
    if (!document.trackerSource)
    {
        FAIL() << "Expected preserved Tracker source.";
    }
    const auto path = temporary.filePath("vectors.trk");
    motrack::writeTrackerProject(path, document.project, *document.trackerSource);
    const auto saved = readBytes(path);
    EXPECT_EQ(motrack::parseLegacyProject(saved).vectors, expected);
    EXPECT_NE(saved.find("<!-- retained -->  keep  "), std::string::npos);
    EXPECT_EQ(document.trackerSource->xml, imported.source.xml);
}

TEST(TrackerWriter, ExportsEmptyVectorsAndTheirFirstNewSample)
{
    auto imported =
        motrack::parseTrackerProject(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk"));
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.removeVectorSample(imported.project, 0, 0));
    ASSERT_TRUE(edits.removeVectorSample(imported.project, 0, 5));
    const auto empty = motrack::renderTrackerProject(imported.project, imported.source);
    imported = motrack::parseTrackerProject(empty);
    EXPECT_TRUE(imported.project.vectors.front().samples.empty());
    edits.clear();
    ASSERT_TRUE(edits.markVector(imported.project, 0, {.frame = 4, .tailX = 11.25, .tailY = 22.5, .componentX = 0, .componentY = 0}));
    EXPECT_EQ(motrack::parseLegacyProject(motrack::renderTrackerProject(imported.project, imported.source)).vectors,
              imported.project.vectors);
    imported.project.vectors.front().samples.front().componentX = std::numeric_limits<double>::infinity();
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(imported.project, imported.source)), motrack::ProjectError);
}

TEST(TrackerWriter, RejectsLossyVectorReconstructionAfterNativeReopen)
{
    const auto imported =
        motrack::parseTrackerProject(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk"));
    QTemporaryDir temporary;
    const auto native = temporary.filePath("overflow.motrack");
    const auto output = temporary.filePath("existing.trk");
    for (const auto& sample :
         std::vector<motrack::VectorSample>{{.frame = 0, .tailX = 1e308, .tailY = 0, .componentX = 1e308, .componentY = 0},
                                            {.frame = 0, .tailX = 0, .tailY = -1e308, .componentX = 0, .componentY = -1e308},
                                            {.frame = 0, .tailX = 1e16, .tailY = 0, .componentX = 1, .componentY = 0},
                                            {.frame = 0, .tailX = 0, .tailY = -1e16, .componentX = 0, .componentY = -1}})
    {
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        document.project.vectors.front().samples.front() = sample;
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Expected preserved Tracker source.";
        }
        motrack::writeTrackerProject(output, imported.project, imported.source);
        EXPECT_THROW(motrack::writeTrackerProject(output, document.project, *document.trackerSource), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), imported.source.xml);
    }
    auto project = imported.project;
    project.vectors.front().samples.front() = {.frame = 0, .tailX = 1000, .tailY = 1e12, .componentX = 0.1, .componentY = 1e10 + 0.1};
    EXPECT_NO_THROW(motrack::writeTrackerProject(output, project, imported.source));
    EXPECT_EQ(motrack::parseLegacyProject(readBytes(output)).vectors, project.vectors);
}

TEST(TrackerWriter, RejectsOriginAndVectorSnappingWithoutReplacingTheDestination)
{
    const auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk");
    const auto imported = motrack::parseTrackerProject(xml);
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(output, imported.project, imported.source);
    for (const auto& sample :
         std::vector<motrack::VectorSample>{{.frame = 0, .tailX = 321, .tailY = 240, .componentX = 3, .componentY = -4},
                                            {.frame = 0, .tailX = 320, .tailY = 240, .componentX = 3, .componentY = -4},
                                            {.frame = 0, .tailX = 51, .tailY = 60, .componentX = 3, .componentY = -4}})
    {
        auto project = imported.project;
        project.vectors.front().samples.front() = sample;
        EXPECT_THROW(motrack::writeTrackerProject(output, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), xml);
    }
    auto project = imported.project;
    project.vectors.front().samples.front().tailX = 320;
    project.vectors.front().samples.front().tailY = 249;
    EXPECT_NO_THROW(static_cast<void>(motrack::renderTrackerProject(project, imported.source))); // Outside the 8-pixel radius.
    project.calibration.reset();
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(project, imported.source)), motrack::ProjectError);
    project = imported.project;
    project.vectors.front().samples.front().tailX += 1;
    project.unsupportedTrackTypes.emplace_back("FutureVectorProvider");
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(project, imported.source)), motrack::ProjectError);
    auto styled = xml;
    const std::string footprint = "Footprint.BoldArrow";
    styled.replace(styled.find(footprint), footprint.size(), "Footprint.BoldArrow#2");
    auto custom = motrack::parseTrackerProject(styled);
    custom.project.vectors.front().samples.front().tailX += 1;
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(custom.project, custom.source)), motrack::ProjectError);
    project = imported.project;
    project.vectors.front().samples.resize(4097);
    for (std::size_t index = 0; index < project.vectors.front().samples.size(); ++index)
    {
        project.vectors.front().samples[index] = {.frame = static_cast<int>(index),
                                                  .tailX = 1000 + 20.0 * static_cast<double>(index),
                                                  .tailY = 1000,
                                                  .componentX = 1,
                                                  .componentY = 2};
    }
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(project, imported.source)), motrack::ProjectError);
}

TEST(TrackerWriter, BoundsDenseVectorFrameArraysBeforeReplacingTheDestination)
{
    const auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk");
    const auto imported = motrack::parseTrackerProject(xml);
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(output, imported.project, imported.source);
    for (const auto frame : {500001, std::numeric_limits<int>::max()})
    {
        auto project = imported.project;
        project.vectors.front().samples.back().frame = frame;
        EXPECT_THROW(motrack::writeTrackerProject(output, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), xml);
    }
    auto project = imported.project;
    project.vectors.front().samples.back().frame = 250000;
    project.vectors.back().samples.back().frame = 250000;
    EXPECT_THROW(motrack::writeTrackerProject(output, project, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(output), xml);
    project.vectors.back().samples.back().frame = 249999;
    EXPECT_NO_THROW(static_cast<void>(motrack::renderTrackerProject(project, imported.source)));

    // A retained null placeholder also sizes Tracker's dense array, but is absent from our samples.
    auto padded = xml;
    const std::string placeholder = R"(name="[9]" type="object">null)";
    const auto position = padded.find(placeholder);
    ASSERT_NE(position, std::string::npos);
    padded.replace(position, placeholder.size(), R"(name="[2147483647]" type="object">null)");
    auto sparse = motrack::parseTrackerProject(padded);
    sparse.project.vectors.front().samples.front().tailX += 1;
    EXPECT_THROW(motrack::writeTrackerProject(output, sparse.project, sparse.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(output), xml);
}

} // namespace
