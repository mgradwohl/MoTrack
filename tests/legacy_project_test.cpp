#include "legacy_project.hpp"

#include <QTemporaryFile>
#include <gtest/gtest.h>

namespace
{

std::string projectXml(std::string_view properties)
{
    return R"(<object class="org.opensourcephysics.cabrillo.tracker.TrackerPanel">)" + std::string(properties) + "</object>";
}

std::string projectWithClip(std::string_view properties)
{
    return projectXml("<property name='videoclip'><object>" + std::string(properties) + "</object></property>");
}

std::string projectWithMarks(std::string_view properties)
{
    return projectXml(R"(<property name="tracks"><property name="item">
        <object class="org.opensourcephysics.cabrillo.tracker.PointMass">
        <property name="framedata">)" +
                      std::string(properties) + "</property></object></property></property>");
}

TEST(LegacyProject, ImportsMetadataAndSparseMarks)
{
    const auto result = motrack::readLegacyProject(std::filesystem::path(MOTRACK_FIXTURES) / "point-mass.trk");
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
    EXPECT_THROW(motrack::parseLegacyProject("<!DOCTYPE object>" + projectXml("")), motrack::ProjectError);
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
    EXPECT_THROW(motrack::readLegacyProject(std::filesystem::path(file.fileName().toStdU16String())), motrack::ProjectError);
}

TEST(LegacyProject, ReadsCalibrationAndKeepsMarksWhenCalibrationIsUnsupported)
{
    const auto result = motrack::readLegacyProject(std::filesystem::path(MOTRACK_FIXTURES) / "image-sequence.trk");
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

} // namespace
