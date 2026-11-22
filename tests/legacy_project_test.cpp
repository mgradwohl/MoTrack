#include "legacy_project.hpp"

#include "filesystem_path.hpp"
#include "osp_layout.hpp"
#include "tracker_writer.hpp"

#include <QFile>
#include <QString>
#include <QTemporaryFile>
#include <gtest/gtest.h>

#include <string_view>
#include <tuple>

namespace
{

/// A compact panel snippet in OSP's writer layout, as Tracker reads it.
std::string projectXml(std::string_view properties)
{
    return motrack::test::ospLayout(R"(<object class="org.opensourcephysics.cabrillo.tracker.TrackerPanel">)" + std::string(properties) +
                                    "</object>");
}

std::string projectWithClip(std::string_view properties)
{
    return projectXml("<property name='videoclip'><object>" + std::string(properties) + "</object></property>");
}

std::string projectWithMarks(std::string_view properties)
{
    return projectXml(R"(<property name="tracks"><property name="item">
        <object class="org.opensourcephysics.cabrillo.tracker.PointMass">
        <property name="framedata" type="array">)" +
                      std::string(properties) + "</property></object></property></property>");
}

TEST(LegacyProject, ImportsMetadataAndSparseMarks)
{
    const auto result = motrack::readLegacyProject(motrack::toFilesystemPath(QString::fromUtf8(MOTRACK_FIXTURES) + "/point-mass.trk"));
    EXPECT_EQ(result.version, "6.3.5");
    EXPECT_EQ(result.description, "Two marks & a gap.");
    EXPECT_EQ(result.clip.firstFrame, 2);
    EXPECT_EQ(result.clip.stepSize, 2);
    EXPECT_EQ(result.clip.stepCount, 3);
    ASSERT_EQ(result.clip.mediaReferences.size(), 1u);
    EXPECT_EQ(result.clip.mediaReferences[0], "media/ball.mp4");
    ASSERT_EQ(result.pointMasses.size(), 1u);
    EXPECT_EQ(result.pointMasses[0].name, "P\xC3\xA9ndulo");
    EXPECT_EQ(result.massUnit, "g");
    EXPECT_DOUBLE_EQ(result.pointMasses[0].mass, 125.5);
    const auto& marks = result.pointMasses[0].marks;
    ASSERT_EQ(marks.size(), 2u);
    EXPECT_EQ(marks[0].frame, 2);
    EXPECT_DOUBLE_EQ(marks[0].x, 120.5);
    EXPECT_DOUBLE_EQ(marks[0].y, 85.25);
    EXPECT_EQ(marks[1].frame, 6);
    EXPECT_EQ(result.unsupportedTrackTypes, std::vector<std::string>{"org.opensourcephysics.cabrillo.tracker.CoordAxes"});
}

TEST(LegacyProject, RetainsTimeLabelsAndDefaultsMissingOrEmptyLabels)
{
    EXPECT_EQ(motrack::parseLegacyProject(projectXml("")).timeUnit, "s");
    EXPECT_EQ(motrack::parseLegacyProject(projectXml("<property name='time_unit' type='string'></property>")).timeUnit, "s");
    EXPECT_EQ(
        motrack::parseLegacyProject(projectXml("<property name='time_unit' type='string'><![CDATA[tick & beat]]></property>")).timeUnit,
        "tick & beat");
}

TEST(LegacyProject, AssignsDistinctIdentitiesToImportedTracksWithDuplicateNames)
{
    const auto project = motrack::parseLegacyProject(projectXml(R"(
      <property name="tracks">
        <property name="[0]"><object class="org.opensourcephysics.cabrillo.tracker.PointMass">
          <property name="name">same</property></object></property>
        <property name="[1]"><object class="UnknownTrack"><property name="name">same</property></object></property>
        <property name="[2]"><object class="org.opensourcephysics.cabrillo.tracker.Vector">
          <property name="name">same</property></object></property>
        <property name="[3]"><object class="org.opensourcephysics.cabrillo.tracker.PointMass">
          <property name="name">same</property></object></property>
      </property>)"));
    ASSERT_EQ(project.pointMasses.size(), 2u);
    ASSERT_EQ(project.vectors.size(), 1u);
    EXPECT_GT(project.pointMasses.front().id, 0u);
    EXPECT_NE(project.pointMasses.front().id, project.pointMasses.back().id);
    EXPECT_NE(project.pointMasses.front().id, project.vectors.front().id);
    EXPECT_NE(project.pointMasses.back().id, project.vectors.front().id);
    EXPECT_GT(project.nextTrackId, project.pointMasses.back().id);
    EXPECT_GT(project.nextTrackId, project.vectors.front().id);
    EXPECT_EQ(project.unsupportedTrackTypes, std::vector<std::string>{"UnknownTrack"});
}

TEST(LegacyProject, ImportsSparseVectorComponentsInImageCoordinates)
{
    const auto project = motrack::parseLegacyProject(projectXml(R"(
      <property name="tracks"><property name="item">
        <object class="org.opensourcephysics.cabrillo.tracker.Vector">
          <property name="name">Displacement</property>
          <property name="framedata">
            <property name="[7]"><object>
              <property name="xtail">2.5</property><property name="ytail">3</property>
              <property name="xcomponent">4</property><property name="ycomponent">-1</property>
            </object></property>
            <property name="[0]">null</property>
          </property>
        </object>
      </property></property>)"));
    EXPECT_TRUE(project.unsupportedTrackTypes.empty());
    ASSERT_EQ(project.vectors.size(), 1u);
    const auto& track = project.vectors.front();
    EXPECT_EQ(track.name, "Displacement");
    ASSERT_EQ(track.samples.size(), 1u);
    EXPECT_EQ(track.samples.front().frame, 7);
    EXPECT_DOUBLE_EQ(track.samples.front().tailX, 2.5);
    EXPECT_DOUBLE_EQ(track.samples.front().componentY, -1.0);
}

TEST(LegacyProject, KeepsOtherTracksWhenAVectorOmitsComponents)
{
    const auto project = motrack::parseLegacyProject(projectXml(R"(
      <property name="tracks">
        <property name="item"><object class="org.opensourcephysics.cabrillo.tracker.PointMass"/></property>
        <property name="item"><object class="org.opensourcephysics.cabrillo.tracker.Vector">
          <property name="framedata"><property name="[0]"><object>
            <property name="xtail">1</property><property name="ytail">2</property>
          </object></property></property>
        </object></property>
      </property>)"));
    EXPECT_EQ(project.pointMasses.size(), 1u);
    EXPECT_TRUE(project.vectors.empty());
    EXPECT_EQ(project.unsupportedTrackTypes, std::vector<std::string>{"org.opensourcephysics.cabrillo.tracker.Vector"});
}

TEST(LegacyProject, OpensBlankSession)
{
    const auto result = motrack::parseLegacyProject(projectXml(""));
    EXPECT_EQ(result.clip.firstFrame, 0);
    EXPECT_EQ(result.clip.stepSize, 1);
    EXPECT_FALSE(result.clip.stepCount.has_value());
    EXPECT_TRUE(result.clip.mediaReferences.empty());
    EXPECT_TRUE(result.pointMasses.empty());
}

TEST(LegacyProject, KeepsImageSequenceInArrayOrder)
{
    const auto result = motrack::parseLegacyProject(projectWithClip(R"(
        <property name="video"><object>
          <property name="paths">
            <property name="[1]">legacy.trz!/videos/ball01.jpg</property>
            <property name="[0]">legacy.trz!/videos/ball00.jpg</property>
          </property>
          <property name="path">fallback.jpg</property>
        </object></property>)"));
    EXPECT_EQ(result.clip.mediaReferences, (std::vector<std::string>{"legacy.trz!/videos/ball00.jpg", "legacy.trz!/videos/ball01.jpg"}));
}

TEST(LegacyProject, RejectsInvalidInput)
{
    EXPECT_THROW(motrack::readLegacyProject("motrack-nonexistent-project.trk"), motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject("<object"), motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject("<object class='java.lang.String'/>"), motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject(std::string(32 * 1024 * 1024 + 1, ' ')), motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject(projectWithClip("<property name='stepsize'>0</property>")), motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject(projectWithClip(R"(
        <property name="video"><object><property name="paths">
          <property name="[1]">missing-zero.jpg</property>
        </property></object></property>)")),
                 motrack::ProjectError);
}

TEST(LegacyProject, RejectsOversizedProjectFilesBeforeReading)
{
    QTemporaryFile file;
    ASSERT_TRUE(file.open());
    ASSERT_TRUE(file.resize(32LL * 1024 * 1024 + 1));
    file.close();
    EXPECT_THROW(motrack::readLegacyProject(motrack::toFilesystemPath(file.fileName())), motrack::ProjectError);
}

TEST(LegacyProject, RefusesWindows1252BytesWhateverTheDeclaration)
{
    // Tracker reads every XMLControl file as UTF-8, whatever it declares, and silently turns invalid bytes into
    // U+FFFD; MoTrack never substitutes silently. Windows-1252 bytes are refused under a UTF-8 or an ISO-8859-1
    // declaration, and the refusal names Windows-1252 (#277); opening them with a warning is #302 (CONTRIBUTING.md,
    // Tracker compatibility exceptions). ReadsDocumentsWithTrackersLineOrientedReader covers undefined bytes.
    QFile input(QString::fromUtf8(MOTRACK_FIXTURES) + "/point-mass.trk");
    ASSERT_TRUE(input.open(QIODevice::ReadOnly));
    const auto text = QString::fromUtf8(input.readAll());
    ASSERT_TRUE(text.contains(QString::fromUtf8("P\xC3\xA9ndulo")) && text.contains(R"(encoding="UTF-8")"));
    for (const auto* declaration : {R"(encoding="UTF-8")", R"(encoding="ISO-8859-1")"})
    {
        SCOPED_TRACE(declaration);
        auto declared = text;
        declared.replace(R"(encoding="UTF-8")", QString::fromLatin1(declaration));
        // e acute is 0xE9 in both Windows-1252 and ISO-8859-1.
        try
        {
            std::ignore = motrack::parseTrackerProject(declared.toLatin1().toStdString());
            ADD_FAILURE() << "accepted";
        }
        catch (const motrack::ProjectError& error)
        {
            EXPECT_NE(std::string_view(error.what()).find("It reads as Windows-1252 text"), std::string_view::npos) << error.what();
        }
    }
}

TEST(LegacyProject, ReadsCalibrationAndKeepsMarksWhenCalibrationIsUnsupported)
{
    const auto result = motrack::readLegacyProject(motrack::toFilesystemPath(QString::fromUtf8(MOTRACK_FIXTURES) + "/image-sequence.trk"));
    EXPECT_EQ(result.lengthUnit, "m");
    EXPECT_EQ(result.clip.frameDurationMs, 250.0);
    EXPECT_DOUBLE_EQ(result.clip.startTimeMs, -500.0);
    if (!result.calibration)
    {
        FAIL() << "Missing saved image calibration.";
    }
    EXPECT_TRUE(result.calibration->fixedOrigin);
    EXPECT_TRUE(result.calibration->fixedAngle);
    EXPECT_TRUE(result.calibration->fixedScale);
    ASSERT_EQ(result.calibration->keyframes.size(), 1u);
    EXPECT_DOUBLE_EQ(result.calibration->keyframes.front().scaleX, 2);
    EXPECT_DOUBLE_EQ(result.calibration->keyframes.front().scaleY, 4);

    const auto malformed = motrack::parseLegacyProject(projectXml(R"(
        <property name="coords"><object class="org.opensourcephysics.media.core.ImageCoordSystem">
          <property name="framedata"><property name="[0]"><object/></property></property>
        </object></property>)"));
    EXPECT_FALSE(malformed.calibration.has_value());
    const auto empty = motrack::parseLegacyProject(projectXml(R"(
        <property name="coords"><object class="org.opensourcephysics.media.core.ImageCoordSystem"/></property>)"));
    EXPECT_FALSE(empty.calibration.has_value());
}

TEST(LegacyProject, RejectsInvalidMarks)
{
    const std::string position = "<object><property name='x'>1</property>"
                                 "<property name='y'>2</property></object>";
    const auto duplicate = "<property name='[1]'>" + position + "</property>";
    EXPECT_THROW(motrack::parseLegacyProject(projectWithMarks(duplicate + duplicate)), motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject(projectWithMarks("<property name='invalid'>" + position + "</property>")),
                 motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject(projectWithMarks("<property name='[1]'><object/></property>")), motrack::ProjectError);
}

std::string fixtureText(std::string_view name)
{
    QFile file(QString::fromUtf8(MOTRACK_FIXTURES) + '/' + QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
    return file.open(QIODevice::ReadOnly) ? file.readAll().toStdString() : std::string{};
}

std::string replaced(std::string text, std::string_view from, std::string_view to)
{
    const auto position = text.find(from);
    EXPECT_NE(position, std::string::npos) << from;
    return position == std::string::npos ? text : text.replace(position, from.size(), to);
}

TEST(LegacyProject, ReadsDocumentsWithTrackersLineOrientedReader)
{
    // #196: OSP never decodes entities, and a blank line ends an array, so the following null element
    // becomes a null object directly in the PointMass, which makes Tracker refuse the whole file.
    const auto source = fixtureText("point-mass.trk");
    const auto entity = motrack::parseLegacyProject(replaced(source, "P\xC3\xA9ndulo", "Ball &amp; spring"));
    ASSERT_EQ(entity.pointMasses.size(), 1u);
    EXPECT_EQ(entity.pointMasses.front().name, "Ball &amp; spring");
    try
    {
        static_cast<void>(motrack::parseLegacyProject(replaced(source,
                                                               R"(          <property name="[4]" type="object">null</property>)",
                                                               "\n"
                                                               R"(          <property name="[4]" type="object">null</property>)")));
        ADD_FAILURE() << "Tracker refuses a null object directly in the PointMass.";
    }
    catch (const motrack::ProjectError& error)
    {
        EXPECT_EQ(std::string_view(error.what()), "Invalid Tracker XML at line 34: A null object property cannot belong to an object.");
    }

    // A document type is inert text to Tracker: internal entities are neither declared nor expanded.
    const auto declared = motrack::parseLegacyProject("<!DOCTYPE object [<!ENTITY note \"expanded\">]>\n" +
                                                      replaced(source, "<![CDATA[Two marks & a gap.]]>", "&note;")
                                                          .substr(std::string_view("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n").size()));
    EXPECT_EQ(declared.description, "&note;");
    EXPECT_EQ(declared.pointMasses.size(), 1u);

    // The lookup returns the last of duplicate properties, as XMLControlElement does.
    const auto duplicate = motrack::parseLegacyProject(replaced(source,
                                                                R"(  <property name="mass_unit" type="string">g</property>)",
                                                                R"(  <property name="mass_unit" type="string">g</property>)"
                                                                "\n"
                                                                R"(  <property name="mass_unit" type="string">mg</property>)"));
    EXPECT_EQ(duplicate.massUnit, "mg");

    // #277: Tracker's Export can write Windows-1252 under a UTF-8 declaration. The strict reader refuses it and
    // names the likely encoding (#302 tracks opening it with a warning); other invalid bytes are refused plainly.
    const auto refusal = [](const std::string& xml)
    {
        try
        {
            static_cast<void>(motrack::parseLegacyProject(xml));
        }
        catch (const motrack::ProjectError& error)
        {
            return std::string(error.what());
        }
        return std::string("accepted");
    };
    const auto windows1252 = refusal(replaced(source, "P\xC3\xA9ndulo", "P\xE9ndulo"));
    EXPECT_TRUE(windows1252.starts_with("Invalid Tracker XML at line ")) << windows1252;
    EXPECT_NE(windows1252.find("The document is not valid UTF-8. It reads as Windows-1252 text"), std::string::npos) << windows1252;
    const auto undecodable = refusal(replaced(source, "P\xC3\xA9ndulo", "P\xE9\x81ndulo"));
    EXPECT_TRUE(undecodable.ends_with(": The document is not valid UTF-8.")) << undecodable;

    // Content after the root's opening line on the same line is not read, so the root never closes: Tracker
    // loads an empty panel from such a truncated document, which MoTrack refuses (#243).
    EXPECT_THROW(static_cast<void>(motrack::parseLegacyProject(R"(<object class="org.opensourcephysics.cabrillo.tracker.TrackerPanel">)"
                                                               R"(<property name="description" type="string">hidden</property></object>)")),
                 motrack::ProjectError);
}

TEST(LegacyProject, AppliesTrackerLoaderDefaultsForUnsavedClipControlAndLengthUnit)
{
    // #214: a media-free panel keeps StepperClipControl's 100 ms and Tracker's preferred "m" unless saved.
    const auto blank = motrack::parseLegacyProject(projectXml(""));
    EXPECT_EQ(blank.lengthUnit, "m");
    EXPECT_EQ(blank.clip.frameDurationMs, 100.0);
    const auto control = [](std::string_view delta)
    {
        return projectXml(
            R"(<property name="clipcontrol" type="object"><object class="org.opensourcephysics.media.core.StepperClipControl">)"
            R"(<property name="rate" type="double">1.0</property>)" +
            std::string(delta) + "</object></property>");
    };
    EXPECT_EQ(motrack::parseLegacyProject(control(R"(<property name="delta_t" type="double">40.0</property>)")).clip.frameDurationMs, 40.0);
    // StepperClipControl.setFrameDuration ignores zero, keeping the default.
    EXPECT_EQ(motrack::parseLegacyProject(control(R"(<property name="delta_t" type="double">0.0</property>)")).clip.frameDurationMs, 100.0);
    // A saved control without delta_t gives Tracker NaN (#292); MoTrack does not substitute a value.
    EXPECT_FALSE(motrack::parseLegacyProject(control("")).clip.frameDurationMs.has_value());
    // Likewise a point mass without "mass" is NaN in Tracker (#292) and keeps MoTrack's 1 kg.
    const auto massless = motrack::parseLegacyProject(
        replaced(fixtureText("point-mass.trk"), "        <property name=\"mass\" type=\"double\">125.5</property>\n", ""));
    ASSERT_EQ(massless.pointMasses.size(), 1u);
    EXPECT_EQ(massless.pointMasses.front().mass, 1.0);
    EXPECT_EQ(motrack::parseLegacyProject(projectXml(R"(<property name="length_unit" type="string"></property>)")).lengthUnit, "");
    // Projects with media take their timing from the video and its control, not the media-free default.
    const auto media = motrack::readLegacyProject(motrack::toFilesystemPath(QString::fromUtf8(MOTRACK_FIXTURES) + "/point-mass.trk"));
    EXPECT_FALSE(media.clip.frameDurationMs.has_value());
    EXPECT_EQ(media.lengthUnit, "m");

    // The defaults are loader state: an export keeps the absent properties absent.
    auto imported = motrack::parseTrackerProject(projectXml(""));
    EXPECT_EQ(motrack::renderTrackerProject(imported.project, imported.source), imported.source.xml);
    imported.project.massUnit = "g";
    const auto exported = motrack::renderTrackerProject(imported.project, imported.source);
    EXPECT_EQ(exported.find("length_unit"), std::string::npos);
    EXPECT_EQ(exported.find("clipcontrol"), std::string::npos);
    const auto reopened = motrack::parseLegacyProject(exported);
    EXPECT_EQ(reopened.massUnit, "g");
    EXPECT_EQ(reopened.lengthUnit, "m");
    EXPECT_EQ(reopened.clip.frameDurationMs, 100.0);
}

} // namespace
