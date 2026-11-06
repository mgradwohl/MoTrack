#include "legacy_project.hpp"

#include <gtest/gtest.h>

namespace {

std::string projectXml(std::string_view properties)
{
    return R"(<object class="org.opensourcephysics.cabrillo.tracker.TrackerPanel">)"
        + std::string(properties) + "</object>";
}

std::string projectWithClip(std::string_view properties)
{
    return projectXml("<property name='videoclip'><object>" + std::string(properties)
                      + "</object></property>");
}

std::string projectWithMarks(std::string_view properties)
{
    return projectXml(R"(<property name="tracks"><property name="item">
        <object class="org.opensourcephysics.cabrillo.tracker.PointMass">
        <property name="framedata">)" + std::string(properties)
        + "</property></object></property></property>");
}

TEST(LegacyProject, ImportsMetadataAndSparseMarks)
{
    const auto result = motrack::readLegacyProject(
        std::filesystem::path(MOTRACK_FIXTURES) / "point-mass.trk");
    EXPECT_EQ(result.version, "6.3.5");
    EXPECT_EQ(result.description, "Two marks & a gap.");
    EXPECT_EQ(result.clip.firstFrame, 2);
    EXPECT_EQ(result.clip.stepSize, 2);
    EXPECT_EQ(result.clip.stepCount, 3);
    ASSERT_EQ(result.clip.mediaReferences.size(), 1u);
    EXPECT_EQ(result.clip.mediaReferences[0], "media/ball.mp4");
    ASSERT_EQ(result.pointMasses.size(), 1u);
    EXPECT_EQ(result.pointMasses[0].name, "P\xC3\xA9ndulo");
    const auto& marks = result.pointMasses[0].marks;
    ASSERT_EQ(marks.size(), 2u);
    EXPECT_EQ(marks[0].frame, 2);
    EXPECT_DOUBLE_EQ(marks[0].x, 120.5);
    EXPECT_DOUBLE_EQ(marks[0].y, 85.25);
    EXPECT_EQ(marks[1].frame, 6);
    EXPECT_EQ(result.unsupportedTrackTypes,
              std::vector<std::string>{"org.opensourcephysics.cabrillo.tracker.CoordAxes"});
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
    EXPECT_EQ(result.clip.mediaReferences,
              (std::vector<std::string>{"legacy.trz!/videos/ball00.jpg",
                                        "legacy.trz!/videos/ball01.jpg"}));
}

TEST(LegacyProject, RejectsInvalidInput)
{
    EXPECT_THROW(motrack::readLegacyProject("motrack-nonexistent-project.trk"),
                 motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject("<object"), motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject("<object class='java.lang.String'/>"),
                 motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject("<!DOCTYPE object>" + projectXml("")),
                 motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject(std::string(32 * 1024 * 1024 + 1, ' ')),
                 motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject(projectWithClip("<property name='stepsize'>0</property>")),
                 motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject(projectWithClip(R"(
        <property name="video"><object><property name="paths">
          <property name="[1]">missing-zero.jpg</property>
        </property></object></property>)")), motrack::ProjectError);
}

TEST(LegacyProject, RejectsInvalidMarks)
{
    const std::string position = "<object><property name='x'>1</property>"
                                 "<property name='y'>2</property></object>";
    const auto duplicate = "<property name='[1]'>" + position + "</property>";
    EXPECT_THROW(motrack::parseLegacyProject(projectWithMarks(duplicate + duplicate)),
                 motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject(projectWithMarks(
                     "<property name='invalid'>" + position + "</property>")),
                 motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject(projectWithMarks(
                     "<property name='[1]'><object/></property>")), motrack::ProjectError);
}

} // namespace
