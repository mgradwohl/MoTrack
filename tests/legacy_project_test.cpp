#include "legacy_project.hpp"

#include "filesystem_path.hpp"
#include "measurements.hpp"
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
    // Duplicate indices resolve as Java's slots do (ReadsTrackArraysAsJavaAssignsTheirSlots); in an array Java cannot
    // place, MoTrack's file-order reading still refuses them.
    const auto duplicate = "<property name='[1]'>" + position + "</property>";
    EXPECT_NO_THROW(static_cast<void>(motrack::parseLegacyProject(projectWithMarks(duplicate + duplicate))));
    EXPECT_THROW(motrack::parseLegacyProject(projectWithMarks(duplicate + duplicate + "<property name='[0]'>" + position + "</property>")),
                 motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject(projectWithMarks("<property name='invalid'>" + position + "</property>")),
                 motrack::ProjectError);
    EXPECT_THROW(motrack::parseLegacyProject(projectWithMarks("<property name='[1]'><object/></property>")), motrack::ProjectError);
}

/// One `[n]` point-mass frame at image (x, y).
std::string massFrame(int index, double x, double y)
{
    return "<property name='[" + std::to_string(index) + "]'><object><property name='x'>" + std::to_string(x) +
           "</property><property name='y'>" + std::to_string(y) + "</property></object></property>";
}

std::string massTrack(std::string_view name, std::string_view frames)
{
    return "<property name='item'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'><property name='name'>" +
           std::string(name) + "</property><property name='framedata'>" + std::string(frames) + "</property></object></property>";
}

std::string coordKey(int index, double originX, double originY, double angle, double scaleX, double scaleY)
{
    return "<property name='[" + std::to_string(index) + "]'><object><property name='xorigin'>" + std::to_string(originX) +
           "</property><property name='yorigin'>" + std::to_string(originY) + "</property><property name='angle'>" + std::to_string(angle) +
           "</property><property name='xscale'>" + std::to_string(scaleX) + "</property><property name='yscale'>" + std::to_string(scaleY) +
           "</property></object></property>";
}

std::string coordSystem(std::string_view keys)
{
    return "<property name='coords'><object class='org.opensourcephysics.media.core.ImageCoordSystem'>"
           "<property name='fixedorigin' type='boolean'>false</property><property name='fixedangle' type='boolean'>false</property>"
           "<property name='fixedscale' type='boolean'>false</property><property name='framedata'>" +
           std::string(keys) + "</property></object></property>";
}

/// A media-free clip with six steps from `startframe` and the given `frameshift` property (none when empty).
std::string shiftedClip(std::string_view startFrame, std::string_view frameShift)
{
    return "<property name='videoclip'><object class='org.opensourcephysics.media.core.VideoClip'>" + std::string(startFrame) +
           "<property name='stepsize' type='int'>1</property><property name='stepcount' type='int'>6</property>" + std::string(frameShift) +
           "</object></property>";
}

std::vector<int> frames(const std::vector<motrack::Mark>& marks)
{
    std::vector<int> result;
    result.reserve(marks.size());
    for (const auto& mark : marks)
        result.push_back(mark.frame);
    return result;
}

TEST(LegacyProject, AppliesTheClipFrameShiftToEachTrackArrayAsTrackersLoaderDoes)
{
    // #313: TrackerIO.AsyncLoader.adjustFrameData removes the first `frameshift` slots of each track array of Java
    // length (last index + 1) above 1, and VideoClip.Loader subtracts the shift from the start frame. The coordinate
    // system is loaded without the adjuster. Saved index n of a mark is encoded as its x = 10n + 10.
    std::string longFrames;
    for (const auto index : {0, 1, 2, 3, 5, 7})
        longFrames += massFrame(index, 10.0 * index + 10, 20.0 + 5.0 * index);
    const auto project = motrack::parseLegacyProject(projectXml(
        shiftedClip("<property name='startframe' type='int'>3</property>", "<property name='frameshift' type='int'>2</property>") +
        coordSystem(coordKey(0, 320, 240, 0, 2, 4) + coordKey(4, 100, 120, 90, 4, 8)) + R"(<property name='tracks'>)" +
        massTrack("long", longFrames) + massTrack("single", massFrame(0, 10, 30)) + massTrack("short", massFrame(1, 40, 50)) +
        massTrack("pair", massFrame(0, 1, 1) + massFrame(1, 2, 2)) + massTrack("edge", massFrame(2, 30, 60)) +
        R"(<property name='item'><object class='org.opensourcephysics.cabrillo.tracker.Vector'><property name='framedata'>
          <property name='[1]'><object><property name='xtail'>30</property><property name='ytail'>60</property>
            <property name='xcomponent'>4</property><property name='ycomponent'>-8</property></object></property>
          <property name='[4]'><object><property name='xtail'>60</property><property name='ytail'>90</property>
            <property name='xcomponent'>2</property><property name='ycomponent'>8</property></object></property>
        </property></object></property></property>)"));
    EXPECT_EQ(project.clip.firstFrame, 1); // 3 - 2
    EXPECT_EQ(project.clip.stepSize, 1);
    EXPECT_EQ(project.clip.stepCount, 6);
    ASSERT_EQ(project.pointMasses.size(), 5u);
    // Length 8: saved [2], [3], [5], [7] load at frames 0, 1, 3, 5; [0] and [1] are dropped.
    const auto& shifted = project.pointMasses[0].marks;
    EXPECT_EQ(frames(shifted), (std::vector<int>{0, 1, 3, 5}));
    EXPECT_EQ(shifted.front(), (motrack::Mark{.frame = 0, .x = 30, .y = 30}));
    EXPECT_EQ(shifted.back(), (motrack::Mark{.frame = 5, .x = 80, .y = 55}));
    // Length 1 (only [0]) is unchanged; lengths 2 (not above the shift) load nothing; length 3 moves [2] to 0.
    EXPECT_EQ(project.pointMasses[1].marks, (std::vector<motrack::Mark>{{.frame = 0, .x = 10, .y = 30}}));
    EXPECT_TRUE(project.pointMasses[2].marks.empty());
    EXPECT_TRUE(project.pointMasses[3].marks.empty());
    EXPECT_EQ(project.pointMasses[4].marks, (std::vector<motrack::Mark>{{.frame = 0, .x = 30, .y = 60}}));
    ASSERT_EQ(project.vectors.size(), 1u);
    EXPECT_EQ(project.vectors.front().samples,
              (std::vector<motrack::VectorSample>{{.frame = 2, .tailX = 60, .tailY = 90, .componentX = 2, .componentY = 8}}));
    // Calibration keys keep their saved frames, so the mark from saved [5] (now frame 3) uses key 0 and the one from
    // saved [7] (now frame 5) uses key 4: origin (100, 120), 90 degrees, 4 and 8 pixels per metre.
    if (!project.calibration)
    {
        FAIL() << "The frame shift dropped the calibration.";
    }
    ASSERT_EQ(project.calibration->keyframes.size(), 2u);
    EXPECT_EQ(project.calibration->keyframes.front().frame, 0);
    EXPECT_EQ(project.calibration->keyframes.back().frame, 4);
    const auto atKeyZero = motrack::worldPosition(*project.calibration, shifted[2]);
    const auto atKeyFour = motrack::worldPosition(*project.calibration, shifted[3]);
    ASSERT_TRUE(atKeyZero && atKeyFour);
    EXPECT_NEAR(atKeyZero->x, (60.0 - 320.0) / 2.0, 1e-12);
    EXPECT_NEAR(atKeyZero->y, (240.0 - 45.0) / 4.0, 1e-12);
    // Unscaled (-20/4, -65/-8) = (-5, 8.125), then rotated by -90 degrees: (8.125, 5).
    EXPECT_NEAR(atKeyFour->x, 8.125, 1e-12);
    EXPECT_NEAR(atKeyFour->y, 5.0, 1e-12);
}

TEST(LegacyProject, LeavesFramesWhereTrackersLoaderIgnoresTheFrameShift)
{
    const auto marks = std::string("<property name='tracks'>") + massTrack("mass", massFrame(1, 1, 1) + massFrame(4, 4, 4)) + "</property>";
    const auto load = [&](std::string_view startFrame, std::string_view frameShift)
    {
        return motrack::parseLegacyProject(projectXml(shiftedClip(startFrame, frameShift) + marks));
    };
    const auto start = [](int frame)
    {
        return "<property name='startframe' type='int'>" + std::to_string(frame) + "</property>";
    };
    for (const auto* ignored : {"",
                                "<property name='frameshift' type='int'>0</property>",
                                "<property name='frameshift' type='double'>2.0</property>",
                                "<property name='frameshift' type='int'>two</property>",
                                "<property name='frameshift' type='int'>-2147483648</property>"})
    {
        const auto project = load(start(3), ignored);
        EXPECT_EQ(project.clip.firstFrame, 3) << ignored;
        EXPECT_EQ(frames(project.pointMasses.front().marks), (std::vector<int>{1, 4})) << ignored;
    }
    // A negative shift moves only the start frame: adjustFrameData shifts arrays by a positive offset only.
    const auto negative = load(start(3), "<property name='frameshift' type='int'>-2</property>");
    EXPECT_EQ(negative.clip.firstFrame, 5);
    EXPECT_EQ(frames(negative.pointMasses.front().marks), (std::vector<int>{1, 4}));
    // setStartFrameNumber keeps the start at frame 0 or later; Java's int subtraction wraps before that.
    const auto shift = std::string("<property name='frameshift' type='int'>2</property>");
    EXPECT_EQ(load(start(1), shift).clip.firstFrame, 0);
    EXPECT_EQ(load(start(2147483647), "<property name='frameshift' type='int'>-1</property>").clip.firstFrame, 0);
    // MoTrack policy, not Tracker's: without a saved start frame (Tracker always saves one) Java's wrapped sentinel is
    // clamped to the clip's last frame, while MoTrack keeps frame 0. The marks still shift.
    const auto unstarted = load("", shift);
    EXPECT_EQ(unstarted.clip.firstFrame, 0);
    EXPECT_EQ(frames(unstarted.pointMasses.front().marks), (std::vector<int>{2}));
    // Java cannot load an array whose last index is unreadable; MoTrack keeps it unshifted, as it read it before #313.
    const auto unreadable = motrack::parseLegacyProject(
        projectXml(shiftedClip(start(3), shift) + "<property name='tracks'>" +
                   massTrack("mass", massFrame(2, 2, 2) + "<property name='[x]'>null</property>") + "</property>"));
    EXPECT_EQ(frames(unreadable.pointMasses.front().marks), (std::vector<int>{2}));
}

TEST(LegacyProject, CountsATrailingNullElementInTheArrayLengthTheShiftUses)
{
    // A null [1] after [0] makes the Java array two slots long, so a shift of 1 drops [0]; alone, [0] is length 1 and stays.
    const auto load = [](std::string_view frames)
    {
        return motrack::parseLegacyProject(projectXml(
            shiftedClip("<property name='startframe' type='int'>3</property>", "<property name='frameshift' type='int'>1</property>") +
            "<property name='tracks'>" + massTrack("mass", frames) + "</property>"));
    };
    EXPECT_TRUE(load(massFrame(0, 1, 1) + "<property name='[1]'>null</property>").pointMasses.front().marks.empty());
    EXPECT_EQ(frames(load(massFrame(0, 1, 1)).pointMasses.front().marks), (std::vector<int>{0}));
}

TEST(LegacyProject, ReadsTrackArraysAsJavaAssignsTheirSlots)
{
    // OSP's array loader lets a later element replace an earlier one with the same index, a null one included.
    const auto load = [](std::string_view frames)
    {
        return motrack::parseLegacyProject(projectXml("<property name='tracks'>" + massTrack("mass", frames) + "</property>"));
    };
    const auto overwritten =
        load(massFrame(0, 0, 0) + massFrame(2, 2, 2) + "<property name='[2]'>null</property>" + massFrame(4, 4, 4)).pointMasses.front();
    EXPECT_EQ(frames(overwritten.marks), (std::vector<int>{0, 4}));
    const auto duplicate = load(massFrame(1, 10, 10) + massFrame(1, 11, 12)).pointMasses.front();
    EXPECT_EQ(duplicate.marks, (std::vector<motrack::Mark>{{.frame = 1, .x = 11, .y = 12}}));
    // A later object replaces an earlier null at the same index too.
    const auto revived = load("<property name='[2]'>null</property>" + massFrame(2, 5, 6)).pointMasses.front();
    EXPECT_EQ(revived.marks, (std::vector<motrack::Mark>{{.frame = 2, .x = 5, .y = 6}}));
    // Java places any index below the length the last element sets, in any order: [3] [1] [4] fill slots 1, 3 and 4.
    const auto unordered = load(massFrame(3, 3, 30) + massFrame(1, 1, 10) + massFrame(4, 4, 40)).pointMasses.front();
    EXPECT_EQ(unordered.marks,
              (std::vector<motrack::Mark>{{.frame = 1, .x = 1, .y = 10}, {.frame = 3, .x = 3, .y = 30}, {.frame = 4, .x = 4, .y = 40}}));
    // Java cannot place an out-of-order index; MoTrack keeps reading such an array in file order, as before (#315).
    EXPECT_EQ(frames(load(massFrame(4, 4, 4) + massFrame(2, 2, 2)).pointMasses.front().marks), (std::vector<int>{2, 4}));
    // Vectors follow the same slots.
    const auto vectors = motrack::parseLegacyProject(projectXml(R"(<property name='tracks'><property name='item'>
        <object class='org.opensourcephysics.cabrillo.tracker.Vector'><property name='framedata'>
          <property name='[3]'><object><property name='xtail'>1</property><property name='ytail'>2</property>
            <property name='xcomponent'>3</property><property name='ycomponent'>4</property></object></property>
          <property name='[3]'><object><property name='xtail'>5</property><property name='ytail'>6</property>
            <property name='xcomponent'>7</property><property name='ycomponent'>8</property></object></property>
        </property></object></property></property>)"));
    ASSERT_EQ(vectors.vectors.size(), 1u);
    EXPECT_EQ(vectors.vectors.front().samples,
              (std::vector<motrack::VectorSample>{{.frame = 3, .tailX = 5, .tailY = 6, .componentX = 7, .componentY = 8}}));
}

TEST(LegacyProject, KeepsTheLastOfDuplicateCalibrationKeysAsTrackerDoes)
{
    // #314: OSP's array loader lets a later element replace an earlier one with the same index, a null one included.
    const auto project = motrack::parseLegacyProject(
        projectXml(coordSystem(coordKey(0, 320, 240, 0, 2, 4) + coordKey(0, 100, 50, 0, 5, 10) + coordKey(3, 10, 10, 0, 8, 8) +
                               "<property name='[3]'>null</property>") +
                   "<property name='tracks'>" + massTrack("mass", massFrame(0, 10, 20) + massFrame(4, 50, 20)) + "</property>"));
    if (!project.calibration)
    {
        FAIL() << "Duplicate keys dropped the calibration.";
    }
    EXPECT_EQ(
        project.calibration->keyframes,
        (std::vector<motrack::CalibrationKey>{{.frame = 0, .originX = 100, .originY = 50, .angleDegrees = 0, .scaleX = 5, .scaleY = 10}}));
    const auto world = motrack::worldPosition(*project.calibration, project.pointMasses.front().marks.back());
    ASSERT_TRUE(world);
    EXPECT_NEAR(world->x, (50.0 - 100.0) / 5.0, 1e-12);
    EXPECT_NEAR(world->y, (50.0 - 20.0) / 10.0, 1e-12);
    // A single key is one slot of length 1 and stays where it is, frame shift or not.
    const auto single = motrack::parseLegacyProject(projectXml(
        shiftedClip("<property name='startframe' type='int'>3</property>", "<property name='frameshift' type='int'>2</property>") +
        coordSystem(coordKey(0, 320, 240, 0, 2, 4))));
    ASSERT_TRUE(single.calibration && single.calibration->keyframes.size() == 1u);
    EXPECT_EQ(single.calibration->keyframes.front().frame, 0);
    // An index Java's array loader cannot place (out of order or negative) leaves the project uncalibrated.
    for (const auto& keys : {coordKey(4, 1, 1, 0, 1, 1) + coordKey(0, 1, 1, 0, 1, 1), coordKey(-1, 1, 1, 0, 1, 1)})
    {
        EXPECT_FALSE(motrack::parseLegacyProject(projectXml(coordSystem(keys))).calibration.has_value());
    }
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
