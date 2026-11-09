#include "tracker_writer.hpp"

#include "local_media_path.hpp"
#include "measurements.hpp"
#include "media_vectors.hpp"
#include "movie_authoring.hpp"
#include "track_edits.hpp"
#include "tracker_references.hpp"
#include "workspace_document.hpp"

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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

motrack::TrackerSource& retainedSource(motrack::WorkspaceDocument& document)
{
    if (!document.trackerSource)
        throw motrack::ProjectError("Test document lost its Tracker source.");
    return *document.trackerSource;
}

motrack::WorkspaceDocument indexedMovieDocument(const QString& source)
{
    auto document = motrack::importTrackerDocument(source);
    document.movie = true;
    document.alignMovieToClip = true;
    document.movieFrameTimes = {0.0, 0.125, 0.25, 0.375};
    document.project.clip.sourceFrameCount = 4;
    return document;
}

TEST(TrackerWriter, SavesMediaVectorsThroughHistoryNativeReopenReorderingAndStandardReimport)
{
    for (const bool movie : {false, true})
    {
        for (const auto* scenario : {"samples", "geometry", "empty", "authoring", "replace", "delete-all"})
        {
            SCOPED_TRACE(movie);
            SCOPED_TRACE(scenario);
            QTemporaryDir temporary;
            const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/vector-" + (movie ? "movie/" : "images/");
            const auto source = fixture + (movie ? "linked-movie.trk" : "linked-images.trk");
            const auto input = temporary.filePath("input.trk");
            ASSERT_TRUE(QFile::copy(source, input));
            auto document = movie ? indexedMovieDocument(input) : motrack::importTrackerDocument(input);
            // Media stay in the fixture directory; only the copied original project is disposable.
            motrack::retainWorkspaceMediaLocations(document, source);
            motrack::TrackEdits edits;
            ASSERT_NO_THROW(editMediaVectors(document.project, edits, scenario));
            const auto originalXml = retainedSource(document).xml;
            std::ranges::reverse(document.project.vectors);
            const auto native = temporary.filePath("saved.motrack");
            motrack::rebaseWorkspaceMedia(document, source, native);
            motrack::writeWorkspaceDocument(native, document);
            const auto before = document.project;
            document = {};
            ASSERT_TRUE(QFile::remove(input));
            document = motrack::readWorkspaceDocument(native);
            EXPECT_EQ(document.project, before);
            const auto target = temporary.filePath("saved.trk");
            ASSERT_NO_THROW(motrack::writeTrackerDocument(target, document, native));
            const auto xml = readBytes(target);
            auto expected = before;
            std::ranges::sort(expected.vectors, {}, &motrack::VectorTrack::id);
            expected.nextTrackId = 1;
            for (auto& vector : expected.vectors)
                vector.id = expected.nextTrackId++;
            expected.clip.sourceFrameCount.reset();
            EXPECT_EQ(motrack::parseLegacyProject(xml), expected);
            EXPECT_EQ(document.project, before);
            EXPECT_EQ(retainedSource(document).xml, originalXml);
            EXPECT_NE(xml.find("vector A and vector B are words in this note."), std::string::npos);
            ASSERT_NO_THROW(motrack::writeTrackerDocument(target, document, native));
            EXPECT_EQ(readBytes(target), xml);
            auto reopened = movie ? indexedMovieDocument(target) : motrack::importTrackerDocument(target);
            edits.clear();
            ASSERT_TRUE(edits.createVector(reopened.project, "after standard reopen"));
            ASSERT_NO_THROW(motrack::writeTrackerDocument(target, reopened, target));
            EXPECT_EQ(motrack::parseLegacyProject(readBytes(target)).vectors.back().name, "after standard reopen");
        }
    }
}

TEST(TrackerWriter, SavesVaryingVectorCalibrationWithHistoryNativeReopenAndIndependentComponents)
{
    // Columns are A0, A3 and B1; components use orthogonal rotations after nonuniform scaling.
    const std::array<std::array<std::array<double, 2>, 3>, 7> components{{
        {{{2, -1}, {-0.25, 0.25}, {0, -1.5}}},
        {{{2, -1}, {-0.25, 0.25}, {0, -1.5}}},
        {{{1, 2}, {-0.25, -0.25}, {0, -1.5}}},
        {{{1, 2}, {-0.25, -0.25}, {0, -1.5}}},
        {{{0.5, -0.25}, {-0.25, 0.25}, {0, -0.375}}},
        {{{0.5, -0.25}, {-0.25, 0.25}, {0, -0.375}}},
        {{{0.25, 0.5}, {-0.25, -0.25}, {0, -0.375}}},
    }};
    for (const bool movie : {false, true})
    {
        for (std::size_t flags = 0; flags < components.size(); ++flags)
        {
            SCOPED_TRACE(movie);
            SCOPED_TRACE(flags);
            QTemporaryDir temporary;
            const auto kind = movie ? "movie" : "images";
            const auto source = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/keyed-vector-" + kind + "-" +
                                QString::number(flags) + "/linked-" + kind + ".trk";
            const auto input = temporary.filePath("input.trk");
            ASSERT_TRUE(QFile::copy(source, input));
            auto document = movie ? indexedMovieDocument(input) : motrack::importTrackerDocument(input);
            motrack::retainWorkspaceMediaLocations(document, source);
            motrack::TrackEdits edits;
            ASSERT_NO_THROW(editMediaVectors(document.project, edits, "keyed-calibration"));
            const auto native = temporary.filePath("saved.motrack");
            motrack::rebaseWorkspaceMedia(document, source, native);
            const auto expected = document.project;
            motrack::writeWorkspaceDocument(native, document);
            document = {};
            ASSERT_TRUE(QFile::remove(input));
            document = motrack::readWorkspaceDocument(native);
            EXPECT_EQ(document.project, expected);
            const auto target = temporary.filePath("saved.trk");
            ASSERT_NO_THROW(motrack::writeTrackerDocument(target, document, native));
            const auto reopened = motrack::importTrackerDocument(target).project;
            EXPECT_EQ(reopened.vectors, expected.vectors);
            EXPECT_EQ(reopened.calibration, expected.calibration);
            if (!reopened.calibration)
                FAIL() << "Saved vectors lost their calibration.";
            ASSERT_GE(reopened.vectors.size(), 2);
            ASSERT_EQ(reopened.vectors[0].samples.size(), 2);
            ASSERT_FALSE(reopened.vectors[1].samples.empty());
            const auto& first = reopened.vectors[0].samples;
            const std::array samples{first[0], first[1], reopened.vectors[1].samples[0]};
            for (std::size_t index = 0; index < samples.size(); ++index)
            {
                const auto& sample = samples[index];
                const auto tail =
                    motrack::worldPosition(*reopened.calibration, {.frame = sample.frame, .x = sample.tailX, .y = sample.tailY});
                const auto tip = motrack::worldPosition(
                    *reopened.calibration,
                    {.frame = sample.frame, .x = sample.tailX + sample.componentX, .y = sample.tailY + sample.componentY});
                if (!tail || !tip)
                    FAIL() << "Saved vector endpoints have no world positions.";
                EXPECT_NEAR(tip->x - tail->x, components[flags][index][0], 1e-12);
                EXPECT_NEAR(tip->y - tail->y, components[flags][index][1], 1e-12);
            }
            ASSERT_TRUE(edits.setClipRange(document.project, 1, 3, 2));
            ASSERT_NO_THROW(motrack::writeTrackerDocument(target, document, native));
            EXPECT_EQ(motrack::importTrackerDocument(target).project.vectors, expected.vectors);
            EXPECT_EQ(document.project.vectors, expected.vectors);
        }
    }
}

TEST(TrackerWriter, VaryingVectorsRejectOtherFramesSnapOriginsBeforeReplacingOutput)
{
    for (const bool movie : {false, true})
    {
        QTemporaryDir temporary;
        const auto kind = movie ? "movie" : "images";
        const auto source = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/keyed-vector-" + kind + "-0/linked-" + kind + ".trk";
        const auto original = movie ? indexedMovieDocument(source) : motrack::importTrackerDocument(source);
        const auto target = temporary.filePath("existing.trk");
        QFile file(target);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("existing"), 8);
        file.close();
        for (const double distance : {0.0, 8.0})
        {
            auto document = original;
            motrack::TrackEdits edits;
            ASSERT_TRUE(edits.removeVectorSample(document.project, 0, 2));
            // A0 stays far from its own frame's origin but can first draw as a trail at frame 2.
            ASSERT_TRUE(edits.setCoordinateAxes(document.project,
                                                {.frame = 2, .originX = 20 - distance, .originY = 4, .positiveX = 28, .positiveY = 4}));
            ASSERT_TRUE(edits.setClipRange(document.project, 1, 3, 1));
            EXPECT_THROW(motrack::writeTrackerDocument(target, document, source), motrack::ProjectError);
            EXPECT_EQ(readBytes(target), "existing");
        }
        const std::vector<std::function<void(motrack::WorkspaceDocument&)>> invalid{
            [](auto& doc) { doc.project.calibration->keyframes.back().frame = 4; },
            [](auto& doc) { doc.project.calibration->keyframes.pop_back(); },
            [](auto& doc) { doc.project.calibration->fixedAngle = true; },
            [](auto& doc) { doc.project.calibration->keyframes.back().scaleX = 0; },
            [](auto& doc) { doc.project.vectors[0].samples[0].tailX = 1e100; },
        };
        for (const auto& change : invalid)
        {
            auto document = original;
            change(document);
            EXPECT_THROW(motrack::writeTrackerDocument(target, document, source), motrack::ProjectError);
            EXPECT_EQ(readBytes(target), "existing");
        }
        auto document = original;
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.removeVectorSample(document.project, 0, 2));
        ASSERT_TRUE(
            edits.setCoordinateAxes(document.project, {.frame = 2, .originX = 11.99, .originY = 4, .positiveX = 28, .positiveY = 4}));
        ASSERT_NO_THROW(motrack::writeTrackerDocument(target, document, source));
        EXPECT_EQ(motrack::importTrackerDocument(target).project.vectors, document.project.vectors);
    }
}

TEST(TrackerWriter, FixedVectorOriginUsesLastLoadedKeyForSnapping)
{
    QTemporaryDir temporary;
    const auto source = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/keyed-vector-images-1/linked-images.trk";
    auto document = motrack::importTrackerDocument(source);
    if (!document.project.calibration)
        FAIL() << "Vector fixture has no calibration.";
    const auto target = temporary.filePath("saved.trk");
    // A raw earlier key lies on A0, but a fixed origin takes its value from the last key.
    auto& keys = document.project.calibration->keyframes;
    keys.front().originX = 20;
    keys.front().originY = 4;
    ASSERT_NO_THROW(motrack::writeTrackerDocument(target, document, source));
    const auto before = readBytes(target);
    keys.back().originX = 20;
    keys.back().originY = 4;
    EXPECT_THROW(motrack::writeTrackerDocument(target, document, source), motrack::ProjectError);
    EXPECT_EQ(readBytes(target), before);
}

TEST(TrackerWriter, VectorOnlyEditsBoundCoordinateWorkIncludingRetainedPadding)
{
    QDomDocument source;
    ASSERT_TRUE(source.setContent(
        QByteArray::fromStdString(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk"))));
    QDomElement coordinates;
    const auto properties = source.elementsByTagName("property");
    for (int index = 0; index < properties.size(); ++index)
    {
        auto property = properties.at(index).toElement();
        if (property.attribute("name") == "fixedangle")
            property.firstChild().setNodeValue("false");
        if (property.attribute("class") == "[Lorg.opensourcephysics.media.core.ImageCoordSystem$FrameData;")
            coordinates = property;
    }
    ASSERT_FALSE(coordinates.isNull());
    for (const int frame : {1, 2})
    {
        auto key = coordinates.firstChildElement().cloneNode(true).toElement();
        key.setAttribute("name", QString("[%1]").arg(frame));
        coordinates.appendChild(key);
    }
    const auto xml = source.toByteArray(-1).toStdString();
    const auto imported = motrack::parseTrackerProject(xml);
    if (!imported.project.calibration)
        FAIL() << "Missing keyed vector calibration.";
    ASSERT_EQ(imported.project.calibration->keyframes.size(), 3);
    ASSERT_TRUE(imported.project.calibration->fixedOrigin);
    ASSERT_FALSE(imported.project.calibration->fixedAngle);
    ASSERT_FALSE(imported.project.vectors.empty());
    ASSERT_FALSE(imported.project.vectors.front().samples.empty());
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    auto changed = imported.project;
    // Three fixed-origin setters each visit 333333 frames: 999999 updates are allowed.
    changed.vectors.front().samples.back().frame = 333332;
    ASSERT_NO_THROW(motrack::writeTrackerProject(output, changed, imported.source));
    const auto saved = readBytes(output);
    EXPECT_EQ(motrack::parseLegacyProject(saved), changed);
    EXPECT_EQ(changed.calibration, imported.project.calibration);
    for (const int frame : {333333, 400000})
    {
        SCOPED_TRACE(frame);
        changed.vectors.front().samples.back().frame = frame;
        EXPECT_THROW(motrack::writeTrackerProject(output, changed, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), saved);
    }
    // A clip-only change must use the same budget even though all samples and keys are unchanged.
    changed = imported.project;
    changed.clip.stepCount = 333334;
    EXPECT_THROW(motrack::writeTrackerProject(output, changed, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(output), saved);

    for (const auto* arrayClass :
         {"[Lorg.opensourcephysics.media.core.ImageCoordSystem$FrameData;", "[Lorg.opensourcephysics.cabrillo.tracker.Vector$FrameData;"})
    {
        SCOPED_TRACE(arrayClass);
        QDomDocument padded;
        ASSERT_TRUE(padded.setContent(QByteArray::fromStdString(xml)));
        const auto arrays = padded.elementsByTagName("property");
        bool added = false;
        for (int index = 0; index < arrays.size() && !added; ++index)
        {
            auto array = arrays.at(index).toElement();
            if (array.attribute("class") != arrayClass)
                continue;
            auto placeholder = padded.createElement("property");
            placeholder.setAttribute("name", "[333333]");
            placeholder.setAttribute("type", "object");
            placeholder.appendChild(padded.createTextNode("null"));
            array.appendChild(placeholder);
            added = true;
        }
        ASSERT_TRUE(added);
        const auto paddedXml = padded.toByteArray(-1).toStdString();
        const auto original = motrack::parseTrackerProject(paddedXml);
        EXPECT_EQ(original.project, imported.project); // Native samples/keys do not contain the null padding.
        const auto untouched = temporary.filePath("unchanged.trk");
        ASSERT_NO_THROW(motrack::writeTrackerProject(untouched, original.project, original.source));
        EXPECT_EQ(readBytes(untouched), paddedXml); // Unchanged source bytes retain their passthrough contract.
        motrack::WorkspaceDocument document{.project = original.project, .movieFrameTimes = {}, .trackerSource = original.source};
        document.project.vectors.front().samples.front().tailX += 1;
        const auto native = temporary.filePath("padded.motrack");
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        EXPECT_EQ(document.project.calibration, imported.project.calibration);
        EXPECT_THROW(motrack::writeTrackerProject(output, document.project, retainedSource(document)), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), saved);
        if (!document.project.calibration)
            FAIL() << "Native reopen lost calibration.";
        document.project.calibration->keyframes.back().angleDegrees = 1;
        EXPECT_THROW(motrack::writeTrackerProject(output, document.project, retainedSource(document)), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), saved);
        document.project = original.project;
        for (auto& vector : document.project.vectors)
            vector.samples.clear();
        EXPECT_THROW(motrack::writeTrackerProject(output, document.project, retainedSource(document)), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), saved);
    }
}

TEST(TrackerWriter, MediaVectorsRetainSnappingPrecisionIdentityFrameAndAtomicSaveGuards)
{
    const std::vector<std::function<void(motrack::WorkspaceDocument&)>> mutations{
        [](auto& doc) { doc.project.vectors[0].samples[0].frame = 4; },
        [](auto& doc)
        {
            doc.project.vectors[0].samples[0].tailX = 2;
            doc.project.vectors[0].samples[0].tailY = 20;
        },
        [](auto& doc)
        {
            doc.project.vectors[0].samples[0].tailX = 17;
            doc.project.vectors[0].samples[0].tailY = 20;
        },
        [](auto& doc) { doc.project.vectors[0].samples[0].tailX = 1e100; },
        [](auto& doc) { doc.project.vectors[0].samples[0].componentX = std::numeric_limits<double>::infinity(); },
        [](auto& doc) { doc.project.vectors[1].id = doc.project.vectors[0].id; },
        [](auto& doc) { doc.project.vectors[1].name = doc.project.vectors[0].name; },
        [](auto& doc) { doc.project.nextTrackId = 1; },
        [](auto& doc) { doc.project.calibration->fixedOrigin = false; },
        [](auto& doc) { doc.project.pointMasses.push_back({.name = "mass", .marks = {}, .id = doc.project.nextTrackId++}); },
        [](auto& doc)
        {
            auto& xml = retainedSource(doc).xml;
            xml.insert(xml.rfind("</object>"), "<property name='unknown_target' type='string'>vector A</property>\n");
        },
    };
    for (const bool movie : {false, true})
    {
        const auto source = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/vector-" +
                            (movie ? "movie/linked-movie.trk" : "images/linked-images.trk");
        const auto original = movie ? indexedMovieDocument(source) : motrack::importTrackerDocument(source);
        QTemporaryDir temporary;
        const auto target = temporary.filePath("existing.trk");
        QFile file(target);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("existing"), 8);
        file.close();
        for (std::size_t i = 0; i < mutations.size(); ++i)
        {
            SCOPED_TRACE(movie);
            SCOPED_TRACE(i);
            auto document = original;
            motrack::TrackEdits edits;
            ASSERT_NO_THROW(editMediaVectors(document.project, edits, "samples"));
            mutations[i](document);
            EXPECT_THROW(motrack::writeTrackerDocument(target, document, source), motrack::ProjectError);
            EXPECT_EQ(readBytes(target), "existing");
        }
    }
}

TEST(TrackerWriter, SavesLinkedMovieEditsAfterNativeReopenSourceRemovalAndRelocation)
{
    QTemporaryDir temporary;
    const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-movie/";
    const auto original = temporary.filePath("source");
    const auto media = temporary.filePath(QString::fromUtf8("média & frames"));
    const auto outputDirectory = temporary.filePath("exports/deep");
    ASSERT_TRUE(QDir().mkpath(original));
    ASSERT_TRUE(QDir().mkpath(media));
    ASSERT_TRUE(QDir().mkpath(outputDirectory));
    const auto input = original + "/input.trk";
    const auto movie = media + "/constant-rate.avi";
    ASSERT_TRUE(QFile::copy(fixture + "linked-movie.trk", input));
    ASSERT_TRUE(QFile::copy(fixture + "constant-rate.avi", movie));
    auto document = indexedMovieDocument(input);
    document.project.clip.mediaReferences = {QDir(original).relativeFilePath(movie).toStdString()};
    motrack::retainWorkspaceMediaLocations(document, input);
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5.0));
    ASSERT_TRUE(edits.markPointMass(document.project, 0, {.frame = 1, .x = 8.0, .y = 12.0}));
    const auto edited = document.project;
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    EXPECT_EQ(document.project, edited);
    const auto native = temporary.filePath("saved.motrack");
    motrack::rebaseWorkspaceMedia(document, input, native);
    motrack::writeWorkspaceDocument(native, document);
    document = {};
    ASSERT_TRUE(QFile::remove(input));
    ASSERT_TRUE(QDir().rmdir(original));
    document = motrack::readWorkspaceDocument(native);
    const auto snapshot = document.project;
    const auto sourceBytes = retainedSource(document).xml;
    const auto output = outputDirectory + "/saved.trk";
    motrack::writeTrackerDocument(output, document, native);
    EXPECT_EQ(document.project, snapshot);
    EXPECT_EQ(retainedSource(document).xml, sourceBytes);
    auto expected = snapshot;
    expected.clip.sourceFrameCount.reset();
    expected.clip.mediaReferences = {QDir(outputDirectory).relativeFilePath(movie).toStdString()};
    const auto bytes = readBytes(output);
    EXPECT_EQ(motrack::parseLegacyProject(bytes), expected);
    EXPECT_EQ(readBytes(movie), readBytes(fixture + "constant-rate.avi"));
    motrack::writeTrackerDocument(output, document, native);
    EXPECT_EQ(readBytes(output), bytes);
    auto reopened = indexedMovieDocument(output);
    ASSERT_TRUE(edits.setPointMassValue(reopened.project, 0, 3.0));
    motrack::writeTrackerDocument(output, reopened, output);
    EXPECT_DOUBLE_EQ(motrack::parseLegacyProject(readBytes(output)).pointMasses.front().mass, 3.0);
    const auto stable = readBytes(output);
    ASSERT_TRUE(QFile::remove(movie));
    EXPECT_THROW(motrack::writeTrackerDocument(output, reopened, output), motrack::ProjectError);
    EXPECT_EQ(readBytes(output), stable);
    ASSERT_TRUE(QFile::copy(fixture + "constant-rate.avi", movie));
    EXPECT_NO_THROW(motrack::writeTrackerDocument(output, reopened, output));
}

TEST(TrackerWriter, SavesMovieTrackAuthoringAfterHistoryNativeReopenAndSourceRemoval)
{
    for (const auto* scenario :
         {"selected-name", "unselected-name", "create", "delete-selected", "delete-unselected", "delete-all", "replace"})
    {
        SCOPED_TRACE(scenario);
        QTemporaryDir temporary;
        const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/movie-tracks/";
        const auto input = temporary.filePath("input.trk");
        ASSERT_TRUE(QFile::copy(fixture + "linked-movie.trk", input));
        ASSERT_TRUE(QFile::copy(fixture + "constant-rate.mov", temporary.filePath("constant-rate.mov")));
        auto document = indexedMovieDocument(input);
        const auto source = retainedSource(document).xml;
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5));
        ASSERT_TRUE(edits.markPointMass(document.project, 0, {.frame = 1, .x = 8, .y = 12}));
        ASSERT_NO_THROW(editMovieTracks(document.project, edits, scenario));
        const auto edited = document.project;
        // Native IDs are retained, even when XML reimport later compacts surviving identities.
        std::ranges::reverse(document.project.pointMasses);
        const auto native = temporary.filePath("saved.motrack");
        motrack::writeWorkspaceDocument(native, document);
        document = {};
        ASSERT_TRUE(QFile::remove(input));
        document = motrack::readWorkspaceDocument(native);
        std::ranges::reverse(document.project.pointMasses);
        EXPECT_EQ(document.project, edited);
        const auto target = temporary.filePath("saved.trk");
        ASSERT_NO_THROW(motrack::writeTrackerDocument(target, document, native));
        const auto bytes = readBytes(target);
        auto expected = edited;
        expected.clip.sourceFrameCount.reset();
        expected.nextTrackId = 1;
        for (auto& track : expected.pointMasses)
            track.id = expected.nextTrackId++;
        EXPECT_EQ(motrack::parseLegacyProject(bytes), expected);
        EXPECT_EQ(retainedSource(document).xml, source);
        EXPECT_EQ(document.project, edited);
        EXPECT_NE(bytes.find("movie mass and other mass are words in this note."), std::string::npos);
        ASSERT_NO_THROW(motrack::writeTrackerDocument(target, document, native));
        EXPECT_EQ(readBytes(target), bytes);
        auto reopened = indexedMovieDocument(target);
        ASSERT_TRUE(edits.createPointMass(reopened.project, "after standard reopen"));
        ASSERT_NO_THROW(motrack::writeTrackerDocument(target, reopened, target));
        EXPECT_EQ(motrack::parseLegacyProject(readBytes(target)).pointMasses.back().name, "after standard reopen");
        EXPECT_EQ(readBytes(temporary.filePath("constant-rate.mov")), readBytes(fixture + "constant-rate.mov"));
    }
}

TEST(TrackerWriter, MovieAuthoringRetainsIdentitySchemaFrameAndAtomicSaveGuards)
{
    const auto source = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/movie-tracks/linked-movie.trk";
    const auto original = indexedMovieDocument(source);
    QTemporaryDir temporary;
    const auto target = temporary.filePath("existing.trk");
    QFile file(target);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("existing"), 8);
    file.close();
    const std::vector<std::function<void(motrack::WorkspaceDocument&)>> mutations{
        [](auto& doc) { doc.project.pointMasses.back().name = "movie mass"; },
        [](auto& doc) { doc.project.pointMasses.back().id = doc.project.pointMasses.front().id; },
        [](auto& doc) { doc.project.nextTrackId = 1; },
        [](auto& doc) { doc.project.pointMasses.back().marks.push_back({.frame = 4, .x = 1, .y = 2}); },
        [](auto& doc) { doc.project.pointMasses.back().name = "null"; },
        [](auto& doc)
        {
            auto& xml = retainedSource(doc).xml;
            xml.insert(xml.rfind("</object>"),
                       "<property name='selected_track_views' type='string'>missing,null;null,null;null,null;null,null</property>\n");
        },
        [](auto& doc)
        {
            auto& xml = retainedSource(doc).xml;
            xml.insert(xml.rfind("</object>"), "<property name='unknown_target' type='string'>movie mass</property>\n");
        },
        [](auto& doc) { doc.project.vectors.push_back({.name = "vector", .samples = {}, .id = doc.project.nextTrackId++}); },
    };
    for (std::size_t i = 0; i < mutations.size(); ++i)
    {
        SCOPED_TRACE(i);
        auto document = original;
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.createPointMass(document.project, "created"));
        mutations[i](document);
        EXPECT_THROW(motrack::writeTrackerDocument(target, document, source), motrack::ProjectError);
        EXPECT_EQ(readBytes(target), "existing");
    }
}

TEST(TrackerWriter, SavesFixedMovieCalibrationAndUnitsThroughHistoryNativeReopenAndClipSaving)
{
    for (const bool mov : {false, true})
    {
        SCOPED_TRACE(mov);
        QTemporaryDir temporary;
        const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/" + (mov ? "linked-mov/" : "linked-movie/");
        const auto mediaName = mov ? QStringLiteral("constant-rate.mov") : QStringLiteral("constant-rate.avi");
        const auto input = temporary.filePath("input.trk");
        const auto movie = temporary.filePath(mediaName);
        ASSERT_TRUE(QFile::copy(fixture + "linked-movie.trk", input));
        ASSERT_TRUE(QFile::copy(fixture + mediaName, movie));
        auto document = indexedMovieDocument(input);
        const auto sourceBytes = retainedSource(document).xml;
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5));
        ASSERT_TRUE(edits.markPointMass(document.project, 0, {.frame = 1, .x = 8, .y = 12}));
        ASSERT_TRUE(edits.setClipRange(document.project, 1, 3, 2));
        const auto before = document.project;
        ASSERT_TRUE(
            edits.setCoordinateAxes(document.project, {.frame = 1, .originX = 10, .originY = 18, .positiveX = 10, .positiveY = 10}));
        ASSERT_TRUE(
            edits.calibrateDistance(document.project, {.frame = 2, .imageDeltaX = 8, .imageDeltaY = 0, .knownLength = 2, .unit = "cm"}));
        auto expected = document.project;
        ASSERT_TRUE(edits.undo(document.project));
        ASSERT_TRUE(edits.undo(document.project));
        EXPECT_EQ(document.project, before);
        ASSERT_TRUE(edits.redo(document.project));
        ASSERT_TRUE(edits.redo(document.project));
        EXPECT_EQ(document.project, expected);
        const auto native = temporary.filePath("saved.motrack");
        motrack::rebaseWorkspaceMedia(document, input, native);
        motrack::writeWorkspaceDocument(native, document);
        document = {};
        ASSERT_TRUE(QFile::remove(input));
        document = motrack::readWorkspaceDocument(native);
        const auto target = temporary.filePath("saved.trk");
        ASSERT_NO_THROW(motrack::writeTrackerDocument(target, document, native));
        const auto reopened = motrack::importTrackerDocument(target).project;
        expected.clip.sourceFrameCount.reset();
        EXPECT_EQ(reopened, expected);
        EXPECT_EQ(retainedSource(document).xml, sourceBytes);
        EXPECT_EQ(readBytes(movie), readBytes(fixture + mediaName));
        if (!reopened.calibration)
            FAIL() << "Saved movie lost its calibration.";
        const std::array<std::array<double, 2>, 3> world{{{0.25, 1.5}, {0.75, 0.5}, {1.75, -0.5}}};
        ASSERT_EQ(reopened.pointMasses.front().marks.size(), world.size());
        for (std::size_t index = 0; index < world.size(); ++index)
        {
            const auto position = motrack::worldPosition(*reopened.calibration, reopened.pointMasses.front().marks[index]);
            if (!position)
                FAIL() << "Saved movie has no world position.";
            EXPECT_NEAR(position->x, world[index][0], 1e-12);
            EXPECT_NEAR(position->y, world[index][1], 1e-12);
        }
    }
}

TEST(TrackerWriter, SavesAllMovingMovieCalibrationFlagsAcrossKeyBoundariesAndNativeReopen)
{
    // Independently evaluated coordinates at original frames 0/1/2/3. The final two
    // marks lie at the same image point; the new frame-1 key and existing frame-2 key differ.
    const std::array<std::array<std::array<double, 2>, 4>, 7> worlds{{
        {{{1, 1}, {-0.5, 1}, {-1, 0}, {-1, 0}}},
        {{{-1, -0.5}, {-0.5, 1}, {-0.25, -0.125}, {-0.25, -0.125}}},
        {{{-1, 1}, {-0.5, 1}, {0, -1}, {0, -1}}},
        {{{0.5, -1}, {-0.5, 1}, {0.125, -0.25}, {0.125, -0.25}}},
        {{{0.25, 0.25}, {-0.125, 0.25}, {-1, 0}, {-1, 0}}},
        {{{-0.25, -0.125}, {-0.125, 0.25}, {-0.25, -0.125}, {-0.25, -0.125}}},
        {{{-0.25, 0.25}, {-0.125, 0.25}, {0, -1}, {0, -1}}},
    }};
    for (std::size_t flags = 0; flags < worlds.size(); ++flags)
    {
        SCOPED_TRACE(flags);
        QTemporaryDir temporary;
        const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/keyed-movie-" + QString::number(flags) + '/';
        const auto source = temporary.filePath("input.trk");
        ASSERT_TRUE(QFile::copy(fixture + "linked-movie.trk", source));
        ASSERT_TRUE(QFile::copy(fixture + "constant-rate.mov", temporary.filePath("constant-rate.mov")));
        auto document = indexedMovieDocument(source);
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5));
        ASSERT_TRUE(edits.markPointMass(document.project, 0, {.frame = 1, .x = 8, .y = 12}));
        ASSERT_TRUE(edits.setClipRange(document.project, 1, 3, 2));
        const auto before = document.project;
        ASSERT_TRUE(
            edits.calibrateDistance(document.project, {.frame = 2, .imageDeltaX = 8, .imageDeltaY = 0, .knownLength = 1, .unit = "cm"}));
        ASSERT_TRUE(
            edits.setCoordinateAxes(document.project, {.frame = 2, .originX = 12, .originY = 16, .positiveX = 20, .positiveY = 16}));
        ASSERT_TRUE(edits.setCoordinateAxes(document.project, {.frame = 1, .originX = 6, .originY = 14, .positiveX = 6, .positiveY = 22}));
        auto expected = document.project;
        for (int index = 0; index < 3; ++index)
            ASSERT_TRUE(edits.undo(document.project));
        EXPECT_EQ(document.project, before);
        for (int index = 0; index < 3; ++index)
            ASSERT_TRUE(edits.redo(document.project));
        EXPECT_EQ(document.project, expected);
        const auto native = temporary.filePath("saved.motrack");
        motrack::rebaseWorkspaceMedia(document, source, native);
        motrack::writeWorkspaceDocument(native, document);
        document = {};
        ASSERT_TRUE(QFile::remove(source));
        document = motrack::readWorkspaceDocument(native);
        const auto target = temporary.filePath("saved.trk");
        ASSERT_NO_THROW(motrack::writeTrackerDocument(target, document, native));
        const auto reopened = motrack::importTrackerDocument(target).project;
        expected.clip.sourceFrameCount.reset();
        EXPECT_EQ(reopened, expected);
        if (!reopened.calibration)
            FAIL() << "Missing keyed movie calibration.";
        ASSERT_EQ(reopened.pointMasses.front().marks.size(), worlds[flags].size());
        for (std::size_t frame = 0; frame < worlds[flags].size(); ++frame)
        {
            const auto position = motrack::worldPosition(*reopened.calibration, reopened.pointMasses.front().marks[frame]);
            if (!position)
                FAIL() << "Missing keyed movie world position.";
            EXPECT_NEAR(position->x, worlds[flags][frame][0], 1e-12);
            EXPECT_NEAR(position->y, worlds[flags][frame][1], 1e-12);
        }
    }
}

TEST(TrackerWriter, RefusesUnsafeMovingMovieCalibrationBeforeReplacingOutput)
{
    QTemporaryDir temporary;
    const auto source = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/keyed-movie-0/linked-movie.trk";
    const auto original = indexedMovieDocument(source);
    const auto target = temporary.filePath("existing.trk");
    const QByteArray preserved("existing file");
    {
        QFile file(target);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(preserved), preserved.size());
    }
    const std::array<std::function<void(motrack::Calibration&)>, 5> invalid{
        [](auto& calibration)
        {
            auto key = calibration.keyframes.back();
            key.frame = 4; // Outside the complete movie, not merely outside its selected clip.
            calibration.keyframes.push_back(key);
        },
        [](auto& calibration) { calibration.keyframes.pop_back(); },
        [](auto& calibration) { calibration.fixedOrigin = true; },
        [](auto& calibration) { calibration.keyframes.back().scaleX = 0; },
        [](auto& calibration)
        {
            calibration.keyframes.back().angleDegrees = std::numeric_limits<double>::infinity();
        }};
    for (const auto& change : invalid)
    {
        auto document = original;
        if (!document.project.calibration)
            FAIL() << "Missing keyed movie source calibration.";
        change(*document.project.calibration);
        EXPECT_THROW(motrack::writeTrackerDocument(target, document, source), motrack::ProjectError);
        EXPECT_EQ(readBytes(target), preserved);
    }
    for (const std::string replacement :
         {R"(<property name="[3]" type="string">null</property>)", R"(<property name="[3]" type="object">missing</property>)"})
    {
        auto document = original;
        auto& xml = retainedSource(document).xml;
        const std::string placeholder = R"(<property name="[3]" type="object">null</property>)";
        const auto at = xml.find(placeholder);
        ASSERT_NE(at, std::string::npos);
        xml.replace(at, placeholder.size(), replacement);
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setCoordinateAxes(document.project, {.frame = 1, .originX = 6, .originY = 14, .positiveX = 6, .positiveY = 22}));
        EXPECT_THROW(motrack::writeTrackerDocument(target, document, source), motrack::ProjectError);
        EXPECT_EQ(readBytes(target), preserved);
    }
}

TEST(TrackerWriter, SavesStridedAndSingleFrameMovieClipsAfterNativeReopenWithoutDroppingMarks)
{
    for (const bool single : {false, true})
    {
        SCOPED_TRACE(single);
        QTemporaryDir temporary;
        const auto source = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-movie/linked-movie.trk";
        auto document = indexedMovieDocument(source);
        const auto sourceBytes = retainedSource(document).xml;
        const auto movie = QFileInfo(source).absolutePath() + "/constant-rate.avi";
        const auto movieBytes = readBytes(movie);
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.markPointMass(document.project, 0, {.frame = 1, .x = 8.0, .y = 12.0}));
        const auto before = document.project;
        ASSERT_TRUE(edits.setClipRange(document.project, single ? 2 : 1, single ? 2 : 3, single ? 1 : 2));
        const auto clipped = document.project;
        ASSERT_TRUE(edits.undo(document.project));
        EXPECT_EQ(document.project, before);
        ASSERT_TRUE(edits.redo(document.project));
        EXPECT_EQ(document.project, clipped);
        const auto native = temporary.filePath("clip.motrack");
        motrack::rebaseWorkspaceMedia(document, source, native);
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        const auto target = temporary.filePath("clip.trk");
        ASSERT_NO_THROW(motrack::writeTrackerDocument(target, document, native));
        const auto reopened = motrack::importTrackerDocument(target).project;
        EXPECT_EQ(reopened.clip.firstFrame, single ? 2 : 1);
        EXPECT_EQ(reopened.clip.stepSize, single ? 1 : 2);
        EXPECT_EQ(reopened.clip.stepCount, single ? 1 : 2);
        EXPECT_EQ(reopened.pointMasses, before.pointMasses);
        EXPECT_DOUBLE_EQ(reopened.clip.startTimeMs, -250.0);
        EXPECT_EQ(document.movieFrameTimes, (std::vector<double>{0.0, 0.125, 0.25, 0.375}));
        EXPECT_EQ(document.project.clip.sourceFrameCount, 4);
        EXPECT_EQ(retainedSource(document).xml, sourceBytes);
        EXPECT_EQ(readBytes(movie), movieBytes);
        auto expanded = indexedMovieDocument(target);
        ASSERT_TRUE(edits.setClipRange(expanded.project, 0, 3, 1));
        ASSERT_NO_THROW(motrack::writeTrackerDocument(target, expanded, target));
        EXPECT_EQ(motrack::importTrackerDocument(target).project.pointMasses, before.pointMasses);
        EXPECT_EQ(motrack::importTrackerDocument(target).project.clip.stepCount, 4);
    }
}

TEST(TrackerWriter, SavesMovieDefaultNumberFormatsAndRejectsCustomFormatsAfterNativeReopen)
{
    for (const bool custom : {false, true})
    {
        SCOPED_TRACE(custom);
        QTemporaryDir temporary;
        const auto source = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-movie/linked-movie.trk";
        auto document = indexedMovieDocument(source);
        const std::string token = "<object class=\"org.opensourcephysics.cabrillo.tracker.PointMass\">";
        auto& xml = retainedSource(document).xml;
        const auto at = xml.find(token);
        ASSERT_NE(at, std::string::npos);
        const auto formats =
            QString("<property name='number_formats' type='array' class='[Ljava.lang.String;'>"
                    "<property name='[0]' type='string'>frame</property><property name='[1]' type='string'>0</property>"
                    "<property name='[2]' type='string'>t</property><property name='[3]' type='string'>%1</property></property>")
                .arg(custom ? "0.0000" : "0.000");
        xml.insert(at + token.size(), formats.toStdString());
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5.0));
        const auto native = temporary.filePath("saved.motrack");
        motrack::rebaseWorkspaceMedia(document, source, native);
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        const auto output = temporary.filePath("saved.trk");
        {
            QFile file(output);
            ASSERT_TRUE(file.open(QIODevice::WriteOnly));
            ASSERT_EQ(file.write("existing"), 8);
        }
        if (custom)
        {
            EXPECT_THROW(motrack::writeTrackerDocument(output, document, native), motrack::ProjectError);
            EXPECT_EQ(readBytes(output), "existing");
        }
        else
        {
            ASSERT_NO_THROW(motrack::writeTrackerDocument(output, document, native));
            const auto saved = readBytes(output);
            EXPECT_DOUBLE_EQ(motrack::parseLegacyProject(saved).pointMasses.front().mass, 5.0);
            QDomDocument parsed;
            ASSERT_TRUE(parsed.setContent(QByteArray::fromStdString(saved)));
            const auto properties = parsed.elementsByTagName("property");
            QDomElement retained;
            for (int index = 0; index < properties.size(); ++index)
                if (properties.at(index).toElement().attribute("name") == "number_formats")
                    retained = properties.at(index).toElement();
            ASSERT_FALSE(retained.isNull());
            EXPECT_EQ(retained.firstChildElement().text(), "frame");
            EXPECT_EQ(retained.firstChildElement().nextSiblingElement().text(), "0");
            EXPECT_EQ(retained.lastChildElement().previousSiblingElement().text(), "t");
            EXPECT_EQ(retained.lastChildElement().text(), "0.000");
        }
    }
}

TEST(TrackerWriter, RejectsUnsupportedMovieStateBeforeReplacingExistingOutput)
{
    QTemporaryDir temporary;
    const auto source = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-movie/linked-movie.trk";
    const auto original = indexedMovieDocument(source);
    const auto output = temporary.filePath("existing.trk");
    {
        QFile file(output);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("existing"), 8);
    }
    const std::vector<std::function<void(motrack::WorkspaceDocument&)>> mutations{
        [](auto& doc) { doc.movie = false; },
        [](auto& doc) { doc.alignMovieToClip = false; },
        [](auto& doc) { doc.useDisplayOrientation = true; },
        [](auto& doc) { doc.movieFrameTimes.clear(); },
        [](auto& doc) { doc.movieFrameTimes[2] = 0.26; },
        [](auto& doc) { doc.movieFrameTimes[0] = std::numeric_limits<double>::quiet_NaN(); },
        [](auto& doc) { doc.movieFrameTimes.pop_back(); },
        [](auto& doc) { doc.project.clip.sourceFrameCount = 3; },
        [](auto& doc) { doc.project.clip.startTimeMs = 0.0; },
        [](auto& doc) { doc.project.clip.frameDurationMs = 125.0; },
        [](auto& doc) { doc.project.clip.stepCount = 5; },
        [](auto& doc) { doc.project.clip.stepSize = 0; },
        [](auto& doc) { doc.project.clip.firstFrame = 4; },
        [](auto& doc) { doc.project.clip.stepCount = 0; },
        [](auto& doc) { doc.project.pointMasses.front().name = "bad,name"; },
        [](auto& doc) { doc.project.pointMasses.front().marks.front().frame = 4; },
        [](auto& doc) { doc.project.massUnit = "g"; },
        [](auto& doc) { doc.project.lengthUnit = "bad\nunit"; },
        [](auto& doc) { doc.project.calibration.reset(); },
        [](auto& doc) { doc.project.calibration = motrack::Calibration{}; },
        [](auto& doc)
        {
            if (!doc.project.calibration)
                FAIL() << "Missing fixture calibration.";
            doc.project.calibration->fixedOrigin = false;
        },
        [](auto& doc) { doc.project.clip.mediaReferences.front() = "missing.avi"; },
        [](auto& doc)
        {
            auto& xml = retainedSource(doc).xml;
            const std::string token = "org.opensourcephysics.media.xuggle.XuggleVideo";
            const auto at = xml.find(token);
            ASSERT_NE(at, std::string::npos);
            xml.replace(at, token.size(), "org.opensourcephysics.media.core.ImageVideo");
        },
        [](auto& doc)
        {
            auto& xml = retainedSource(doc).xml;
            const auto at = xml.find("<property name=\"path\"");
            ASSERT_NE(at, std::string::npos);
            xml.insert(at, "<property name=\"filters\" type=\"collection\" class=\"java.util.ArrayList\"/>\n");
        }};
    for (std::size_t index = 0; index < mutations.size(); ++index)
    {
        SCOPED_TRACE(index);
        auto document = original;
        ASSERT_NO_FATAL_FAILURE(mutations[index](document));
        EXPECT_THROW(motrack::writeTrackerDocument(output, document, source), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), "existing");
    }
}

TEST(TrackerWriter, SavesLinkedImagesAfterNativeReopenSourceRemovalAndRelocation)
{
    QTemporaryDir temporary;
    const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/";
    const auto sourceDirectory = temporary.filePath(QString::fromUtf8("élève & source"));
    const auto nativeDirectory = temporary.filePath("native");
    const auto outputDirectory = temporary.filePath(QString::fromUtf8("édition"));
    ASSERT_TRUE(QDir().mkpath(sourceDirectory + "/images"));
    ASSERT_TRUE(QDir().mkpath(nativeDirectory));
    ASSERT_TRUE(QDir().mkpath(outputDirectory));
    for (int frame = 0; frame < 4; ++frame)
    {
        const auto name = "images/frame" + QString::number(frame) + ".png";
        ASSERT_TRUE(QFile::copy(fixture + name, sourceDirectory + "/" + name));
    }
    const auto input = sourceDirectory + "/input.trk";
    ASSERT_TRUE(QFile::copy(fixture + "linked-images.trk", input));
    auto document = motrack::importTrackerDocument(input);
    const auto originalSource = retainedSource(document).xml;
    const auto unchanged = sourceDirectory + "/unchanged.trk";
    motrack::writeTrackerDocument(unchanged, document, input);
    EXPECT_EQ(readBytes(unchanged), originalSource);
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5.0));
    ASSERT_TRUE(edits.markPointMass(document.project, 0, {.frame = 1, .x = 8.0, .y = 12.0}));
    const auto edited = document.project;
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    EXPECT_EQ(document.project, edited);
    const auto native = nativeDirectory + "/saved.motrack";
    for (auto& reference : document.project.clip.mediaReferences)
        reference = QDir(nativeDirectory).relativeFilePath(sourceDirectory + "/" + QString::fromStdString(reference)).toStdString();
    document.project.clip.sourceFrameCount = 4;
    motrack::writeWorkspaceDocument(native, document);
    document = {};
    ASSERT_TRUE(QFile::remove(input));
    document = motrack::readWorkspaceDocument(native);
    const auto nativeProject = document.project;
    const auto output = outputDirectory + "/saved.trk";
    motrack::writeTrackerDocument(output, document, native);
    EXPECT_EQ(document.project, nativeProject);
    EXPECT_EQ(retainedSource(document).xml, originalSource);
    const auto bytes = readBytes(output);
    const auto exported = motrack::parseTrackerProject(bytes);
    auto expected = nativeProject;
    expected.clip.sourceFrameCount.reset();
    for (std::size_t frame = 0; frame < expected.clip.mediaReferences.size(); ++frame)
    {
        const auto name = "images/frame" + QString::number(frame) + ".png";
        expected.clip.mediaReferences[frame] = QDir(outputDirectory).relativeFilePath(sourceDirectory + "/" + name).toStdString();
        const auto resolved = QDir(outputDirectory).absoluteFilePath(QString::fromStdString(exported.project.clip.mediaReferences[frame]));
        EXPECT_EQ(readBytes(resolved), readBytes(fixture + name));
    }
    EXPECT_EQ(exported.project, expected);
    EXPECT_NE(bytes.find("<![CDATA["), std::string::npos); // Unicode and '&' paths must use OSP-safe text.
    motrack::writeTrackerDocument(output, document, native);
    EXPECT_EQ(readBytes(output), bytes);
    auto reimported = motrack::importTrackerDocument(output);
    ASSERT_TRUE(edits.setPointMassValue(reimported.project, 0, 3.0));
    motrack::writeTrackerDocument(output, reimported, output);
    EXPECT_DOUBLE_EQ(motrack::parseLegacyProject(readBytes(output)).pointMasses.front().mass, 3.0);
    EXPECT_THROW(motrack::writeTrackerProject(output, document.project, retainedSource(document)), motrack::ProjectError);
}

TEST(TrackerWriter, SavesTrimmedImageClipsWithStrideAndPreservesAllSourceFrames)
{
    const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/";
    const std::array ranges{std::array{1, 3, 2}, std::array{1, 2, 1}, std::array{2, 2, 1}, std::array{0, 2, 2}};
    for (const bool explicitCount : {false, true})
        for (const auto& range : ranges)
        {
            SCOPED_TRACE(explicitCount);
            SCOPED_TRACE(::testing::PrintToString(range));
            QTemporaryDir temporary;
            ASSERT_TRUE(QDir(temporary.path()).mkdir("images"));
            const auto input = temporary.filePath("input.trk");
            ASSERT_TRUE(QFile::copy(fixture + "linked-images.trk", input));
            for (int frame = 0; frame < 4; ++frame)
            {
                const auto name = "images/frame" + QString::number(frame) + ".png";
                ASSERT_TRUE(QFile::copy(fixture + name, temporary.filePath(name)));
            }
            auto document = motrack::importTrackerDocument(input);
            if (explicitCount)
            {
                const std::string start = "<object class=\"org.opensourcephysics.media.core.VideoClip\">";
                const auto position = retainedSource(document).xml.find(start);
                ASSERT_NE(position, std::string::npos);
                retainedSource(document).xml.insert(position + start.size(),
                                                    "\n<property name=\"video_framecount\" type=\"int\">4</property>\n");
            }
            const auto original = document.project;
            motrack::TrackEdits edits;
            ASSERT_TRUE(edits.setClipRange(document.project, range[0], range[1], range[2]));
            const auto expected = document.project;
            ASSERT_TRUE(edits.undo(document.project));
            EXPECT_EQ(document.project, original);
            ASSERT_TRUE(edits.redo(document.project));
            const auto native = temporary.filePath("clip.motrack");
            motrack::writeWorkspaceDocument(native, document);
            ASSERT_TRUE(QFile::remove(input));
            document = motrack::readWorkspaceDocument(native);
            const auto output = temporary.filePath("clip.trk");
            ASSERT_NO_THROW(motrack::writeTrackerDocument(output, document, native));
            const auto bytes = readBytes(output);
            const auto reopened = motrack::parseLegacyProject(bytes);
            EXPECT_EQ(reopened, expected);
            EXPECT_EQ(reopened.pointMasses, original.pointMasses);
            EXPECT_EQ(reopened.clip.mediaReferences, original.clip.mediaReferences);
            for (int frame = 0; frame < 4; ++frame)
            {
                const auto name = "images/frame" + QString::number(frame) + ".png";
                EXPECT_EQ(readBytes(temporary.filePath(name)), readBytes(fixture + name));
            }
            document = motrack::importTrackerDocument(output);
            EXPECT_NO_THROW(motrack::writeTrackerDocument(output, document, output));
            EXPECT_EQ(readBytes(output), bytes);
        }
}

TEST(TrackerWriter, RetainedMediaLocationsSurviveRemovalOfTheOriginalProjectDirectory)
{
    QTemporaryDir temporary;
    const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/";
    ASSERT_TRUE(QDir(temporary.path()).mkpath("import"));
    ASSERT_TRUE(QDir(temporary.path()).mkpath("media"));
    ASSERT_TRUE(QDir(temporary.path()).mkpath("exports/deep"));
    for (int frame = 0; frame < 4; ++frame)
    {
        const auto name = "frame" + QString::number(frame) + ".png";
        ASSERT_TRUE(QFile::copy(fixture + "images/" + name, temporary.filePath("media/" + name)));
    }
    auto xml = QByteArray::fromStdString(readBytes(fixture + "linked-images.trk"));
    xml.replace("images/", "../media/");
    const auto input = temporary.filePath("import/input.trk");
    QFile source(input);
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    ASSERT_EQ(source.write(xml), xml.size());
    source.close();
    auto document = motrack::importTrackerDocument(input);
    motrack::retainWorkspaceMediaLocations(document, input);
    ASSERT_EQ(document.mediaLocations.size(), 4u);
    ASSERT_TRUE(QFile::remove(input));
    ASSERT_TRUE(QDir(temporary.path()).rmdir("import"));
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5));
    const auto output = temporary.filePath("exports/deep/saved.trk");
    motrack::writeTrackerDocument(output, document, input);
    const auto reopened = motrack::importTrackerDocument(output);
    EXPECT_EQ(reopened.project.pointMasses.front().mass, 5);
    ASSERT_EQ(reopened.project.clip.mediaReferences.size(), 4u);
    for (int frame = 0; frame < 4; ++frame)
    {
        const auto name = "frame" + QString::number(frame) + ".png";
        const auto& reference = reopened.project.clip.mediaReferences[static_cast<std::size_t>(frame)];
        EXPECT_EQ(reference, ("../../media/" + name).toStdString());
        EXPECT_EQ(readBytes(motrack::resolveLocalMediaPath(output, reference)), readBytes(fixture + "images/" + name));
    }
}

TEST(TrackerWriter, RestoredMissingMediaCanBeSavedWithoutReopeningTheProject)
{
    for (const bool removeOriginalDirectory : {false, true})
    {
        SCOPED_TRACE(removeOriginalDirectory);
        QTemporaryDir temporary;
        const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/";
        ASSERT_TRUE(QDir(temporary.path()).mkpath("import"));
        ASSERT_TRUE(QDir(temporary.path()).mkpath("media"));
        ASSERT_TRUE(QDir(temporary.path()).mkpath("exports"));
        auto xml = QByteArray::fromStdString(readBytes(fixture + "linked-images.trk"));
        xml.replace("images/", "../media/");
        const auto input = temporary.filePath("import/input.trk");
        QFile source(input);
        ASSERT_TRUE(source.open(QIODevice::WriteOnly));
        ASSERT_EQ(source.write(xml), xml.size());
        source.close();
        auto document = motrack::importTrackerDocument(input);
        motrack::retainWorkspaceMediaLocations(document, input);
        // Empty bindings model an alias that could not be resolved at open, on every platform.
        // Nonempty bindings still cover removal of the original project directory below.
        if (!removeOriginalDirectory)
            for (auto& location : document.mediaLocations)
                location.clear();
        const auto output = temporary.filePath("exports/saved.trk");
        EXPECT_THROW(motrack::writeTrackerDocument(output, document, input), motrack::ProjectError);
        EXPECT_FALSE(QFileInfo::exists(output));
        for (int frame = 0; frame < 4; ++frame)
        {
            const auto name = "frame" + QString::number(frame) + ".png";
            ASSERT_TRUE(QFile::copy(fixture + "images/" + name, temporary.filePath("media/" + name)));
        }
        if (removeOriginalDirectory)
        {
            ASSERT_TRUE(QFile::remove(input));
            ASSERT_TRUE(QDir(temporary.path()).rmdir("import"));
        }
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5));
        EXPECT_NO_THROW(motrack::writeTrackerDocument(output, document, input));
        motrack::refreshWorkspaceMediaLocations(document, input);
        for (const auto& location : document.mediaLocations)
            EXPECT_FALSE(location.isEmpty());
        const auto reopened = motrack::importTrackerDocument(output);
        EXPECT_EQ(reopened.project.pointMasses.front().mass, 5);
        for (int frame = 0; frame < 4; ++frame)
        {
            const auto name = "frame" + QString::number(frame) + ".png";
            EXPECT_EQ(readBytes(motrack::resolveLocalMediaPath(output, reopened.project.clip.mediaReferences.at(frame))),
                      readBytes(fixture + "images/" + name));
        }
#ifndef Q_OS_WIN
        // A restored image may itself be an alias; refresh its identity without changing the retained base.
        const auto first = temporary.filePath("media/frame0.png");
        ASSERT_TRUE(QFile::remove(first));
        ASSERT_TRUE(QFile::link(fixture + "images/frame0.png", first));
        EXPECT_NO_THROW(motrack::writeTrackerDocument(output, document, input));
        const auto aliased = motrack::importTrackerDocument(output);
        EXPECT_EQ(readBytes(motrack::resolveLocalMediaPath(output, aliased.project.clip.mediaReferences.front())),
                  readBytes(fixture + "images/frame0.png"));
#endif
    }
}

TEST(TrackerWriter, RejectsHardLinkedMediaDestinationsWithoutChangingEitherName)
{
    QTemporaryDir temporary;
    const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/";
    const auto media = temporary.filePath("image.png");
    ASSERT_TRUE(QFile::copy(fixture + "images/frame0.png", media));
    const auto target = temporary.filePath("saved.trk");
    std::error_code error;
    std::filesystem::create_hard_link(std::filesystem::path(media.toStdU16String()), std::filesystem::path(target.toStdU16String()), error);
    ASSERT_FALSE(error) << error.message();
    ASSERT_TRUE(motrack::sameLocalFile(media, target));
    const auto original = readBytes(media);
    auto document = motrack::importTrackerDocument(fixture + "linked-images.trk");
    document.project.clip.mediaReferences.front() = media.toStdString();
    EXPECT_THROW(motrack::writeTrackerDocument(target, document, fixture + "linked-images.trk"), motrack::ProjectError);
    EXPECT_EQ(readBytes(media), original);
    EXPECT_EQ(readBytes(target), original);
    EXPECT_TRUE(motrack::sameLocalFile(media, target));
}

TEST(TrackerWriter, RejectsUnsafeLinkedImageExportsBeforeReplacingTheDestination)
{
    QTemporaryDir temporary;
    const auto input = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/linked-images.trk";
    const auto original = motrack::importTrackerDocument(input);
    const auto destination = temporary.filePath("existing.trk");
    {
        QFile file(destination);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("existing"), 8);
    }
    const std::vector<std::function<void(motrack::WorkspaceDocument&)>> mutations{
        [](auto& doc) { doc.trackerSource.reset(); },
        [](auto& doc) { doc.movie = true; },
        [](auto& doc) { doc.movieFrameTimes = {0.0}; },
        [](auto& doc) { doc.useDisplayOrientation = true; },
        [](auto& doc) { doc.archiveReference = "old.trz"; },
        [](auto& doc) { doc.archiveMember = "project.trk"; },
        [](auto& doc) { doc.project.clip.mediaReferences[1] = "missing.png"; },
        [](auto& doc) { doc.project.clip.mediaReferences[1] = "https://example.org/frame.png"; },
        [](auto& doc) { doc.project.clip.mediaReferences[1] = "archive.zip!/frame.png"; },
        [](auto& doc) { doc.project.clip.mediaReferences[1] = ":resource.png"; },
        [](auto& doc) { doc.project.clip.mediaReferences[1] = "line\nbreak.png"; },
        [](auto& doc) { doc.project.clip.mediaReferences[1] = "bad]]>name.png"; },
        [](auto& doc) { doc.project.clip.mediaReferences.pop_back(); },
        [](auto& doc) { doc.project.clip.sourceFrameCount = 3; },
        [](auto& doc) { doc.project.clip.firstFrame = 1; },
        [](auto& doc) { doc.project.clip.frameDurationMs = 250.0; },
        [](auto& doc) { doc.project.clip.startTimeMs = 0.0; },
        [](auto& doc) { doc.project.pointMasses.front().marks.front().frame = 4; },
        [](auto& doc)
        {
            motrack::TrackEdits edits;
            ASSERT_TRUE(edits.createVector(doc.project, "unsupported image vector"));
        },
        [](auto& doc)
        {
            const std::string type = "org.opensourcephysics.media.core.ImageVideo";
            const auto position = retainedSource(doc).xml.find(type);
            ASSERT_NE(position, std::string::npos);
            retainedSource(doc).xml.replace(position, type.size(), "org.opensourcephysics.media.xuggle.XuggleVideo");
        },
        [](auto& doc)
        {
            const std::string start = "<object class=\"org.opensourcephysics.media.core.ImageVideo\">";
            const auto position = retainedSource(doc).xml.find(start);
            ASSERT_NE(position, std::string::npos);
            retainedSource(doc).xml.insert(position + start.size(),
                                           "\n<property name=\"filters\" type=\"collection\" class=\"java.util.ArrayList\"/>\n");
        },
        [](auto& doc)
        {
            const std::string previous = R"(<property name="path" type="string">images/frame0.png)";
            const auto position = retainedSource(doc).xml.find(previous);
            ASSERT_NE(position, std::string::npos);
            retainedSource(doc).xml.replace(position, previous.size(), R"(<property name="path" type="string">images/frame1.png)");
        },
        [](auto& doc)
        {
            const std::string previous = R"(<property name="delta_t" type="double">125.0)";
            const auto position = retainedSource(doc).xml.find(previous);
            ASSERT_NE(position, std::string::npos);
            retainedSource(doc).xml.replace(position, previous.size(), R"(<property name="delta_t" type="double">250.0)");
        }};
    for (std::size_t index = 0; index < mutations.size(); ++index)
    {
        SCOPED_TRACE(index);
        auto document = original;
        ASSERT_NO_FATAL_FAILURE(mutations[index](document));
        EXPECT_THROW(motrack::writeTrackerDocument(destination, document, input), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), "existing");
    }
    EXPECT_THROW(motrack::writeTrackerDocument(destination, original, {}), motrack::ProjectError);
    EXPECT_THROW(motrack::writeTrackerDocument(destination, original, "relative.trk"), motrack::ProjectError);
    EXPECT_THROW(motrack::writeTrackerDocument(temporary.filePath("missing/output.trk"), original, input), motrack::ProjectError);
    EXPECT_THROW(motrack::writeTrackerDocument(temporary.filePath("output.trz"), original, input), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), "existing");
}

TEST(TrackerWriter, AuditsSavedImageFrameCountsAndNeverReplacesALinkedImage)
{
    QTemporaryDir temporary;
    const auto input = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/linked-images.trk";
    auto document = motrack::importTrackerDocument(input);
    const auto output = temporary.filePath("output.trk");
    const std::string start = "<object class=\"org.opensourcephysics.media.core.VideoClip\">";
    const auto position = retainedSource(document).xml.find(start);
    ASSERT_NE(position, std::string::npos);
    retainedSource(document).xml.insert(position + start.size(), "\n<property name=\"video_framecount\" type=\"int\">4</property>\n");
    document.project.clip.sourceFrameCount = 4;
    motrack::writeTrackerDocument(output, document, input);
    const auto correct = readBytes(output);
    EXPECT_NE(correct.find("name=\"video_framecount\""), std::string::npos);
    const std::string count = R"(name="video_framecount" type="int">4)";
    const auto countPosition = retainedSource(document).xml.find(count);
    ASSERT_NE(countPosition, std::string::npos);
    retainedSource(document).xml.replace(countPosition, count.size(), R"(name="video_framecount" type="int">5)");
    EXPECT_THROW(motrack::writeTrackerDocument(output, document, input), motrack::ProjectError);
    EXPECT_EQ(readBytes(output), correct);

    document = motrack::importTrackerDocument(input);
    const auto imageDestination = temporary.filePath("image-with-project-extension.trk");
    ASSERT_TRUE(QFile::copy(QFileInfo(input).absolutePath() + "/images/frame0.png", imageDestination));
    const auto originalImage = readBytes(imageDestination);
    document.project.clip.mediaReferences[0] = imageDestination.toStdString();
    EXPECT_THROW(motrack::writeTrackerDocument(imageDestination, document, input), motrack::ProjectError);
    EXPECT_EQ(readBytes(imageDestination), originalImage);
}

TEST(TrackerWriter, RejectsUnsafeGeneratedImageReferencesWithoutReplacingEitherFile)
{
    QTemporaryDir temporary;
    const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/";
    const auto directory = temporary.filePath("source!");
    ASSERT_TRUE(QDir().mkpath(directory + "/images"));
    ASSERT_TRUE(QDir().mkpath(temporary.filePath("exports")));
    const auto input = directory + "/input.trk";
    ASSERT_TRUE(QFile::copy(fixture + "linked-images.trk", input));
    for (int frame = 0; frame < 4; ++frame)
    {
        const auto name = "images/frame" + QString::number(frame) + ".png";
        ASSERT_TRUE(QFile::copy(fixture + name, directory + '/' + name));
    }
    auto document = motrack::importTrackerDocument(input);
    document.project.pointMasses.front().mass = 5;
    const auto before = document;
    const auto destination = temporary.filePath("exports/saved.trk");
    ASSERT_TRUE(QFile::copy(input, destination));
    const auto previous = readBytes(destination);
    EXPECT_THROW(motrack::writeTrackerDocument(destination, document, input), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), previous);
    EXPECT_EQ(readBytes(input), previous);
    EXPECT_EQ(document.project, before.project);
    if (!before.trackerSource)
        FAIL() << "Expected the original source envelope.";
    const auto expectedXml = before.trackerSource->xml;
    EXPECT_EQ(retainedSource(document).xml, expectedXml);
    EXPECT_EQ(motrack::resolveLocalMediaPath(input, "images/frame0.png"), QFileInfo(directory + "/images/frame0.png").canonicalFilePath());
    EXPECT_TRUE(motrack::resolveLocalMediaPath(destination, "../source!/images/frame0.png").isEmpty());
    EXPECT_TRUE(motrack::resolveLocalMediaPath(temporary.filePath("missing/base.trk"), "frame.png").isEmpty());
}

#ifndef _WIN32
// QFile::link creates POSIX symbolic links here; on Windows it creates shortcuts with different semantics.
TEST(TrackerWriter, RetainsDanglingDirectoryAliasTargetsForRestoredImages)
{
    QTemporaryDir temporary;
    const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/";
    ASSERT_TRUE(QDir(temporary.path()).mkpath("import"));
    ASSERT_TRUE(QDir(temporary.path()).mkpath("exports"));
    const auto media = temporary.filePath("restored");
    const auto alias = temporary.filePath("import/images");
    ASSERT_TRUE(QFile::link(media, alias));
    const auto input = temporary.filePath("import/input.trk");
    ASSERT_TRUE(QFile::copy(fixture + "linked-images.trk", input));
    auto document = motrack::importTrackerDocument(input);
    motrack::retainWorkspaceMediaLocations(document, input);
    const auto output = temporary.filePath("exports/saved.trk");
    EXPECT_THROW(motrack::writeTrackerDocument(output, document, input), motrack::ProjectError);
    EXPECT_FALSE(QFileInfo::exists(output));
    ASSERT_TRUE(QFile::remove(input));
    ASSERT_TRUE(QFile::remove(alias));
    ASSERT_TRUE(QDir(temporary.path()).rmdir("import"));
    ASSERT_TRUE(QDir().mkpath(media));
    for (int frame = 0; frame < 4; ++frame)
    {
        const auto name = "frame" + QString::number(frame) + ".png";
        ASSERT_TRUE(QFile::copy(fixture + "images/" + name, media + '/' + name));
    }
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5));
    EXPECT_NO_THROW(motrack::writeTrackerDocument(output, document, input));
    const auto reopened = motrack::importTrackerDocument(output);
    EXPECT_EQ(reopened.project.pointMasses.front().mass, 5);
    for (int frame = 0; frame < 4; ++frame)
    {
        const auto name = "frame" + QString::number(frame) + ".png";
        const auto& reference = reopened.project.clip.mediaReferences.at(frame);
        EXPECT_EQ(reference, ("../restored/" + name).toStdString());
        EXPECT_EQ(readBytes(motrack::resolveLocalMediaPath(output, reference)), readBytes(fixture + "images/" + name));
    }
}

TEST(TrackerWriter, RebasesPhysicalImagePathsThroughDirectoryAndProjectAliases)
{
    QTemporaryDir temporary;
    const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/";
    const auto source = temporary.filePath("source");
    const auto physical = temporary.filePath("elsewhere/exports");
    const auto assets = temporary.filePath("elsewhere/assets");
    ASSERT_TRUE(QDir().mkpath(source + "/images"));
    ASSERT_TRUE(QDir().mkpath(physical));
    ASSERT_TRUE(QDir().mkpath(assets + "/deep"));
    ASSERT_TRUE(QDir().mkpath(assets + "/images"));
    for (int frame = 0; frame < 4; ++frame)
    {
        const auto name = "images/frame" + QString::number(frame) + ".png";
        ASSERT_TRUE(QFile::copy(fixture + name, source + '/' + name));
        ASSERT_TRUE(QFile::copy(fixture + "images/frame3.png", assets + '/' + name));
    }
    const auto input = source + "/input.trk";
    ASSERT_TRUE(QFile::copy(fixture + "linked-images.trk", input));
    const auto inputAlias = temporary.filePath("input-alias.trk");
    const auto directoryAlias = temporary.filePath("exports-alias");
    ASSERT_TRUE(QFile::link(input, inputAlias));
    ASSERT_TRUE(QFile::link(physical, directoryAlias));
    ASSERT_TRUE(QFile::link(assets + "/deep", source + "/shortcut"));
    auto document = motrack::importTrackerDocument(inputAlias);
    document.project.pointMasses.front().mass = 5;
    const auto destination = directoryAlias + "/saved.trk";
    for (const bool redirected : {false, true})
    {
        SCOPED_TRACE(redirected);
        for (std::size_t frame = 0; frame < 4; ++frame)
            document.project.clip.mediaReferences[frame] =
                (redirected ? "shortcut/../images/frame" : "images/frame") + std::to_string(frame) + ".png";
        const auto before = document.project;
        motrack::writeTrackerDocument(destination, document, inputAlias);
        const auto exported = motrack::parseLegacyProject(readBytes(destination));
        for (std::size_t frame = 0; frame < 4; ++frame)
        {
            const auto expected = (redirected ? assets : source) + "/images/frame" + QString::number(frame) + ".png";
            const auto& reference = exported.clip.mediaReferences[frame];
            EXPECT_EQ(motrack::resolveLocalMediaPath(destination, reference), QFileInfo(expected).canonicalFilePath());
            EXPECT_EQ(motrack::resolveLocalMediaPath(physical + "/saved.trk", reference), QFileInfo(expected).canonicalFilePath());
            EXPECT_EQ(readBytes(motrack::resolveLocalMediaPath(destination, reference)), readBytes(expected));
        }
        EXPECT_EQ(document.project, before);
    }
    const auto outputAlias = temporary.filePath("output-alias.trk");
    ASSERT_TRUE(QFile::link(destination, outputAlias));
    const auto bytes = readBytes(destination);
    EXPECT_THROW(motrack::writeTrackerDocument(outputAlias, document, inputAlias), motrack::ProjectError);
    EXPECT_EQ(readBytes(outputAlias), bytes);
    EXPECT_EQ(readBytes(destination), bytes);
}
#endif

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

std::string namedSource(const std::string& extra = "")
{
    // Opaque labels exercise name isolation; an empty pattern requests no numeric override.
    return "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>\n"
           "<property name='description' type='string'><![CDATA[mass A and mass B are words in this note.]]></property>\n"
           "<property name='tracks' type='collection' class='java.util.ArrayList'>\n"
           "<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>\n"
           "<property name='name' type='string'>mass A</property><property name='mass' type='double'>1</property>\n"
           "</object></property>\n"
           "<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.Protractor'>\n"
           "<property name='name' type='string'>protractor</property>\n"
           "<property name='framedata' type='array' class='[[D'><property name='array' "
           "type='string'>{{0,0,1,0,0,1}}</property></property>\n"
           "</object></property>\n"
           "<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>\n"
           "<property name='name' type='string'>mass B</property></object></property></property>\n"
           "<property name='selectedtrack' type='string'>mass B</property>\n"
           "<property name='selected_track_views' type='string'>mass B,null;null,mass B;null,null;null,null</property>\n"
           "<property name='views' type='array' class='[[Lorg.opensourcephysics.cabrillo.tracker.TView;'>\n"
           "<property name='[1]' type='array' class='[Lorg.opensourcephysics.cabrillo.tracker.TView;'>\n"
           "<property name='[1]' type='object'><object class='org.opensourcephysics.cabrillo.tracker.TableTView'>\n"
           "<property name='selected_track' type='string'>mass B</property>\n"
           "<property name='track_columns' type='array' class='[[Ljava.lang.String;'>\n"
           "<property name='[0]' type='array' class='[Ljava.lang.String;'>\n"
           "<property name='[0]' type='string'>mass B</property><property name='[1]' type='string'>mass B</property>\n"
           "</property></property>\n"
           "<property name='column_formats' type='array' class='[[[Ljava.lang.String;'>\n"
           "<property name='[0]' type='array' class='[[Ljava.lang.String;'><property name='[0]' type='array' class='[Ljava.lang.String;'>\n"
           "<property name='[0]' type='string'>mass B</property><property name='[1]' type='string'>mass B</property>\n"
           "<property name='[2]' type='string'></property></property></property></property>\n"
           "</object></property></property></property>\n" +
           extra + "</object>\n";
}

TEST(TrackerWriter, DeletedSelectionsUseRenamedSurvivorsOrRetainedToolsAfterNativeReopen)
{
    auto xml = namedSource();
    const std::string oldOwner = "<property name='[0]' type='string'>mass B</property>";
    const std::string toolOwner = "<property name='[0]' type='string'>protractor</property>";
    for (auto position = xml.find(oldOwner); position != std::string::npos; position = xml.find(oldOwner, position + toolOwner.size()))
        xml.replace(position, oldOwner.size(), toolOwner);
    const auto imported = motrack::parseTrackerProject(xml);
    for (const bool allMasses : {false, true})
    {
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.removePointMass(document.project, 1));
        ASSERT_TRUE(edits.undo(document.project));
        EXPECT_EQ(motrack::renderTrackerProject(document.project, imported.source), xml);
        ASSERT_TRUE(edits.redo(document.project));
        if (allMasses)
            ASSERT_TRUE(edits.removePointMass(document.project, 0));
        else
            ASSERT_TRUE(edits.renamePointMass(document.project, 0, "renamed survivor"));
        QTemporaryDir temporary;
        const auto native = temporary.filePath("selected-deletion.motrack");
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Native reopen lost the original selections.";
        }
        EXPECT_EQ(document.trackerSource->xml, xml);
        const auto output = motrack::parseTrackerProject(motrack::renderTrackerProject(document.project, *document.trackerSource));
        EXPECT_EQ(output.project.pointMasses.size(), allMasses ? 0u : 1u);
        const auto references = motrack::indexTrackerReferences(output.source, output.project);
        EXPECT_TRUE(references.malformedPaths.empty());
        EXPECT_EQ(output.source.xml.find("name=\"selectedtrack\""), std::string::npos);
        int replaced = 0;
        for (const auto& reference : references.references)
        {
            if ((reference.kind == motrack::TrackerReferenceKind::PlotSelection ||
                 reference.kind == motrack::TrackerReferenceKind::TableSelection) &&
                reference.resolution == motrack::TrackerReferenceResolution::Resolved)
            {
                EXPECT_EQ(reference.originalName, allMasses ? "protractor" : "renamed survivor");
                ++replaced;
            }
        }
        EXPECT_EQ(replaced, 3);
        EXPECT_EQ(motrack::renderTrackerProject(output.project, output.source), output.source.xml);
    }
}

TEST(TrackerWriter, DeletingAllSelectedVectorsClearsSelectionsWithoutChoosingAxes)
{
    auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk");
    xml.insert(xml.rfind("</object>"),
               "<property name='selectedtrack' type='string'>vector A</property>\n"
               "<property name='selected_track_views' type='string'>vector A,null;null,vector B;null,null;null,null</property>\n"
               "<property name='views' type='array' class='[[Lorg.opensourcephysics.cabrillo.tracker.TView;'>"
               "<property name='[1]' type='array' class='[Lorg.opensourcephysics.cabrillo.tracker.TView;'>"
               "<property name='[1]' type='object'><object class='org.opensourcephysics.cabrillo.tracker.TableTView'>"
               "<property name='selected_track' type='string'>vector B</property></object></property></property></property>\n");
    const auto imported = motrack::parseTrackerProject(xml);
    auto project = imported.project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.removeVector(project, 0));
    ASSERT_TRUE(edits.removeVector(project, 0));
    const auto output = motrack::parseTrackerProject(motrack::renderTrackerProject(project, imported.source));
    EXPECT_TRUE(output.project.vectors.empty());
    EXPECT_EQ(output.source.xml.find("name=\"selectedtrack\""), std::string::npos);
    EXPECT_EQ(output.source.xml.find("name=\"selected_track\""), std::string::npos);
    EXPECT_EQ(output.source.xml.find("TableTView"), std::string::npos);
    const auto references = motrack::indexTrackerReferences(output.source, output.project);
    ASSERT_EQ(references.references.size(), 8);
    for (const auto& reference : references.references)
        EXPECT_EQ(reference.resolution, motrack::TrackerReferenceResolution::NoTarget);
    ASSERT_TRUE(edits.createVector(project, "replacement"));
    const auto withNewTrack = motrack::parseTrackerProject(motrack::renderTrackerProject(project, imported.source));
    const auto newReferences = motrack::indexTrackerReferences(withNewTrack.source, withNewTrack.project);
    int fallbackCount = 0;
    for (const auto& reference : newReferences.references)
    {
        if (reference.resolution == motrack::TrackerReferenceResolution::Resolved)
        {
            EXPECT_EQ(reference.originalName, "replacement");
            ++fallbackCount;
        }
    }
    EXPECT_EQ(fallbackCount, 3);
}

TEST(TrackerWriter, DeletesAnUnreferencedMassAfterNativeReopenAndRestoresItThroughHistory)
{
    const auto imported = motrack::parseTrackerProject(namedSource());
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.removePointMass(document.project, 0));
    ASSERT_TRUE(edits.undo(document.project));
    EXPECT_EQ(motrack::renderTrackerProject(document.project, imported.source), imported.source.xml);
    ASSERT_TRUE(edits.redo(document.project));
    QTemporaryDir temporary;
    const auto native = temporary.filePath("deleted.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = motrack::readWorkspaceDocument(native);
    if (!document.trackerSource || !document.trackerSource->bindings)
    {
        FAIL() << "Native reopen lost the original deleted-track association.";
    }
    EXPECT_EQ(document.trackerSource->xml, imported.source.xml);
    EXPECT_EQ(document.trackerSource->bindings, imported.source.bindings);
    const auto output = motrack::renderTrackerProject(document.project, *document.trackerSource);
    EXPECT_EQ(output, motrack::renderTrackerProject(document.project, *document.trackerSource));
    const auto reparsed = motrack::parseTrackerProject(output);
    ASSERT_EQ(reparsed.project.pointMasses.size(), 1);
    EXPECT_EQ(reparsed.project.pointMasses.front().name, "mass B");
    EXPECT_EQ(reparsed.project.pointMasses.front().id, 1); // Native ID 2 became parsed ID 1.
    EXPECT_EQ(reparsed.project.unsupportedTrackTypes, imported.project.unsupportedTrackTypes);
    EXPECT_EQ(motrack::renderTrackerProject(reparsed.project, reparsed.source), output);
    const auto index = motrack::indexTrackerReferences(reparsed.source, reparsed.project);
    EXPECT_TRUE(std::ranges::all_of(index.references,
                                    [](const auto& reference)
                                    {
                                        return reference.resolution == motrack::TrackerReferenceResolution::Resolved ||
                                               reference.resolution == motrack::TrackerReferenceResolution::NoTarget;
                                    }));
}

TEST(TrackerWriter, VectorDeletionKeepsTheSurvivingSamplesAndNativeHistory)
{
    const auto imported =
        motrack::parseTrackerProject(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk"));
    for (std::size_t deleted = 0; deleted < 2; ++deleted)
    {
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.removeVector(document.project, deleted));
        ASSERT_TRUE(edits.undo(document.project));
        EXPECT_EQ(motrack::renderTrackerProject(document.project, imported.source), imported.source.xml);
        ASSERT_TRUE(edits.redo(document.project));
        QTemporaryDir temporary;
        const auto native = temporary.filePath("deleted-vector.motrack");
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Native reopen lost vector source associations.";
        }
        const auto bytes = motrack::renderTrackerProject(document.project, *document.trackerSource);
        const auto output = motrack::parseTrackerProject(bytes);
        ASSERT_EQ(output.project.vectors.size(), 1);
        EXPECT_EQ(output.project.vectors.front().name, imported.project.vectors[1 - deleted].name);
        EXPECT_EQ(output.project.vectors.front().samples, imported.project.vectors[1 - deleted].samples);
        EXPECT_EQ(motrack::renderTrackerProject(document.project, *document.trackerSource), bytes);
        EXPECT_EQ(document.trackerSource->xml, imported.source.xml);
    }
}

TEST(TrackerWriter, DeletionCompactsParsedIdsAcrossInterleavedMassesAndVectors)
{
    std::string xml = "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>\n"
                      "<property name='tracks' type='collection' class='java.util.ArrayList'>\n";
    for (int index = 0; index < 5; ++index)
    {
        xml += "<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.";
        xml += index % 2 == 0 ? "PointMass" : "Vector";
        xml += "'><property name='name' type='string'>track " + std::to_string(index) + "</property></object></property>\n";
    }
    xml += "</property></object>\n";
    const auto imported = motrack::parseTrackerProject(xml);
    for (unsigned mask = 1; mask < 32; ++mask)
    {
        SCOPED_TRACE(mask);
        auto project = imported.project;
        const auto removed = [mask](const auto& track)
        {
            return (mask & (1u << (track.id - 1))) != 0;
        };
        std::erase_if(project.pointMasses, removed);
        std::erase_if(project.vectors, removed);
        std::ranges::reverse(project.pointMasses);
        std::ranges::reverse(project.vectors);
        const auto output = motrack::renderTrackerProject(project, imported.source);
        const auto result = motrack::parseTrackerProject(output);
        EXPECT_EQ(result.project.pointMasses.size(), project.pointMasses.size());
        EXPECT_EQ(result.project.vectors.size(), project.vectors.size());
        for (const auto& track : project.pointMasses)
            EXPECT_NE(std::ranges::find(result.project.pointMasses, track.name, &motrack::PointMass::name),
                      result.project.pointMasses.end());
        for (const auto& track : project.vectors)
            EXPECT_NE(std::ranges::find(result.project.vectors, track.name, &motrack::VectorTrack::name), result.project.vectors.end());
        EXPECT_EQ(motrack::renderTrackerProject(result.project, result.source), output);
    }
}

TEST(TrackerWriter, DeletionAndCreationWithEqualCountsUseIdentityAndPermitSafeNameReuse)
{
    const auto imported = motrack::parseTrackerProject(namedSource());
    auto project = imported.project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.removePointMass(project, 0));
    ASSERT_TRUE(edits.createPointMass(project, "discarded"));
    ASSERT_TRUE(edits.undo(project));
    ASSERT_TRUE(edits.createPointMass(project, "mass A"));
    ASSERT_TRUE(edits.setPointMassValue(project, 1, 3));
    auto output = motrack::parseTrackerProject(motrack::renderTrackerProject(project, imported.source));
    ASSERT_EQ(output.project.pointMasses.size(), 2);
    EXPECT_EQ(output.project.pointMasses[0].name, "mass B");
    EXPECT_EQ(output.project.pointMasses[1].name, "mass A");
    EXPECT_DOUBLE_EQ(output.project.pointMasses[1].mass, 3);
    ASSERT_TRUE(edits.removePointMass(project, 1));
    ASSERT_TRUE(edits.renamePointMass(project, 0, "mass A"));
    output = motrack::parseTrackerProject(motrack::renderTrackerProject(project, imported.source));
    ASSERT_EQ(output.project.pointMasses.size(), 1);
    EXPECT_EQ(output.project.pointMasses[0].name, "mass A");
    const auto index = motrack::indexTrackerReferences(output.source, output.project);
    for (const auto& reference : index.references)
    {
        if (reference.resolution != motrack::TrackerReferenceResolution::NoTarget)
        {
            EXPECT_EQ(reference.resolution, motrack::TrackerReferenceResolution::Resolved);
            EXPECT_EQ(reference.originalName, "mass A");
        }
    }
}

TEST(TrackerWriter, RejectsKnownLossyTableColumnsUntilAllAffectedOwnersAreRemoved)
{
    const auto fixtureDirectory = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/";
    for (const auto* fixture :
         {"table-owners-subset.trk", "table-owners-defaults.trk", "table-owners-legacy-formats.trk", "table-owners.trk"})
    {
        SCOPED_TRACE(fixture);
        auto imported = motrack::parseTrackerProject(readBytes(fixtureDirectory + fixture));
        EXPECT_EQ(motrack::renderTrackerProject(imported.project, imported.source), imported.source.xml);
        const auto original = imported.source.xml;
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setPointMassValue(imported.project, 0, 5.0));
        QTemporaryDir temporary;
        const auto destination = temporary.filePath("existing.trk");
        {
            QFile output(destination);
            ASSERT_TRUE(output.open(QIODevice::WriteOnly));
            ASSERT_EQ(output.write("existing"), 8);
        }
        try
        {
            motrack::writeTrackerProject(destination, imported.project, imported.source);
            FAIL() << "A known lossy table must not replace the destination.";
        }
        catch (const motrack::ProjectError& error)
        {
            const auto owner = std::string(fixture) == "table-owners-legacy-formats.trk" ? "mass B" : "mass A";
            EXPECT_NE(std::string(error.what()).find(owner), std::string::npos);
            EXPECT_NE(std::string(error.what()).find("#64"), std::string::npos);
        }
        EXPECT_EQ(readBytes(destination), "existing");
        ASSERT_TRUE(edits.removePointMass(imported.project, 0));
        EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(imported.project, imported.source)), motrack::ProjectError);
        ASSERT_TRUE(edits.removePointMass(imported.project, 0));
        EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(imported.project, imported.source)), motrack::ProjectError);
        ASSERT_TRUE(edits.removePointMass(imported.project, 0));
        const auto cleaned = motrack::renderTrackerProject(imported.project, imported.source);
        EXPECT_TRUE(motrack::parseLegacyProject(cleaned).pointMasses.empty());
        EXPECT_EQ(cleaned.find("column_formats"), std::string::npos);
        EXPECT_EQ(cleaned.find("track_columns"), std::string::npos);
        ASSERT_TRUE(edits.undo(imported.project));
        EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(imported.project, imported.source)), motrack::ProjectError);
        ASSERT_TRUE(edits.redo(imported.project));
        EXPECT_EQ(motrack::renderTrackerProject(imported.project, imported.source), cleaned);
        EXPECT_EQ(imported.source.xml, original);
    }
}

TEST(TrackerWriter, RejectsMalformedOrCustomPointMassNumberFormats)
{
    QDomDocument baseline;
    ASSERT_TRUE(baseline.setContent(
        QByteArray::fromStdString(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/table-default-format-owners.trk"))));
    QDomDocument defaults;
    ASSERT_TRUE(defaults.setContent(
        QString("<property name='number_formats' type='array' class='[Ljava.lang.String;'>"
                "<property name='[0]' type='string'>frame</property><property name='[1]' type='string'>0</property>"
                "<property name='[2]' type='string'>t</property><property name='[3]' type='string'>0.000</property></property>")));
    const auto objects = baseline.elementsByTagName("object");
    bool added = false;
    for (int i = 0; i < objects.size() && !added; ++i)
    {
        auto object = objects.at(i).toElement();
        if (object.attribute("class") == "org.opensourcephysics.cabrillo.tracker.PointMass")
        {
            object.appendChild(baseline.importNode(defaults.documentElement(), true));
            added = true;
        }
    }
    ASSERT_TRUE(added);
    const auto original = baseline.toByteArray(-1).toStdString();
    const std::vector<std::function<void(QDomElement&)>> mutations{
        [](auto& formats) { formats.removeChild(formats.lastChildElement()); },
        [](auto& formats) { formats.lastChildElement().setAttribute("name", "[9]"); },
        [](auto& formats) { formats.firstChildElement().firstChild().setNodeValue("temperature"); },
        [](auto& formats) { formats.firstChildElement().nextSiblingElement().firstChild().setNodeValue("invalid["); },
        [](auto& formats) { formats.firstChildElement().nextSiblingElement().firstChild().setNodeValue("0\n"); },
        [](auto& formats) { formats.firstChildElement().nextSiblingElement().firstChild().setNodeValue("0.0000"); },
        [](auto& formats)
        {
            formats.lastChildElement().previousSiblingElement().firstChild().setNodeValue("frame");
        }};
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    auto valid = motrack::parseTrackerProject(original);
    valid.project.pointMasses.front().mass = 5;
    ASSERT_NO_THROW(motrack::writeTrackerProject(destination, valid.project, valid.source));
    const auto saved = readBytes(destination);
    ASSERT_DOUBLE_EQ(motrack::parseLegacyProject(saved).pointMasses.front().mass, 5);
    for (std::size_t i = 0; i < mutations.size(); ++i)
    {
        SCOPED_TRACE(i);
        QDomDocument xml;
        ASSERT_TRUE(xml.setContent(QByteArray::fromStdString(original)));
        const auto properties = xml.elementsByTagName("property");
        QDomElement formats;
        for (int index = 0; index < properties.size() && formats.isNull(); ++index)
            if (properties.at(index).toElement().attribute("name") == "number_formats")
                formats = properties.at(index).toElement();
        ASSERT_FALSE(formats.isNull());
        mutations[i](formats);
        auto imported = motrack::parseTrackerProject(xml.toByteArray(-1).toStdString());
        imported.project.pointMasses.front().mass = 5;
        ASSERT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), saved);
    }
}

TEST(TrackerWriter, RemovesOnlyDeletedTableOwnersAfterHistoryNativeReopenAndNameReuse)
{
    const auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/table-default-format-owners.trk");
    const auto imported = motrack::parseTrackerProject(xml);
    for (unsigned removed = 1; removed < 8; ++removed)
    {
        SCOPED_TRACE(removed);
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        motrack::TrackEdits edits;
        std::map<std::string, unsigned> expectedFormats;
        for (std::size_t i = 3; i-- > 0;)
        {
            if ((removed & (1u << i)) != 0)
            {
                ASSERT_TRUE(edits.removePointMass(document.project, i));
                ASSERT_TRUE(edits.undo(document.project));
                ASSERT_TRUE(edits.redo(document.project));
            }
            else
            {
                const auto name = "surviving " + document.project.pointMasses[i].name;
                ASSERT_TRUE(edits.renamePointMass(document.project, i, name));
                expectedFormats.emplace(name, i == 2 ? 1u : 2u);
            }
        }
        const std::string reused = (removed & 1u) != 0 ? "mass A" : (removed & 2u) != 0 ? "mass B" : "mass C";
        ASSERT_TRUE(edits.createPointMass(document.project, reused));
        auto expected = document.project.pointMasses;
        std::ranges::reverse(document.project.pointMasses);
        QTemporaryDir temporary;
        const auto native = temporary.filePath("table-deletion.motrack");
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Native reopen lost the original table source.";
        }
        const auto bytes = motrack::renderTrackerProject(document.project, *document.trackerSource);
        const auto output = motrack::parseTrackerProject(bytes);
        ASSERT_EQ(output.project.pointMasses.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i)
        {
            expected[i].id = output.project.pointMasses[i].id;
            EXPECT_EQ(output.project.pointMasses[i], expected[i]);
        }
        const auto references = motrack::indexTrackerReferences(output.source, output.project);
        EXPECT_TRUE(references.malformedPaths.empty());
        std::map<std::string, unsigned> formats;
        std::map<std::string, unsigned> columns;
        for (const auto& reference : references.references)
        {
            EXPECT_EQ(reference.resolution,
                      reference.originalName == "null" ? motrack::TrackerReferenceResolution::NoTarget
                                                       : motrack::TrackerReferenceResolution::Resolved);
            if (reference.kind == motrack::TrackerReferenceKind::TableColumnOwner)
                ++columns[reference.originalName];
            if (reference.kind == motrack::TrackerReferenceKind::TableFormatOwner)
                ++formats[reference.originalName];
        }
        EXPECT_EQ(formats, expectedFormats);
        ASSERT_EQ(columns.size(), expectedFormats.size());
        for (const auto& [name, count] : columns)
        {
            EXPECT_TRUE(expectedFormats.contains(name));
            EXPECT_EQ(count, 1);
        }
        EXPECT_EQ(document.trackerSource->xml, xml);
        EXPECT_EQ(motrack::renderTrackerProject(document.project, *document.trackerSource), bytes);
        EXPECT_EQ(motrack::renderTrackerProject(output.project, output.source), bytes);
    }
}

TEST(TrackerWriter, OmitsAnEmptyCustomTableAfterDeletingItsLastOwner)
{
    const auto path = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/table-default-format-owners.trk";
    for (int arrays = 0; arrays < 3; ++arrays)
    {
        for (const bool retainOtherViews : {false, true})
        {
            SCOPED_TRACE(arrays);
            SCOPED_TRACE(retainOtherViews);
            QDomDocument source;
            ASSERT_TRUE(source.setContent(QByteArray::fromStdString(readBytes(path))));
            const auto objects = source.elementsByTagName("object");
            QDomElement table;
            for (int i = 0; i < objects.size(); ++i)
                if (objects.at(i).toElement().attribute("class") == "org.opensourcephysics.cabrillo.tracker.TableTView")
                    table = objects.at(i).toElement();
            ASSERT_FALSE(table.isNull());
            if (arrays != 0)
            {
                for (auto child = table.firstChildElement(); !child.isNull();)
                {
                    const auto next = child.nextSiblingElement();
                    if (child.attribute("name") != "selected_track")
                    {
                        if (arrays == 1)
                            table.removeChild(child);
                        else
                            while (!child.firstChild().isNull())
                                child.removeChild(child.firstChild());
                    }
                    child = next;
                }
            }
            if (retainOtherViews)
            {
                QDomDocument other;
                ASSERT_TRUE(other.setContent(QString(
                    "<property name='[0]' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PlotTView'/></property>")));
                auto chooser = table.parentNode().parentNode();
                chooser.appendChild(source.importNode(other.documentElement(), true));
                auto secondChooser = source.createElement("property");
                secondChooser.setAttribute("name", "[3]");
                secondChooser.setAttribute("type", "array");
                secondChooser.setAttribute("class", "[Lorg.opensourcephysics.cabrillo.tracker.TView;");
                secondChooser.appendChild(source.importNode(other.documentElement(), true));
                chooser.parentNode().appendChild(secondChooser);
            }
            const auto original = source.toByteArray().toStdString();
            const auto imported = motrack::parseTrackerProject(original);
            motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
            motrack::TrackEdits edits;
            for (int remaining = 3; remaining > 0; --remaining)
                ASSERT_TRUE(edits.removePointMass(document.project, 0));
            ASSERT_TRUE(edits.undo(document.project));
            ASSERT_TRUE(edits.redo(document.project));
            QTemporaryDir temporary;
            const auto native = temporary.filePath("empty-table.motrack");
            motrack::writeWorkspaceDocument(native, document);
            document = motrack::readWorkspaceDocument(native);
            if (!document.trackerSource)
            {
                FAIL() << "Native reopen lost the source table state.";
            }
            const auto retained = *document.trackerSource;
            const auto bytes = motrack::renderTrackerProject(document.project, retained);
            EXPECT_EQ(bytes.find("TableTView"), std::string::npos);
            QDomDocument parsed;
            ASSERT_TRUE(parsed.setContent(QByteArray::fromStdString(bytes)));
            QDomElement views;
            for (auto property = parsed.documentElement().firstChildElement(); !property.isNull(); property = property.nextSiblingElement())
                if (property.attribute("name") == "views")
                    views = property;
            EXPECT_EQ(views.isNull(), !retainOtherViews);
            if (retainOtherViews)
            {
                const auto first = views.firstChildElement();
                const auto last = first.nextSiblingElement();
                EXPECT_EQ(first.attribute("name"), "[0]");
                EXPECT_EQ(last.attribute("name"), "[3]");
                EXPECT_TRUE(last.nextSiblingElement().isNull());
                for (const auto& chooser : {first, last})
                {
                    const auto slot = chooser.firstChildElement();
                    EXPECT_EQ(slot.attribute("name"), "[0]");
                    EXPECT_TRUE(slot.nextSiblingElement().isNull());
                    EXPECT_EQ(slot.firstChildElement().attribute("class"), "org.opensourcephysics.cabrillo.tracker.PlotTView");
                }
            }
            EXPECT_TRUE(motrack::parseLegacyProject(bytes).pointMasses.empty());
            EXPECT_EQ(retained.xml, original);
            EXPECT_NE(bytes.find("selected_view_types"), std::string::npos);
            EXPECT_NE(bytes.find("null,null;null,null;null,null;null,null"), std::string::npos);
        }
    }
}

TEST(TrackerWriter, RefusesDependentAndTableDatasetDeletionBeforeTouchingTheDestination)
{
    auto xml = namedSource();
    const auto tableColumns = xml.find("<property name='track_columns'");
    ASSERT_NE(tableColumns, std::string::npos);
    xml.insert(tableColumns,
               "<property name='dataset_indices' type='array' class='[[Ljava.lang.String;'>"
               "<property name='[0]' type='array' class='[Ljava.lang.String;'>"
               "<property name='[0]' type='string'>mass B</property><property name='[1]' type='string'>0</property>"
               "</property></property>");
    const auto imported = motrack::parseTrackerProject(xml);
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(destination, imported.project, imported.source);
    auto project = imported.project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.removePointMass(project, 1));
    EXPECT_THROW(motrack::writeTrackerProject(destination, project, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), imported.source.xml);
    ASSERT_TRUE(edits.createPointMass(project, "mass B"));
    EXPECT_THROW(motrack::writeTrackerProject(destination, project, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), imported.source.xml);
    const auto dependent =
        motrack::parseTrackerProject(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/center-of-mass.trk"));
    project = dependent.project;
    edits.clear();
    ASSERT_TRUE(edits.removePointMass(project, 0));
    EXPECT_THROW(motrack::writeTrackerProject(destination, project, dependent.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), imported.source.xml);
}

TEST(TrackerWriter, CreatesMarkedAndEmptyMassesAfterNativeReopenWithStableHistoryAndRepeatedExport)
{
    const auto imported = motrack::parseTrackerProject(namedSource());
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createPointMass(document.project, "discarded"));
    ASSERT_TRUE(edits.undo(document.project));
    const auto name = QString::fromUtf8("nouvelle β & <C>").toStdString();
    ASSERT_TRUE(edits.createPointMass(document.project, name));
    const auto id = document.project.pointMasses.back().id;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 2, 3));
    ASSERT_TRUE(edits.markPointMass(document.project, 2, {.frame = 1, .x = 330, .y = 220}));
    ASSERT_TRUE(edits.markPointMass(document.project, 2, {.frame = 4, .x = 340, .y = 210}));
    ASSERT_TRUE(edits.markPointMass(document.project, 2, {.frame = 8, .x = 360, .y = 230}));
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    ASSERT_TRUE(edits.removePointMass(document.project, 2));
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.createPointMass(document.project, "empty"));
    std::ranges::reverse(document.project.pointMasses);
    QTemporaryDir temporary;
    const auto native = temporary.filePath("created.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = motrack::readWorkspaceDocument(native);
    if (!document.trackerSource)
    {
        FAIL() << "Native reopen lost the original source.";
    }
    const auto first = motrack::renderTrackerProject(document.project, *document.trackerSource);
    EXPECT_EQ(first, motrack::renderTrackerProject(document.project, *document.trackerSource));
    EXPECT_EQ(document.trackerSource->xml, imported.source.xml);
    const auto added = std::ranges::find(document.project.pointMasses, id, &motrack::PointMass::id);
    ASSERT_NE(added, document.project.pointMasses.end());
    const auto output = motrack::parseTrackerProject(first);
    ASSERT_EQ(output.project.pointMasses.size(), 4);
    EXPECT_EQ(output.project.pointMasses[2].name, name);
    EXPECT_EQ(output.project.pointMasses[2].mass, 3);
    EXPECT_EQ(output.project.pointMasses[2].marks, added->marks);
    EXPECT_EQ(output.project.pointMasses[3].name, "empty");
    EXPECT_TRUE(output.project.pointMasses[3].marks.empty());
    EXPECT_EQ(motrack::renderTrackerProject(output.project, output.source), first);
    const auto references = motrack::indexTrackerReferences(output.source, output.project);
    const auto originalReferences = motrack::indexTrackerReferences(imported.source, imported.project);
    ASSERT_EQ(references.references.size(), originalReferences.references.size());
    for (std::size_t i = 0; i < references.references.size(); ++i)
    {
        EXPECT_EQ(references.references[i].originalName, originalReferences.references[i].originalName);
        EXPECT_EQ(references.references[i].candidateSourceIndices, originalReferences.references[i].candidateSourceIndices);
        EXPECT_EQ(references.references[i].resolution, originalReferences.references[i].resolution);
    }
    ASSERT_TRUE(edits.undo(document.project)); // Identity-bound history survives storage and reordering.
    EXPECT_EQ(motrack::parseLegacyProject(motrack::renderTrackerProject(document.project, *document.trackerSource)).pointMasses.size(), 3);
}

TEST(TrackerWriter, RejectsUnsafeCreatedTrackNamesReferencesAndSourceSchemasAtomically)
{
    const auto original = namedSource();
    std::vector<std::string> sources{original};
    for (const auto& [before, after] : std::vector<std::pair<std::string, std::string>>{
             {"<property name='selectedtrack' type='string'>mass B", "<property name='selectedtrack' type='string'>new mass"},
             {"<property name='name' type='string'>mass B", "<property name='name' type='string'>mass A"},
             {"<property name='name' type='string'>protractor</property>", ""},
             {"tracker.Protractor", "tracker.Unknown"},
             {"name='tracks' type='collection' class='java.util.ArrayList'",
              "name='tracks' type='collection' class='java.util.LinkedList'"},
             {"</object>\n", "<property name='unknown_link' type='string'>new mass</property></object>\n"},
             {"<property name='item' type='object'>", "<!-- collection comment --><property name='item' type='object'>"}})
    {
        auto xml = original;
        const auto position = xml.find(before);
        ASSERT_NE(position, std::string::npos);
        xml.replace(position, before.size(), after);
        sources.push_back(std::move(xml));
    }
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    for (std::size_t i = 0; i < sources.size(); ++i)
    {
        SCOPED_TRACE(i);
        auto imported = motrack::parseTrackerProject(sources[i]);
        motrack::writeTrackerProject(destination, imported.project, imported.source);
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.createPointMass(imported.project, i == 0 ? "protractor" : "new mass"));
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), sources[i]);
    }
    for (const auto& name : {"", " mass ", "null", "mass A", "new,mass", "new;mass", "new]]>mass", "new\nmass"})
    {
        auto imported = motrack::parseTrackerProject(original);
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.createPointMass(imported.project, "temporary"));
        imported.project.pointMasses.back().name = name;
        EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(imported.project, imported.source)), motrack::ProjectError) << name;
    }
}

TEST(TrackerWriter, CreationDoesNotAuthorizeInvalidMarksOrMedia)
{
    const auto imported = motrack::parseTrackerProject(namedSource());
    for (const auto& mutate :
         std::vector<std::function<void(motrack::LegacyProject&)>>{
             [](auto& project) { project.pointMasses.back().marks = {{.frame = 500001, .x = 1, .y = 2}}; },
             [](auto& project) { project.pointMasses.back().marks = {{.frame = 1, .x = std::numeric_limits<double>::infinity(), .y = 2}}; },
             [](auto& project) { project.pointMasses.back().mass = 0; },
             [](auto& project)
             {
                 project.clip.mediaReferences.push_back("movie.mp4");
             }})
    {
        auto project = imported.project;
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.createPointMass(project, "new mass"));
        mutate(project);
        EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(project, imported.source)), motrack::ProjectError);
    }
}

TEST(TrackerWriter, BoundsNewMassDenseArraysAndSupportsCreationWithExistingRename)
{
    auto imported = motrack::parseTrackerProject(namedSource());
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createPointMass(imported.project, "new mass"));
    ASSERT_TRUE(edits.renamePointMass(imported.project, 1, "renamed old mass"));
    const auto renamed = motrack::parseTrackerProject(motrack::renderTrackerProject(imported.project, imported.source));
    EXPECT_EQ(renamed.project.pointMasses[1].name, "renamed old mass");
    EXPECT_EQ(renamed.project.pointMasses[2].name, "new mass");
    ASSERT_TRUE(edits.markPointMass(imported.project, 2, {.frame = 500000, .x = 1, .y = 2}));
    ASSERT_TRUE(edits.createPointMass(imported.project, "second mass"));
    ASSERT_TRUE(edits.markPointMass(imported.project, 3, {.frame = 500000, .x = 3, .y = 4}));
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    const auto original = motrack::parseTrackerProject(namedSource());
    motrack::writeTrackerProject(destination, original.project, original.source);
    EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), namedSource());
}

TEST(TrackerWriter, CreatesAnotherMassAfterStandardReimportWithHighOriginalFrames)
{
    for (const int frame : {10000, 500000})
    {
        SCOPED_TRACE(frame);
        auto imported = motrack::parseTrackerProject(namedSource());
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.createPointMass(imported.project, "high frame"));
        ASSERT_TRUE(edits.markPointMass(imported.project, 2, {.frame = frame, .x = 2, .y = 3}));
        imported = motrack::parseTrackerProject(motrack::renderTrackerProject(imported.project, imported.source));
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        QTemporaryDir temporary;
        const auto native = temporary.filePath("high-frame.motrack");
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Native reopen lost the reimported source.";
        }
        edits.clear();
        ASSERT_TRUE(edits.createPointMass(document.project, "next mass"));
        const auto result = motrack::parseTrackerProject(motrack::renderTrackerProject(document.project, *document.trackerSource));
        ASSERT_EQ(result.project.pointMasses.size(), 4);
        EXPECT_EQ(result.project.pointMasses[2].marks, imported.project.pointMasses[2].marks);
        EXPECT_EQ(result.project.pointMasses[3].name, "next mass");
        EXPECT_EQ(motrack::renderTrackerProject(result.project, result.source), result.source.xml);
    }
}

TEST(TrackerWriter, CreatedMassCountsRetainedFrameArraysAndNullPlaceholdersBeforeReplacingDestination)
{
    for (const bool nullFrames : {false, true})
    {
        const auto sourceFor = [&](int lastFrame)
        {
            std::string xml = "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>\n"
                              "<property name='tracks' type='collection' class='java.util.ArrayList'>\n";
            for (int index = 0; index < 2; ++index)
            {
                xml += "<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>\n"
                       "<property name='name' type='string'>mass " +
                       std::to_string(index) +
                       "</property>\n"
                       "<property name='framedata' type='array' class='[Lorg.opensourcephysics.cabrillo.tracker.PointMass$FrameData;'>\n"
                       "<property name='[" +
                       std::to_string(lastFrame) + "]' type='object'>";
                xml += nullFrames ? "null"
                                  : "<object class='org.opensourcephysics.cabrillo.tracker.PointMass$FrameData'>"
                                    "<property name='x' type='double'>1</property><property name='y' type='double'>2</property>"
                                    "</object>";
                xml += "</property>\n</property>\n</object></property>\n";
            }
            return xml + "</property>\n</object>\n";
        };
        QTemporaryDir temporary;
        const auto destination = temporary.filePath("existing.trk");
        auto imported = motrack::parseTrackerProject(sourceFor(500000));
        motrack::writeTrackerProject(destination, imported.project, imported.source);
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.createPointMass(imported.project, "new empty mass"));
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), sourceFor(500000));
        imported = motrack::parseTrackerProject(sourceFor(499999));
        edits.clear();
        ASSERT_TRUE(edits.createPointMass(imported.project, "new empty mass"));
        EXPECT_NO_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source));
        const auto saved = readBytes(destination);
        imported = motrack::parseTrackerProject(saved);
        edits.clear();
        ASSERT_TRUE(edits.markPointMass(imported.project, 2, {.frame = 0, .x = 3, .y = 4}));
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), saved);
    }
}

TEST(TrackerWriter, RetainedInheritedMassArraysCountTowardTheExportAllocationLimit)
{
    std::string xml = "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>\n"
                      "<property name='tracks' type='collection' class='java.util.ArrayList'>\n"
                      "<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>"
                      "<property name='name' type='string'>mass A</property></object></property>\n";
    for (int index = 0; index < 2; ++index)
    {
        xml += "<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.CenterOfMass'>"
               "<property name='name' type='string'>center " +
               std::to_string(index) +
               "</property>"
               "<property name='framedata' type='array' class='[Lorg.opensourcephysics.cabrillo.tracker.PointMass$FrameData;'>"
               "<property name='[500000]' type='object'>null</property></property>"
               "<property name='masses' type='collection' class='java.util.ArrayList'>"
               "<property name='item' type='string'>mass A</property></property></object></property>\n";
    }
    xml += "</property>\n</object>\n";
    auto imported = motrack::parseTrackerProject(xml);
    ASSERT_EQ(imported.project.pointMasses.size(), 1);
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(destination, imported.project, imported.source);
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.renamePointMass(imported.project, 0, "renamed"));
    EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), xml);
}

TEST(TrackerWriter, CreatedTrackMetadataMustFitTheFinalSchemaBudget)
{
    const auto imported = motrack::parseTrackerProject(namedSource());
    auto project = imported.project;
    motrack::TrackEdits edits;
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(destination, imported.project, imported.source);
    for (int index = 0; index < 1000; ++index)
        ASSERT_TRUE(edits.createPointMass(project, "new " + std::to_string(index)));
    EXPECT_THROW(motrack::writeTrackerProject(destination, project, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), namedSource());
    project.pointMasses.resize(502);
    motrack::writeTrackerProject(destination, project, imported.source);
    auto reopened = motrack::parseTrackerProject(readBytes(destination));
    edits.clear();
    ASSERT_TRUE(edits.createPointMass(reopened.project, "another mass"));
    ASSERT_TRUE(edits.renamePointMass(reopened.project, 0, "renamed"));
    EXPECT_NO_THROW(motrack::writeTrackerProject(destination, reopened.project, reopened.source));
}

TEST(TrackerWriter, CreatedMassRejectsPresentButUnusableCalibrationAtomically)
{
    for (const auto& fields : {std::string("<property name='xscale' type='double'>1</property>"),
                               std::string("<property name='angle' type='double'>0</property>"
                                           "<property name='xscale' type='double'>NaN</property>")})
    {
        const auto source = namedSource(
            "<property name='coords' type='object'><object class='org.opensourcephysics.media.core.ImageCoordSystem'>"
            "<property name='fixedorigin' type='boolean'>true</property><property name='fixedangle' type='boolean'>true</property>"
            "<property name='fixedscale' type='boolean'>true</property>"
            "<property name='framedata' type='array' class='[Lorg.opensourcephysics.media.core.ImageCoordSystem$FrameData;'>"
            "<property name='[0]' type='object'><object class='org.opensourcephysics.media.core.ImageCoordSystem$FrameData'>"
            "<property name='xorigin' type='double'>0</property><property name='yorigin' type='double'>0</property>" +
            fields + "<property name='yscale' type='double'>1</property></object></property></property></object></property>");
        auto imported = motrack::parseTrackerProject(source);
        ASSERT_FALSE(imported.project.calibration);
        QTemporaryDir temporary;
        const auto destination = temporary.filePath("existing.trk");
        motrack::writeTrackerProject(destination, imported.project, imported.source);
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.createPointMass(imported.project, "new mass"));
        ASSERT_TRUE(edits.markPointMass(imported.project, 2, {.frame = 1, .x = 2, .y = 3}));
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), source);
        imported = motrack::parseTrackerProject(source);
        edits.clear();
        ASSERT_TRUE(edits.markPointMass(imported.project, 0, {.frame = 1, .x = 2, .y = 3}));
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), source);
    }
}

TEST(TrackerWriter, CreatedMassRejectsCoordinateCancellationWithoutChangingCalibrationOrDestination)
{
    const auto xml =
        namedSource("<property name='coords' type='object'><object class='org.opensourcephysics.media.core.ImageCoordSystem'>"
                    "<property name='fixedorigin' type='boolean'>true</property><property name='fixedangle' type='boolean'>true</property>"
                    "<property name='fixedscale' type='boolean'>true</property>"
                    "<property name='framedata' type='array' class='[Lorg.opensourcephysics.media.core.ImageCoordSystem$FrameData;'>"
                    "<property name='[0]' type='object'><object class='org.opensourcephysics.media.core.ImageCoordSystem$FrameData'>"
                    "<property name='xorigin' type='double'>16</property><property name='yorigin' type='double'>16</property>"
                    "<property name='angle' type='double'>0</property><property name='xscale' type='double'>1e-100</property>"
                    "<property name='yscale' type='double'>2e-100</property></object></property></property></object></property>");
    const auto imported = motrack::parseTrackerProject(xml);
    auto project = imported.project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createPointMass(project, "new mass"));
    const motrack::Mark mark{.frame = 1, .x = std::nextafter(16.0, 17.0), .y = 16};
    ASSERT_TRUE(edits.markPointMass(project, 2, mark));
    if (!project.calibration)
    {
        FAIL() << "Expected the original calibration.";
    }
    const auto world = motrack::worldPosition(*project.calibration, mark);
    if (!world)
    {
        FAIL() << "Expected finite native world coordinates.";
    }
    EXPECT_NEAR(world->x, 3.552713678800501e85, 1e71);
    // Java's diagonal AffineTransform multiplies first and then adds translation.
    const auto product = mark.x * (1 / 1e-100);
    const auto trackerX = product - 16 / 1e-100;
    EXPECT_GT(std::abs(trackerX - world->x), 0.1 * world->x);
    EXPECT_EQ(project.calibration, imported.project.calibration);
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(destination, imported.project, imported.source);
    EXPECT_THROW(motrack::writeTrackerProject(destination, project, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), xml);

    // Creating an empty mass first must not let a later mark-only edit bypass the same check.
    auto empty = imported;
    edits.clear();
    ASSERT_TRUE(edits.createPointMass(empty.project, "new mass"));
    empty = motrack::parseTrackerProject(motrack::renderTrackerProject(empty.project, empty.source));
    motrack::writeTrackerProject(destination, empty.project, empty.source);
    motrack::WorkspaceDocument document{.project = empty.project, .movieFrameTimes = {}, .trackerSource = empty.source};
    edits.clear();
    ASSERT_TRUE(edits.markPointMass(document.project, 2, mark));
    const auto native = temporary.filePath("mark-only.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = motrack::readWorkspaceDocument(native);
    if (!document.trackerSource)
    {
        FAIL() << "Native reopen lost the standard-reimported source.";
    }
    EXPECT_THROW(motrack::writeTrackerProject(destination, document.project, *document.trackerSource), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), empty.source.xml);
}

TEST(TrackerWriter, RenamesSelectedMassByIdentityAndPreservesNonReferenceWordsAfterNativeReopen)
{
    const auto imported = motrack::parseTrackerProject(namedSource());
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    const auto name = QString::fromUtf8("élève β & <B>").toStdString();
    ASSERT_TRUE(edits.renamePointMass(document.project, 1, "temporary"));
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 4));
    ASSERT_TRUE(edits.renamePointMass(document.project, 1, name));
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    ASSERT_TRUE(edits.removePointMass(document.project, 0));
    ASSERT_TRUE(edits.undo(document.project));
    std::ranges::reverse(document.project.pointMasses);
    QTemporaryDir temporary;
    const auto native = temporary.filePath("names.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = motrack::readWorkspaceDocument(native);
    if (!document.trackerSource)
    {
        FAIL() << "Native reopen lost the original source.";
    }
    const auto originalIndex = motrack::indexTrackerReferences(imported.source, imported.project);
    const auto bytes = motrack::renderTrackerProject(document.project, *document.trackerSource);
    const auto output = motrack::parseTrackerProject(bytes);
    EXPECT_EQ(output.project.pointMasses[1].name, name);
    EXPECT_EQ(output.project.pointMasses[0].mass, 4);
    EXPECT_EQ(output.project.description, imported.project.description);
    EXPECT_EQ(document.trackerSource->xml, imported.source.xml);
    const auto updated = motrack::indexTrackerReferences(output.source, output.project);
    ASSERT_EQ(updated.references.size(), originalIndex.references.size());
    for (std::size_t i = 0; i < updated.references.size(); ++i)
    {
        EXPECT_EQ(updated.references[i].path, originalIndex.references[i].path);
        EXPECT_EQ(updated.references[i].candidateSourceIndices, originalIndex.references[i].candidateSourceIndices);
        EXPECT_EQ(updated.references[i].resolution, originalIndex.references[i].resolution);
        EXPECT_EQ(updated.references[i].originalName,
                  originalIndex.references[i].originalName == "mass B" ? name : originalIndex.references[i].originalName);
    }
    // Column labels remain data even when identical to a renamed track.
    const std::string label = R"(name="[1]" type="string">mass B</property>)";
    const auto firstLabel = bytes.find(label);
    ASSERT_NE(firstLabel, std::string::npos);
    EXPECT_NE(bytes.find(label, firstLabel + label.size()), std::string::npos);
}

TEST(TrackerWriter, RenamesUnselectedMassAndSwapsNamesWithoutResolvingAgainstEditedLabels)
{
    auto imported = motrack::parseTrackerProject(namedSource());
    const auto original = motrack::indexTrackerReferences(imported.source, imported.project);
    imported.project.pointMasses[0].name = "new A";
    auto output = motrack::parseTrackerProject(motrack::renderTrackerProject(imported.project, imported.source));
    auto references = motrack::indexTrackerReferences(output.source, output.project);
    EXPECT_EQ(output.project.pointMasses[0].name, "new A");
    EXPECT_EQ(references.references[0].originalName, "mass B");
    imported.project.pointMasses[0].name = "mass B";
    imported.project.pointMasses[1].name = "mass A";
    output = motrack::parseTrackerProject(motrack::renderTrackerProject(imported.project, imported.source));
    references = motrack::indexTrackerReferences(output.source, output.project);
    EXPECT_EQ(output.project.pointMasses[0].name, "mass B");
    EXPECT_EQ(output.project.pointMasses[1].name, "mass A");
    EXPECT_EQ(references.references[0].originalName, "mass A");
    EXPECT_EQ(references.references[0].candidateSourceIndices, original.references[0].candidateSourceIndices);
}

TEST(TrackerWriter, RejectsReservedOriginalNamesWithoutReplacingDestinationOrChangingUneditedSources)
{
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    for (const std::string oldName : {"null", "bad,name", "bad;name"})
    {
        auto xml = namedSource();
        std::size_t position = 0;
        while ((position = xml.find("mass B", position)) != std::string::npos)
        {
            xml.replace(position, 6, oldName);
            position += oldName.size();
        }
        const auto imported = motrack::parseTrackerProject(xml);
        motrack::writeTrackerProject(destination, imported.project, imported.source);
        EXPECT_EQ(readBytes(destination), xml);
        // Even an unchanged reserved-name track makes another track's rename ambiguous.
        for (std::size_t selected : {0u, 1u})
        {
            auto project = imported.project;
            project.pointMasses[selected].name = "valid name";
            EXPECT_THROW(motrack::writeTrackerProject(destination, project, imported.source), motrack::ProjectError);
            EXPECT_EQ(readBytes(destination), xml);
        }
    }
}

TEST(TrackerWriter, UndoingANameEditRestoresByteExactSource)
{
    auto imported = motrack::parseTrackerProject(namedSource());
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.renamePointMass(imported.project, 1, "new name"));
    ASSERT_TRUE(edits.undo(imported.project));
    EXPECT_EQ(motrack::renderTrackerProject(imported.project, imported.source), imported.source.xml);
}

std::string namedVectorSource()
{
    auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk");
    const auto end = xml.rfind("</object>");
    if (end == std::string::npos)
        throw motrack::ProjectError("Missing vector fixture");
    xml.insert(end,
               "<property name='selectedtrack' type='string'>vector B</property>\n"
               "<property name='selected_track_views' type='string'>vector B,null;null,vector B;null,null;null,null</property>\n"
               "<property name='description' type='string'>vector A and vector B are words in this note.</property>\n");
    return xml;
}

TEST(TrackerWriter, RenamesBoundVectorWithSelectionsAfterInterleavedEditsAndNativeReopen)
{
    const auto imported = motrack::parseTrackerProject(namedVectorSource());
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    const auto name = QString::fromUtf8("élève β & <B>").toStdString();
    ASSERT_TRUE(edits.renameVector(document.project, 1, "temporary"));
    ASSERT_TRUE(edits.markVector(document.project, 0, {.frame = 3, .tailX = 100, .tailY = 200, .componentX = 3, .componentY = 4}));
    ASSERT_TRUE(edits.renameVector(document.project, 1, name));
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    ASSERT_TRUE(edits.removeVector(document.project, 0));
    ASSERT_TRUE(edits.undo(document.project));
    const auto first = document.project.vectors[0];
    const auto second = document.project.vectors[1];
    std::ranges::reverse(document.project.vectors);
    QTemporaryDir temporary;
    const auto native = temporary.filePath("vector-names.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = motrack::readWorkspaceDocument(native);
    if (!document.trackerSource)
    {
        FAIL() << "Native reopen lost the original vector source.";
    }
    const auto output = motrack::parseTrackerProject(motrack::renderTrackerProject(document.project, *document.trackerSource));
    EXPECT_EQ(output.project.vectors, (std::vector<motrack::VectorTrack>{first, second}));
    EXPECT_EQ(output.project.description, imported.project.description);
    const auto references = motrack::indexTrackerReferences(output.source, output.project);
    ASSERT_EQ(references.references.size(), 9u);
    for (const auto& reference : references.references)
        if (reference.resolution != motrack::TrackerReferenceResolution::NoTarget)
        {
            EXPECT_EQ(reference.originalName, name);
            EXPECT_EQ(reference.candidateSourceIndices, (std::vector<std::size_t>{2}));
        }
    EXPECT_EQ(document.trackerSource->xml, imported.source.xml);
}

TEST(TrackerWriter, VectorRenamesKeepSnappingUnknownDataAndNameCollisionGuards)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("existing.trk");
    const auto imported = motrack::parseTrackerProject(namedVectorSource());
    motrack::writeTrackerProject(path, imported.project, imported.source);
    for (const auto& name : {"vector A", "axes", "null", "bad,name"})
    {
        auto project = imported.project;
        project.vectors[1].name = name;
        EXPECT_THROW(motrack::writeTrackerProject(path, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(path), imported.source.xml);
    }
    auto snapping = imported.project;
    snapping.vectors[1].name = "renamed";
    snapping.vectors[1].samples.front().tailX = 321;
    snapping.vectors[1].samples.front().tailY = 240;
    EXPECT_THROW(motrack::writeTrackerProject(path, snapping, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(path), imported.source.xml);
    for (const auto& [from, to] :
         std::vector<std::pair<std::string, std::string>>{{"type=\"object\">null", "type=\"object\">missing"},
                                                          {"cabrillo.tracker.Vector$FrameData", "cabrillo.tracker.FutureFrameData"},
                                                          {"name=\"footprint\"", "name=\"attachments\""}})
    {
        auto xml = namedVectorSource();
        ASSERT_NE(xml.find(from), std::string::npos);
        xml.replace(xml.find(from), from.size(), to);
        auto unknown = motrack::parseTrackerProject(xml);
        unknown.project.vectors[1].name = "renamed";
        EXPECT_THROW(motrack::writeTrackerProject(path, unknown.project, unknown.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(path), imported.source.xml);
    }
}

TEST(TrackerWriter, RenamesPlotOwnerAndGuestThroughNativeHistoryWithoutChangingTargetsOrMeasurements)
{
    const auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/plot-guests.trk");
    const auto imported = motrack::parseTrackerProject(xml);
    const auto before = motrack::indexTrackerReferences(imported.source, imported.project);
    for (std::size_t selected : {0u, 1u})
    {
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        motrack::TrackEdits edits;
        const auto old = document.project.pointMasses[selected].name;
        const auto name = QString::fromUtf8("élève β & <B>").toStdString();
        ASSERT_TRUE(edits.renamePointMass(document.project, selected, "temporary"));
        ASSERT_TRUE(edits.renamePointMass(document.project, selected, name));
        ASSERT_TRUE(edits.undo(document.project));
        ASSERT_TRUE(edits.redo(document.project));
        ASSERT_TRUE(edits.removePointMass(document.project, 2));
        ASSERT_TRUE(edits.undo(document.project));
        std::ranges::reverse(document.project.pointMasses);
        QTemporaryDir temporary;
        const auto native = temporary.filePath("plot-name.motrack");
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Native reopen lost the original plot source.";
        }
        const auto output = motrack::parseTrackerProject(motrack::renderTrackerProject(document.project, *document.trackerSource));
        auto expected = imported.project;
        expected.pointMasses[selected].name = name;
        EXPECT_EQ(output.project, expected);
        const auto after = motrack::indexTrackerReferences(output.source, output.project);
        ASSERT_EQ(after.references.size(), before.references.size());
        for (std::size_t i = 0; i < before.references.size(); ++i)
        {
            EXPECT_EQ(after.references[i].path, before.references[i].path);
            EXPECT_EQ(after.references[i].candidateSourceIndices, before.references[i].candidateSourceIndices);
            EXPECT_EQ(after.references[i].originalName,
                      before.references[i].originalName == old ? name : before.references[i].originalName);
        }
        EXPECT_EQ(document.trackerSource->xml, xml);
    }
}

TEST(TrackerWriter, DeletedPlotOwnersAndGuestsStayDeletedThroughNativeReopenAndLaterEdits)
{
    const auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/plot-guests.trk");
    const auto imported = motrack::parseTrackerProject(xml);
    for (unsigned removed = 1; removed < 8; ++removed)
    {
        SCOPED_TRACE(removed);
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        motrack::TrackEdits edits;
        for (std::size_t i = 3; i-- > 0;)
            if ((removed & (1u << i)) != 0)
            {
                ASSERT_TRUE(edits.removePointMass(document.project, i));
                ASSERT_TRUE(edits.undo(document.project));
                ASSERT_TRUE(edits.redo(document.project));
            }
        if (!document.project.pointMasses.empty())
            ASSERT_TRUE(edits.renamePointMass(document.project, 0, "renamed survivor"));
        auto expected = document.project.pointMasses;
        std::ranges::reverse(document.project.pointMasses);
        QTemporaryDir temporary;
        const auto native = temporary.filePath("deleted-plot.motrack");
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Native reopen lost the original plot source.";
        }
        const auto bytes = motrack::renderTrackerProject(document.project, *document.trackerSource);
        auto output = motrack::parseTrackerProject(bytes);
        // The pinned OSP reader consumes array/collection properties line by line, unlike
        // Qt's general XML reader. A surviving child must not share its container's line.
        for (const std::string field : {"guests", "track_views"})
        {
            const auto name = "name=\"" + field + "\"";
            for (auto position = bytes.find(name); position != std::string::npos; position = bytes.find(name, position + name.size()))
            {
                const auto end = bytes.find('>', position);
                ASSERT_NE(end, std::string::npos);
                if (bytes[end - 1] != '/')
                {
                    ASSERT_LT(end + 1, bytes.size());
                    EXPECT_EQ(bytes[end + 1], '\n') << field;
                }
            }
        }
        ASSERT_EQ(output.project.pointMasses.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i)
        {
            expected[i].id = output.project.pointMasses[i].id;
            EXPECT_EQ(output.project.pointMasses[i], expected[i]);
        }
        const auto references = motrack::indexTrackerReferences(output.source, output.project);
        EXPECT_TRUE(references.malformedPaths.empty());
        std::vector<std::string> guests;
        unsigned owners = 0;
        for (const auto& reference : references.references)
        {
            EXPECT_EQ(reference.resolution, motrack::TrackerReferenceResolution::Resolved);
            if (reference.kind == motrack::TrackerReferenceKind::PlotOwner)
            {
                ++owners;
                EXPECT_EQ(reference.originalName, "renamed survivor");
            }
            if (reference.kind == motrack::TrackerReferenceKind::PlotGuest)
                guests.push_back(reference.originalName);
        }
        std::vector<std::string> expectedGuests;
        if ((removed & 1u) == 0)
        {
            if ((removed & 4u) == 0)
                expectedGuests.emplace_back("mass C");
            if ((removed & 2u) == 0)
            {
                expectedGuests.emplace_back("mass B");
                expectedGuests.emplace_back("mass B");
            }
        }
        EXPECT_EQ(owners, (removed & 1u) == 0 ? 1u : 0u);
        EXPECT_EQ(guests, expectedGuests);
        EXPECT_EQ(document.trackerSource->xml, xml);
        EXPECT_EQ(motrack::renderTrackerProject(document.project, *document.trackerSource), bytes);
        // Reusing a deleted label after standard reimport must not inherit its old plot links.
        ASSERT_TRUE(edits.createPointMass(output.project, (removed & 1u) != 0 ? "mass A" : (removed & 2u) != 0 ? "mass B" : "mass C"));
        const auto again = motrack::parseTrackerProject(motrack::renderTrackerProject(output.project, output.source));
        const auto againReferences = motrack::indexTrackerReferences(again.source, again.project);
        ASSERT_EQ(againReferences.references.size(), references.references.size());
        for (std::size_t i = 0; i < references.references.size(); ++i)
        {
            EXPECT_EQ(againReferences.references[i].kind, references.references[i].kind);
            EXPECT_EQ(againReferences.references[i].path, references.references[i].path);
            EXPECT_EQ(againReferences.references[i].originalName, references.references[i].originalName);
            EXPECT_EQ(againReferences.references[i].candidateSourceIndices, references.references[i].candidateSourceIndices);
        }
    }
}

TEST(TrackerWriter, PlotGuestDeletionPreservesNumericIndicesWhenSourceCellsAreOutOfOrder)
{
    auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/plot-guests.trk");
    const auto tracksEnd = xml.find("    </property>\n    <property name=\"selectedtrack\"");
    ASSERT_NE(tracksEnd, std::string::npos);
    xml.insert(tracksEnd,
               R"(<property name="item" type="object">)"
               "\n"
               R"(<object class="org.opensourcephysics.cabrillo.tracker.PointMass">)"
               "\n"
               R"(<property name="name" type="string">mass D</property>)"
               "\n</object>\n</property>\n");
    const std::string first = R"(<property name="[0]" type="string">mass C</property>)";
    const std::string second = R"(<property name="[1]" type="string">mass B</property>)";
    ASSERT_NE(xml.find(first), std::string::npos);
    ASSERT_NE(xml.find(second), std::string::npos);
    xml.replace(xml.find(first),
                first.size(),
                R"(<property name="[1]" type="string">mass C</property>)"
                "\n"
                R"(<property name="[0]" type="string">mass B</property>)");
    xml.replace(xml.find(second), second.size(), R"(<property name="[2]" type="string">mass D</property>)");
    const auto imported = motrack::parseTrackerProject(xml);
    auto project = imported.project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.removePointMass(project, 3));
    const auto output = motrack::parseTrackerProject(motrack::renderTrackerProject(project, imported.source));
    std::vector<std::string> guests;
    for (const auto& reference : motrack::indexTrackerReferences(output.source, output.project).references)
        if (reference.kind == motrack::TrackerReferenceKind::PlotGuest)
            guests.push_back(reference.originalName);
    EXPECT_EQ(guests, (std::vector<std::string>{"mass B", "mass C", "mass B"}));
}

TEST(TrackerWriter, PlotDeletionResolvesAllOriginalCollectionOrdinalsBeforeRemovingOwners)
{
    auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/plot-guests.trk");
    const auto collection = xml.find("name=\"track_views\"");
    ASSERT_NE(collection, std::string::npos);
    const auto first = xml.find(R"(                    <property name="item" type="object">)", collection);
    ASSERT_NE(first, std::string::npos);
    const auto end = xml.find("\n                </property>", first);
    ASSERT_NE(end, std::string::npos);
    std::string additional;
    for (const std::string name : {"mass B", "mass C"})
    {
        auto added = xml.substr(first, end - first);
        const std::string owner = R"(name="track" type="string">mass A)";
        const std::string guest = R"(name="[0]" type="string">mass C)";
        ASSERT_NE(added.find(owner), std::string::npos);
        ASSERT_NE(added.find(guest), std::string::npos);
        added.replace(added.find(owner), owner.size(), R"(name="track" type="string">)" + name);
        added.replace(added.find(guest), guest.size(), R"(name="[0]" type="string">mass A)");
        additional += "\n" + added;
    }
    xml.insert(end, additional);
    const auto imported = motrack::parseTrackerProject(xml);
    auto project = imported.project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.renamePointMass(project, 2, "surviving C"));
    ASSERT_TRUE(edits.removePointMass(project, 1));
    ASSERT_TRUE(edits.removePointMass(project, 0));
    const auto output = motrack::parseTrackerProject(motrack::renderTrackerProject(project, imported.source));
    const auto references = motrack::indexTrackerReferences(output.source, output.project);
    ASSERT_EQ(references.references.size(), 2);
    EXPECT_EQ(references.references[0].kind, motrack::TrackerReferenceKind::PlotSelection);
    EXPECT_EQ(references.references[1].kind, motrack::TrackerReferenceKind::PlotOwner);
    EXPECT_EQ(references.references[1].path, "/views/0/0/track_views/0/track");
    for (const auto& reference : references.references)
    {
        EXPECT_EQ(reference.originalName, "surviving C");
        EXPECT_EQ(reference.resolution, motrack::TrackerReferenceResolution::Resolved);
    }
}

TEST(TrackerWriter, RenameWalksPlotCollectionsByOrdinalAndLeavesVariableTextAlone)
{
    const std::string xml =
        "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>"
        "<property name='tracks' type='collection' class='java.util.ArrayList'>"
        "<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>"
        "<property name='name' type='string'>A</property></object></property>"
        "<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>"
        "<property name='name' type='string'>B</property></object></property></property>"
        "<property name='views' type='array' class='[[Lorg.opensourcephysics.cabrillo.tracker.TView;'>"
        "<property name='[0]' type='array' class='[Lorg.opensourcephysics.cabrillo.tracker.TView;'>"
        "<property name='[0]' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PlotTView'>"
        "<property name='selected_track' type='string'>A</property>"
        "<property name='track_views' type='collection' class='java.util.ArrayList'>"
        "<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PlotTrackView'>"
        "<property name='linked' type='boolean'>false</property>"
        "<property name='track' type='string'>A</property></object></property>"
        "<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PlotTrackView'>"
        "<property name='linked' type='boolean'>false</property>"
        "<property name='track' type='string'>B</property>"
        "<property name='plot0' type='object'><object class='org.opensourcephysics.cabrillo.tracker.TrackPlottingPanel'>"
        "<property name='x_var' type='string'>A</property><property name='y_var' type='string'>B</property>"
        "<property name='guests' type='array' class='[Ljava.lang.String;'>"
        "<property name='[0]' type='string'>A</property></property></object></property>"
        "</object></property></property></object></property></property></property></object>";
    auto imported = motrack::parseTrackerProject(xml);
    imported.project.pointMasses[0].name = "new A";
    imported.project.pointMasses[1].name = "new B";
    const auto bytes = motrack::renderTrackerProject(imported.project, imported.source);
    const auto output = motrack::parseTrackerProject(bytes);
    const auto after = motrack::indexTrackerReferences(output.source, output.project);
    ASSERT_EQ(after.references.size(), 4u);
    EXPECT_EQ(after.references[2].path, "/views/0/0/track_views/1/track");
    EXPECT_EQ(after.references[2].originalName, "new B");
    EXPECT_EQ(after.references[3].originalName, "new A");
    EXPECT_NE(bytes.find(R"(name="x_var" type="string">A</property>)"), std::string::npos);
    EXPECT_NE(bytes.find(R"(name="y_var" type="string">B</property>)"), std::string::npos);
}

TEST(TrackerWriter, RejectsMissingAmbiguousMalformedAndUncoveredPlotRenameRelationships)
{
    const auto original = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/plot-guests.trk");
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    const auto unchanged = motrack::parseTrackerProject(original);
    motrack::writeTrackerProject(destination, unchanged.project, unchanged.source);
    for (const auto& [from, to] :
         std::vector<std::pair<std::string, std::string>>{{">mass C</property>", ">mass B</property>"},
                                                          {R"(name="[0]" type="string">mass C)", R"(name="[0]" type="string">missing)"},
                                                          {R"(name="guests" type="array")", R"(name="future_guests" type="array")"},
                                                          {R"(name="[1]" type="string">mass B)", R"(name="[3]" type="string">mass B)"},
                                                          {"cabrillo.tracker.PlotTrackView", "cabrillo.tracker.FuturePlotView"},
                                                          {R"(name="track" type="string">mass A)", R"(name="track" type="object">mass A)"},
                                                          {R"(name="track_views" type="collection")", R"(name="track_views" type="array")"},
                                                          {R"(name="linked" type="boolean")", R"(name="scaled" type="boolean")"}})
    {
        auto xml = original;
        ASSERT_NE(xml.find(from), std::string::npos);
        xml.replace(xml.find(from), from.size(), to);
        auto imported = motrack::parseTrackerProject(xml);
        imported.project.pointMasses[1].name = "renamed";
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), original);
    }
}

TEST(TrackerWriter, RenamesCenterMembersByOriginalIdentityAfterNativeHistoryAndReopen)
{
    const auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/center-of-mass.trk");
    const auto imported = motrack::parseTrackerProject(xml);
    const auto before = motrack::indexTrackerReferences(imported.source, imported.project);
    for (std::size_t selected : {0u, 1u})
    {
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        motrack::TrackEdits edits;
        const auto old = document.project.pointMasses[selected].name;
        const auto name = QString::fromUtf8("élève β & <B>").toStdString();
        ASSERT_TRUE(edits.renamePointMass(document.project, selected, "temporary"));
        ASSERT_TRUE(edits.renamePointMass(document.project, selected, name));
        ASSERT_TRUE(edits.undo(document.project));
        ASSERT_TRUE(edits.redo(document.project));
        ASSERT_TRUE(edits.removePointMass(document.project, 1 - selected));
        ASSERT_TRUE(edits.undo(document.project));
        std::ranges::reverse(document.project.pointMasses);
        QTemporaryDir temporary;
        const auto native = temporary.filePath("center-name.motrack");
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Native reopen lost the original center source.";
        }
        const auto output = motrack::parseTrackerProject(motrack::renderTrackerProject(document.project, *document.trackerSource));
        auto expected = imported.project;
        expected.pointMasses[selected].name = name;
        EXPECT_EQ(output.project, expected);
        const auto after = motrack::indexTrackerReferences(output.source, output.project);
        ASSERT_EQ(after.references.size(), before.references.size());
        ASSERT_EQ(after.references.size(), 3u);
        for (std::size_t i = 0; i < before.references.size(); ++i)
        {
            EXPECT_EQ(after.references[i].path, before.references[i].path);
            EXPECT_EQ(after.references[i].candidateSourceIndices, before.references[i].candidateSourceIndices);
            EXPECT_EQ(after.references[i].resolution, before.references[i].resolution);
            EXPECT_EQ(after.references[i].originalName,
                      before.references[i].originalName == old ? name : before.references[i].originalName);
        }
        EXPECT_EQ(document.trackerSource->xml, xml);
    }
}

TEST(TrackerWriter, RejectsLossyOrUnclassifiedCenterMembersBeforeReplacingAFile)
{
    const auto original = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/center-of-mass.trk");
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    const auto unchanged = motrack::parseTrackerProject(original);
    motrack::writeTrackerProject(destination, unchanged.project, unchanged.source);
    const std::string first = R"(<property name="item" type="string">mass A</property>)";
    const std::string second = R"(<property name="item" type="string">mass B</property>)";
    for (const auto& [from, to] :
         std::vector<std::pair<std::string, std::string>>{{first, second},
                                                          {first, R"(<property name="item" type="string">missing</property>)"},
                                                          {first, R"(<property name="item" type="string">center</property>)"},
                                                          {first, R"(<property name="item" type="object">mass A</property>)"},
                                                          {R"(name="masses" type="collection")", R"(name="masses" type="array")"},
                                                          {"cabrillo.tracker.CenterOfMass", "cabrillo.tracker.FutureCenter"}})
    {
        auto xml = original;
        ASSERT_NE(xml.find(from), std::string::npos);
        xml.replace(xml.find(from), from.size(), to);
        auto imported = motrack::parseTrackerProject(xml);
        imported.project.pointMasses[0].name = "renamed";
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), original);
    }
    for (const bool removeCollection : {false, true})
    {
        auto xml = original;
        const auto begin = xml.find(R"(<property name="masses")");
        const auto end = xml.find("</property>", xml.find(second, begin) + second.size());
        ASSERT_NE(begin, std::string::npos);
        ASSERT_NE(end, std::string::npos);
        xml.replace(begin,
                    end + std::string_view("</property>").size() - begin,
                    removeCollection ? "" : R"(<property name="masses" type="collection" class="java.util.ArrayList"/>)");
        auto imported = motrack::parseTrackerProject(xml);
        imported.project.pointMasses[0].name = "renamed";
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), original);
    }
}

TEST(TrackerWriter, RenamesEitherTapeAttachmentTargetAfterNativeHistoryWithoutFillingEmptySlots)
{
    const auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/attachments.trk");
    const auto imported = motrack::parseTrackerProject(xml);
    const auto before = motrack::indexTrackerReferences(imported.source, imported.project);
    for (std::size_t selected : {0u, 1u})
    {
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        motrack::TrackEdits edits;
        const auto old = document.project.pointMasses[selected].name;
        const auto name = QString::fromUtf8("élève β & <B>").toStdString();
        ASSERT_TRUE(edits.renamePointMass(document.project, selected, "temporary"));
        ASSERT_TRUE(edits.renamePointMass(document.project, selected, name));
        ASSERT_TRUE(edits.undo(document.project));
        ASSERT_TRUE(edits.redo(document.project));
        ASSERT_TRUE(edits.removePointMass(document.project, 1 - selected));
        ASSERT_TRUE(edits.undo(document.project));
        std::ranges::reverse(document.project.pointMasses);
        QTemporaryDir temporary;
        const auto native = temporary.filePath("attachment-name.motrack");
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Native reopen lost the original attachment source.";
        }
        const auto bytes = motrack::renderTrackerProject(document.project, *document.trackerSource);
        const auto output = motrack::parseTrackerProject(bytes);
        auto expected = imported.project;
        expected.pointMasses[selected].name = name;
        EXPECT_EQ(output.project, expected);
        const auto after = motrack::indexTrackerReferences(output.source, output.project);
        ASSERT_EQ(after.references.size(), before.references.size());
        ASSERT_EQ(after.references.size(), 7u);
        for (std::size_t i = 0; i < before.references.size(); ++i)
        {
            EXPECT_EQ(after.references[i].path, before.references[i].path);
            EXPECT_EQ(after.references[i].candidateSourceIndices, before.references[i].candidateSourceIndices);
            EXPECT_EQ(after.references[i].resolution, before.references[i].resolution);
            EXPECT_EQ(after.references[i].originalName,
                      before.references[i].originalName == old ? name : before.references[i].originalName);
        }
        EXPECT_EQ(document.trackerSource->xml, xml);
        EXPECT_EQ(bytes.find(R"(name="[0]" type="object">null</property>)"), std::string::npos); // Keep the implicit gap implicit.
        EXPECT_NE(bytes.find(R"(name="[1]" type="object">null</property>)"), std::string::npos);
    }
}

TEST(TrackerWriter, RejectsUncoveredAttachmentModesTargetsAndSlotsBeforeReplacingAFile)
{
    const auto original = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/attachments.trk");
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    const auto unchanged = motrack::parseTrackerProject(original);
    motrack::writeTrackerProject(destination, unchanged.project, unchanged.source);
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
             {R"(name="[0]" type="string">mass A)", R"(name="[0]" type="string">missing)"},
             {R"(name="[0]" type="string">mass A)", R"(name="[0]" type="string">both ends)"},
             {R"(name="[1]" type="string">mass B)", R"(name="[2]" type="string">mass B)"},
             {R"(name="[1]" type="object">null)", R"(name="[1]" type="object">missing)"},
             {R"(name="readonly" type="boolean">true)", R"(name="readonly" type="boolean">false)"},
             {R"(name="fixedtape" type="boolean">true)", R"(name="fixedtape" type="boolean">false)"},
             {R"(name="fixedlength" type="boolean">true)", R"(name="fixedlength" type="boolean">false)"},
             {R"(name="stickmode" type="boolean">false)", R"(name="stickmode" type="boolean">true)"},
             {R"(name="worldlengths")", R"(name="future_lengths")"},
             {"cabrillo.tracker.TapeMeasure$FrameData", "cabrillo.tracker.FutureFrameData"}})
    {
        auto xml = original;
        ASSERT_NE(xml.find(from), std::string::npos);
        xml.replace(xml.find(from), from.size(), to);
        auto imported = motrack::parseTrackerProject(xml);
        imported.project.pointMasses[0].name = "renamed";
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), original);
    }
}

TEST(TrackerWriter, RejectsPlotDefinitionsThatJavaWouldCoalesceBeforeReplacingDestination)
{
    const auto original = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/plot-guests.trk");
    const auto unchanged = motrack::parseTrackerProject(original);
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    for (const int mutation : {0, 1, 2})
    {
        SCOPED_TRACE(mutation);
        auto xml = original;
        if (mutation == 0)
        {
            const std::string guest = R"(name="[0]" type="string">mass C</property>)";
            ASSERT_NE(xml.find(guest), std::string::npos);
            xml.replace(xml.find(guest), guest.size(), R"(name="[0]" type="string">mass B</property>)");
        }
        else if (mutation == 1)
        {
            const auto owner = xml.find(R"(<object class="org.opensourcephysics.cabrillo.tracker.PlotTrackView">)");
            ASSERT_NE(owner, std::string::npos);
            const auto begin = xml.rfind("<property", owner);
            ASSERT_NE(begin, std::string::npos);
            std::size_t cursor = owner + 7;
            int depth = 1;
            while (depth > 0)
            {
                const auto next = xml.find("<object", cursor);
                const auto end = xml.find("</object>", cursor);
                ASSERT_NE(end, std::string::npos);
                if (next != std::string::npos && next < end)
                {
                    ++depth;
                    cursor = next + 7;
                }
                else
                {
                    --depth;
                    cursor = end + 9;
                }
            }
            const auto end = xml.find("</property>", cursor);
            ASSERT_NE(end, std::string::npos);
            xml.insert(end + 11, xml.substr(begin, end + 11 - begin));
        }
        else
        {
            const std::string owner = R"(<property name="track" type="string">mass A</property>)";
            ASSERT_NE(xml.find(owner), std::string::npos);
            xml.erase(xml.find(owner), owner.size());
        }
        auto imported = motrack::parseTrackerProject(xml);
        EXPECT_EQ(motrack::renderTrackerProject(imported.project, imported.source), xml);
        imported.project.pointMasses[0].name = "renamed";
        motrack::writeTrackerProject(destination, unchanged.project, unchanged.source);
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_TRUE(readBytes(destination) == original) << "Unsupported plot rename replaced the existing destination.";
    }
}

TEST(TrackerWriter, RenamesModelBoosterAfterNativeHistoryWithoutChangingSystemOrFunctions)
{
    const auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/model-dependencies.trk");
    const auto imported = motrack::parseTrackerProject(xml);
    const auto before = motrack::indexTrackerReferences(imported.source, imported.project);
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    const auto name = QString::fromUtf8("élève β & <B>").toStdString();
    ASSERT_TRUE(edits.renamePointMass(document.project, 0, "temporary"));
    ASSERT_TRUE(edits.renamePointMass(document.project, 0, name));
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    ASSERT_TRUE(edits.removePointMass(document.project, 0));
    ASSERT_TRUE(edits.undo(document.project));
    QTemporaryDir temporary;
    const auto native = temporary.filePath("model-booster-name.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = motrack::readWorkspaceDocument(native);
    if (!document.trackerSource)
    {
        FAIL() << "Native reopen lost the original model source.";
    }
    const auto output = motrack::parseTrackerProject(motrack::renderTrackerProject(document.project, *document.trackerSource));
    auto expected = imported.project;
    expected.pointMasses[0].name = name;
    EXPECT_EQ(output.project, expected);
    const auto after = motrack::indexTrackerReferences(output.source, output.project);
    ASSERT_EQ(after.references.size(), before.references.size());
    ASSERT_EQ(after.references.size(), 4u);
    for (std::size_t i = 0; i < before.references.size(); ++i)
    {
        EXPECT_EQ(after.references[i].path, before.references[i].path);
        EXPECT_EQ(after.references[i].candidateSourceIndices, before.references[i].candidateSourceIndices);
        EXPECT_EQ(after.references[i].resolution, before.references[i].resolution);
        EXPECT_EQ(after.references[i].originalName,
                  before.references[i].originalName == "mass A" ? name : before.references[i].originalName);
    }
    EXPECT_EQ(document.trackerSource->xml, xml);
}

TEST(TrackerWriter, RejectsUnknownOrLossyModelDependenciesAndFunctionDefinitionsBeforeReplacement)
{
    const auto original = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/model-dependencies.trk");
    const auto unchanged = motrack::parseTrackerProject(original);
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(destination, unchanged.project, unchanged.source);
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
             {R"(name="booster" type="string">mass A)", R"(name="booster" type="string">particle A)"},
             {R"(name="booster" type="string">mass A)", R"(name="booster" type="string">missing)"},
             {R"(name="[1]" type="string">particle B)", R"(name="[1]" type="string">particle A)"},
             {R"(name="[1]" type="string">particle B)", R"(name="[2]" type="string">particle B)"},
             {R"(name="in_system" type="boolean">true)", R"(name="in_system" type="boolean">false)"},
             {R"(name="user_parameters")", R"(name="support_functions")"},
             {R"(name="name" type="string">y</property>)", R"(name="name" type="string">x</property>)"},
             {R"(name="name" type="string">fy</property>)", R"(name="name" type="string">fx</property>)"},
             {R"(<property name="function" type="string">1.0</property>)", ""},
             {R"(<property name="expression" type="string">0</property>)", ""},
             {"class=\"org.opensourcephysics.cabrillo.tracker.DynamicParticle\"",
              "class=\"org.opensourcephysics.cabrillo.tracker.DynamicParticlePolar\""}})
    {
        SCOPED_TRACE(from);
        auto xml = original;
        ASSERT_NE(xml.find(from), std::string::npos);
        xml.replace(xml.find(from), from.size(), to);
        auto imported = motrack::parseTrackerProject(xml);
        imported.project.pointMasses[0].name = "renamed";
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_TRUE(readBytes(destination) == original);
    }
}

TEST(TrackerWriter, RejectsTruncatedInitialStateAndMismatchedModelFunctionArraysAtomically)
{
    const auto original = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/model-dependencies.trk");
    const auto unchanged = motrack::parseTrackerProject(original);
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    for (const int mutation : {0, 1, 2, 3, 4, 5, 6, 7})
    {
        SCOPED_TRACE(mutation);
        auto xml = original;
        if (mutation == 0)
        {
            const auto begin = xml.find(R"(<property name="[4]" type="object">)", xml.find(R"(name="initial_values")"));
            ASSERT_NE(begin, std::string::npos);
            const auto end = xml.find("</property>", xml.find("</object>", begin)) + std::string("</property>").size();
            xml.erase(begin, end - begin);
        }
        else if (mutation == 1)
        {
            const std::string name = R"(name="name" type="string">vx</property>)";
            xml.replace(xml.find(name), name.size(), R"(name="name" type="string">vz</property>)");
        }
        else if (mutation < 6)
        {
            const std::string values = R"(<property name="array" type="string">{1.0}</property>)";
            const auto begin = xml.find(values, xml.find(R"(name="parameter_values")"));
            ASSERT_NE(begin, std::string::npos);
            const std::string replacement = mutation == 2   ? R"(<property name="array" type="string">{}</property>)"
                                            : mutation == 3 ? R"(<property name="array" type="string">{1.0,2.0}</property>)"
                                            : mutation == 4 ? R"(<property name="array" type="string">{NaN}</property>)"
                                                            : R"(<property name="[1]" type="double">1.0</property>)";
            xml.replace(begin, values.size(), replacement);
        }
        else
        {
            const auto field = xml.find(mutation == 6 ? R"(name="parameter_descriptions")" : R"(name="variables")");
            const auto begin = xml.find('>', field) + 1;
            if (mutation == 6)
            {
                const auto childEnd = xml.find("</property>", begin) + std::string("</property>").size();
                xml.erase(begin, childEnd - begin);
            }
            else
            {
                const auto last = xml.find(R"(<property name="[4]")", begin);
                ASSERT_NE(last, std::string::npos);
                const auto childEnd = xml.find("</property>", last) + std::string("</property>").size();
                xml.erase(begin, childEnd - begin);
            }
        }
        auto imported = motrack::parseTrackerProject(xml);
        imported.project.pointMasses[0].name = "renamed";
        motrack::writeTrackerProject(destination, unchanged.project, unchanged.source);
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_TRUE(readBytes(destination) == original);
    }
}

TEST(TrackerWriter, RejectsForceFunctionsWithWrongStateWidthBeforeReplacingDestination)
{
    const auto original = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/model-dependencies.trk");
    const auto unchanged = motrack::parseTrackerProject(original);
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    for (int function = 0; function < 6; ++function)
    {
        SCOPED_TRACE(function);
        std::size_t variables = 0;
        for (int index = 0; index <= function; ++index)
        {
            variables = original.find(R"(<property name="variables")", variables);
            ASSERT_NE(variables, std::string::npos);
            ++variables;
        }
        const auto last = original.find(R"(<property name="[4]")", variables);
        ASSERT_NE(last, std::string::npos);
        const auto end = original.find("</property>", last) + std::string("</property>").size();
        for (int count : {1, 4, 6})
        {
            SCOPED_TRACE(count);
            auto xml = original;
            if (count < 5)
            {
                const auto firstRemoved = xml.find("<property name=\"[" + std::to_string(count) + "]\"", variables);
                ASSERT_NE(firstRemoved, std::string::npos);
                xml.erase(firstRemoved, end - firstRemoved);
            }
            else
                xml.insert(end, "\n<property name='[5]' type='string'>extra</property>\n");
            auto imported = motrack::parseTrackerProject(xml);
            imported.project.pointMasses[0].name = "renamed";
            motrack::writeTrackerProject(destination, unchanged.project, unchanged.source);
            EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
            EXPECT_TRUE(readBytes(destination) == original);
        }
    }
}

TEST(TrackerWriter, ValidatesExpandedModelParameterValuesWithoutRewritingThem)
{
    const auto original = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/model-dependencies.trk");
    const std::string packed = R"(<property name="array" type="string">{1.0}</property>)";
    const auto begin = original.find(packed, original.find(R"(name="parameter_values")"));
    ASSERT_NE(begin, std::string::npos);
    for (const auto& value : {"1.0", "NaN", "1e999", "oops"})
    {
        auto xml = original;
        const auto expanded = std::string(R"(<property name="[0]" type="double">)") + value + "</property>";
        xml.replace(begin, packed.size(), expanded);
        auto imported = motrack::parseTrackerProject(xml);
        imported.project.pointMasses[0].name = "renamed";
        if (std::string_view(value) == "1.0")
        {
            const auto output = motrack::renderTrackerProject(imported.project, imported.source);
            EXPECT_NE(output.find(expanded), std::string::npos);
        }
        else
        {
            EXPECT_THROW((void)motrack::renderTrackerProject(imported.project, imported.source), motrack::ProjectError);
        }
    }
}

TEST(TrackerWriter, RejectsUnauditedLinkedPlotSettingsBeforeReplacingDestination)
{
    const auto original = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/plot-guests.trk");
    const auto unchanged = motrack::parseTrackerProject(original);
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    for (const std::string linked : {"true", "", "missing", "invalid"})
    {
        SCOPED_TRACE(linked);
        auto xml = original;
        const std::string field = R"(<property name="linked" type="boolean">false</property>)";
        ASSERT_NE(xml.find(field), std::string::npos);
        xml.replace(xml.find(field),
                    field.size(),
                    linked == "missing" ? "" : R"(<property name="linked" type="boolean">)" + linked + "</property>");
        // Linked loading synchronizes x axes: the later panel's x can replace the earlier t.
        const std::string variable = R"(<property name="x_var" type="string">t</property>)";
        ASSERT_NE(xml.rfind(variable), xml.find(variable));
        xml.replace(xml.rfind(variable), variable.size(), R"(<property name="x_var" type="string">x</property>)");
        auto imported = motrack::parseTrackerProject(xml);
        EXPECT_EQ(motrack::renderTrackerProject(imported.project, imported.source), xml);
        imported.project.pointMasses[1].name = "renamed";
        motrack::writeTrackerProject(destination, unchanged.project, unchanged.source);
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_TRUE(readBytes(destination) == original);
    }
}

TEST(TrackerWriter, RejectsNonViewablePlotAndTableSelectionsBeforeReplacingDestination)
{
    const auto original = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/plot-guests.trk");
    const auto unchanged = motrack::parseTrackerProject(original);
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    for (const int selection : {0, 1, 2, 3, 4})
    {
        SCOPED_TRACE(selection);
        auto xml = original;
        if (selection < 3)
        {
            const std::string field = selection == 0 ? "selected_track" : selection == 1 ? "track" : "[0]";
            const auto old = "name=\"" + field + R"(" type="string">)" + (selection == 2 ? "mass C" : "mass A") + "</property>";
            ASSERT_NE(xml.find(old), std::string::npos);
            xml.replace(xml.find(old), old.size(), "name=\"" + field + R"(" type="string">axes</property>)");
        }
        else
        {
            const auto end = xml.rfind("</object>");
            xml.insert(end,
                       "<property name='selected_track_views' type='string'>" + std::string(selection == 3 ? "axes,null" : "null,axes") +
                           ";null,null;null,null;null,null</property>\n");
        }
        auto imported = motrack::parseTrackerProject(xml);
        imported.project.pointMasses[1].name = "renamed";
        motrack::writeTrackerProject(destination, unchanged.project, unchanged.source);
        EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
        EXPECT_TRUE(readBytes(destination) == original);
    }
}

TEST(TrackerWriter, BoundsRenameSchemaAndRejectsUnclassifiedFieldsAndEncodings)
{
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
             {"name='description' type='string'", "name='description' type='string' future='mass B'"},
             {"class='org.opensourcephysics.cabrillo.tracker.TableTView'", "class='org.opensourcephysics.cabrillo.tracker.PlotTView'"},
             {"name='[1]' type='array' class='[Lorg", "name='[10000]' type='array' class='[Lorg"},
             {"name='[1]' type='array' class='[Lorg", "name='[01]' type='array' class='[Lorg"},
             {"name='column_formats'", "name='dataset_indices'"},
             {"name='description' type='string'", "name='attachments' type='string'"},
             {"name='selectedtrack' type='string'>mass B", "name='selectedtrack' type='string'><!-- cannot patch -->mass B"},
             {"class='java.util.ArrayList'", "class='java.util.LinkedList'"},
             {"name='[0]' type='string'>mass B", "name='[0]' type='object'>mass B"},
             {"type='array' class='[[D'", "type='array' class='[[I'"}})
    {
        auto xml = namedSource();
        ASSERT_NE(xml.find(from), std::string::npos);
        xml.replace(xml.find(from), from.size(), to);
        auto imported = motrack::parseTrackerProject(xml);
        imported.project.pointMasses[1].name = "new name";
        EXPECT_THROW((void)motrack::renderTrackerProject(imported.project, imported.source), motrack::ProjectError);
    }
    std::string many = "<property name='selected_view_types' type='array' class='[I'>";
    for (int i = 0; i < 10000; ++i)
        many += "<property name='[" + std::to_string(i) + "]' type='int'>0</property>";
    many += "</property>";
    auto bounded = motrack::parseTrackerProject(namedSource(many));
    bounded.project.pointMasses[1].name = "new name";
    EXPECT_THROW((void)motrack::renderTrackerProject(bounded.project, bounded.source), motrack::ProjectError);
}

TEST(TrackerWriter, RejectsUnsafeRenameNamesAndUnknownRelationshipsWithoutReplacingAFile)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("existing.trk");
    const auto imported = motrack::parseTrackerProject(namedSource());
    motrack::writeTrackerProject(path, imported.project, imported.source);
    for (const auto& name : {std::string{},
                             std::string("null"),
                             std::string("mass A"),
                             std::string("protractor"),
                             std::string("comma,name"),
                             std::string("semi;colon"),
                             std::string(" spaced "),
                             std::string("line\nbreak"),
                             std::string("end]]>"),
                             std::string(4097, 'x'),
                             std::string("\xff")})
    {
        auto project = imported.project;
        project.pointMasses[1].name = name;
        EXPECT_THROW(motrack::writeTrackerProject(path, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(path), imported.source.xml);
    }
    for (const auto& field : {"<property name='future_link' type='string'>mass B</property>",
                              "<property name='referenceframe' type='string'>mass B</property>",
                              "<property name='description' type='string'>duplicate</property>",
                              "<property name='description' type='object'><object class='Future'/></property>"})
    {
        auto unknown = motrack::parseTrackerProject(namedSource(field));
        unknown.project.pointMasses[1].name = "renamed";
        EXPECT_THROW(motrack::writeTrackerProject(path, unknown.project, unknown.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(path), imported.source.xml);
    }
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
             {">mass A<", ">mass B<"},
             {">mass B</property>\n<property name='selected_track_views'", ">missing</property>\n<property name='selected_track_views'"},
             {"cabrillo.tracker.Protractor", "cabrillo.tracker.FutureTrack"},
             {"type='string'>mass B,null", "type='double'>mass B,null"},
             {"name='[1]' type='array' class='[Lorg", "name='[9]' type='array' class='[Lorg"}})
    {
        auto xml = namedSource();
        ASSERT_NE(xml.find(from), std::string::npos);
        xml.replace(xml.find(from), from.size(), to);
        auto malformed = motrack::parseTrackerProject(xml);
        malformed.project.pointMasses[1].name = "renamed";
        EXPECT_THROW(motrack::writeTrackerProject(path, malformed.project, malformed.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(path), imported.source.xml);
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

TEST(TrackerWriter, CreatesEmptyAndMarkedVectorsAfterNativeReopenWithoutDuplicatingOrRebindingTracks)
{
    const auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk");
    const auto imported = motrack::parseTrackerProject(xml);
    for (const bool empty : {false, true})
    {
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.createVector(document.project, "discarded"));
        ASSERT_TRUE(edits.undo(document.project));
        const auto name = QString::fromUtf8("vecteur β & <C>").toStdString();
        ASSERT_TRUE(edits.createVector(document.project, name));
        if (!empty)
        {
            ASSERT_TRUE(edits.markVector(document.project, 2, {.frame = 1, .tailX = 100, .tailY = 120, .componentX = 4, .componentY = -8}));
            ASSERT_TRUE(edits.markVector(document.project, 2, {.frame = 6, .tailX = 200, .tailY = 180, .componentX = 0, .componentY = 0}));
            ASSERT_TRUE(
                edits.markVector(document.project, 2, {.frame = 8, .tailX = 160, .tailY = 100, .componentX = -6, .componentY = 12}));
        }
        ASSERT_TRUE(edits.undo(document.project));
        ASSERT_TRUE(edits.redo(document.project));
        ASSERT_TRUE(edits.removeVector(document.project, 2));
        ASSERT_TRUE(edits.undo(document.project));
        const auto created = document.project.vectors.back();
        std::ranges::reverse(document.project.vectors);
        QTemporaryDir temporary;
        const auto native = temporary.filePath("creation.motrack");
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Native reopen lost the vector source.";
        }
        const auto bytes = motrack::renderTrackerProject(document.project, *document.trackerSource);
        EXPECT_EQ(bytes, motrack::renderTrackerProject(document.project, *document.trackerSource));
        EXPECT_EQ(document.trackerSource->xml, xml);
        const auto output = motrack::parseTrackerProject(bytes);
        ASSERT_EQ(output.project.vectors.size(), 3);
        EXPECT_EQ(output.project.vectors[0], imported.project.vectors[0]);
        EXPECT_EQ(output.project.vectors[1], imported.project.vectors[1]);
        EXPECT_EQ(output.project.vectors.back().name, name);
        EXPECT_EQ(output.project.vectors.back().samples, created.samples);
        EXPECT_EQ(motrack::renderTrackerProject(output.project, output.source), bytes);
        EXPECT_EQ(bytes.find("keyFrames"), std::string::npos);
    }
}

TEST(TrackerWriter, CreatesAnotherVectorAfterStandardReimportWithHighOriginalFrames)
{
    auto imported =
        motrack::parseTrackerProject(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk"));
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createVector(imported.project, "high frame"));
    ASSERT_TRUE(edits.markVector(imported.project, 2, {.frame = 10000, .tailX = 100, .tailY = 120, .componentX = 4, .componentY = -8}));
    imported = motrack::parseTrackerProject(motrack::renderTrackerProject(imported.project, imported.source));
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    QTemporaryDir temporary;
    const auto native = temporary.filePath("high-frame.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = motrack::readWorkspaceDocument(native);
    if (!document.trackerSource)
    {
        FAIL() << "Native reopen lost the reimported vector source.";
    }
    edits.clear();
    ASSERT_TRUE(edits.createVector(document.project, "next vector"));
    const auto result = motrack::parseTrackerProject(motrack::renderTrackerProject(document.project, *document.trackerSource));
    ASSERT_EQ(result.project.vectors.size(), 4);
    EXPECT_EQ(result.project.vectors[2].samples, imported.project.vectors[2].samples);
    EXPECT_EQ(result.project.vectors[3].name, "next vector");
    EXPECT_EQ(motrack::renderTrackerProject(result.project, result.source), result.source.xml);
}

TEST(TrackerWriter, CreatedVectorMetadataMustFitTheFinalSchemaBudget)
{
    const auto imported =
        motrack::parseTrackerProject(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk"));
    auto project = imported.project;
    motrack::TrackEdits edits;
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(destination, imported.project, imported.source);
    for (int index = 0; index < 1000; ++index)
        ASSERT_TRUE(edits.createVector(project, "new " + std::to_string(index)));
    EXPECT_THROW(motrack::writeTrackerProject(destination, project, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), imported.source.xml);
    project.vectors.resize(502);
    motrack::writeTrackerProject(destination, project, imported.source);
    auto reopened = motrack::parseTrackerProject(readBytes(destination));
    edits.clear();
    ASSERT_TRUE(edits.createVector(reopened.project, "another vector"));
    ASSERT_TRUE(edits.renameVector(reopened.project, 0, "renamed"));
    EXPECT_NO_THROW(motrack::writeTrackerProject(destination, reopened.project, reopened.source));
}

TEST(TrackerWriter, CreatedVectorsRetainSnappingPrecisionAndAllocationGuardsAtomically)
{
    const auto imported =
        motrack::parseTrackerProject(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk"));
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(destination, imported.project, imported.source);
    for (const auto& sample : std::vector<motrack::VectorSample>{
             {.frame = 1, .tailX = 321, .tailY = 240, .componentX = 4, .componentY = 2},
             {.frame = 1, .tailX = 13, .tailY = 17, .componentX = 4, .componentY = 2},
             {.frame = 1, .tailX = 1e16, .tailY = 100, .componentX = 1, .componentY = 2},
             {.frame = 1, .tailX = 100, .tailY = 100, .componentX = std::numeric_limits<double>::infinity(), .componentY = 2},
             {.frame = 500000, .tailX = 100, .tailY = 100, .componentX = 4, .componentY = 2},
             {.frame = 500001, .tailX = 100, .tailY = 100, .componentX = 4, .componentY = 2}})
    {
        auto project = imported.project;
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.createVector(project, "new vector"));
        project.vectors.back().samples = {sample};
        EXPECT_THROW(motrack::writeTrackerProject(destination, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), imported.source.xml);
    }
    auto project = imported.project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createVector(project, "new vector"));
    ASSERT_TRUE(edits.markVector(project, 2, {.frame = 1, .tailX = 100, .tailY = 120, .componentX = 4, .componentY = -8}));
    ASSERT_TRUE(edits.markVector(project, 2, {.frame = 6, .tailX = 104, .tailY = 112, .componentX = 2, .componentY = 3}));
    EXPECT_THROW(motrack::writeTrackerProject(destination, project, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), imported.source.xml);
    ASSERT_TRUE(edits.removeVector(project, 0));
    EXPECT_THROW(motrack::writeTrackerProject(destination, project, imported.source), motrack::ProjectError);
}

TEST(TrackerWriter, CreatedVectorNamesAndUnknownDependenciesRemainAudited)
{
    auto mixed = motrack::parseTrackerProject(namedSource());
    motrack::TrackEdits mixedEdits;
    ASSERT_TRUE(mixedEdits.createVector(mixed.project, "new vector"));
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(mixed.project, mixed.source)), motrack::ProjectError);
    const auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk");
    for (const auto& name : {"vector A", "axes", "new,vector", "null"})
    {
        auto imported = motrack::parseTrackerProject(xml);
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.createVector(imported.project, "temporary"));
        imported.project.vectors.back().name = name;
        EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(imported.project, imported.source)), motrack::ProjectError);
    }
    auto dangling = xml;
    dangling.insert(dangling.rfind("</object>"), "<property name='selectedtrack' type='string'>new vector</property>\n");
    auto imported = motrack::parseTrackerProject(dangling);
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createVector(imported.project, "new vector"));
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(imported.project, imported.source)), motrack::ProjectError);
    auto sum = motrack::parseTrackerProject(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/vector-sum.trk"));
    edits.clear();
    ASSERT_TRUE(edits.createVector(sum.project, "new vector"));
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(sum.project, sum.source)), motrack::ProjectError);
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

TEST(TrackerWriter, RejectsVectorSumEditsEvenAfterAllMemberSamplesAreRemoved)
{
    const auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/vector-sum.trk");
    auto imported = motrack::parseTrackerProject(xml);
    EXPECT_EQ(motrack::renderTrackerProject(imported.project, imported.source), xml); // Exact original preservation remains possible.
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(output, imported.project, imported.source);
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    for (std::size_t index = 0; index < document.project.vectors.size(); ++index)
    {
        while (!document.project.vectors[index].samples.empty())
        {
            ASSERT_TRUE(edits.removeVectorSample(document.project, index, document.project.vectors[index].samples.back().frame));
        }
    }
    const auto native = temporary.filePath("empty-members.motrack");
    motrack::writeWorkspaceDocument(native, document);
    const auto reopened = motrack::readWorkspaceDocument(native);
    if (!reopened.trackerSource)
    {
        FAIL() << "Expected preserved vector-sum source";
    }
    EXPECT_EQ(reopened.trackerSource->xml, xml);
    EXPECT_THROW(motrack::writeTrackerProject(output, reopened.project, *reopened.trackerSource), motrack::ProjectError);
    EXPECT_EQ(readBytes(output), xml);
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

TEST(TrackerWriter, ExportsClipRangeAfterNativeReopenWithUndoRedoAndOutOfRangeSamples)
{
    const auto imported =
        motrack::parseTrackerProject(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk"));
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setClipRange(document.project, 2, 8, 2));
    const auto expected = document.project;
    const auto exported = motrack::renderTrackerProject(expected, imported.source);
    ASSERT_TRUE(edits.undo(document.project));
    EXPECT_EQ(motrack::renderTrackerProject(document.project, imported.source), imported.source.xml);
    ASSERT_TRUE(edits.redo(document.project));
    EXPECT_EQ(motrack::renderTrackerProject(document.project, imported.source), exported);
    QTemporaryDir temporary;
    const auto native = temporary.filePath("clip.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = motrack::readWorkspaceDocument(native);
    if (!document.trackerSource)
    {
        FAIL() << "Expected preserved Tracker source.";
    }
    const auto output = temporary.filePath("clip.trk");
    motrack::writeTrackerProject(output, document.project, *document.trackerSource);
    const auto reopened = motrack::parseLegacyProject(readBytes(output));
    EXPECT_EQ(reopened, expected);
    EXPECT_EQ(reopened.vectors, imported.project.vectors); // Frames 0 and 5 are outside selected clip steps.
    ASSERT_TRUE(edits.setClipRange(document.project, 500000, 500000, 1));
    EXPECT_EQ(motrack::parseLegacyProject(motrack::renderTrackerProject(document.project, imported.source)).clip.firstFrame, 500000);
}

TEST(TrackerWriter, RejectsInvalidClipRangesTimingAndUnverifiedLegacyFrameMappings)
{
    const auto original = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk");
    const auto imported = motrack::parseTrackerProject(original);
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(output, imported.project, imported.source);
    const std::vector<std::function<void(motrack::Clip&)>> changes{[](auto& clip) { clip.firstFrame = -1; },
                                                                   [](auto& clip) { clip.stepSize = 0; },
                                                                   [](auto& clip) { clip.stepCount = 0; },
                                                                   [](auto& clip) { clip.stepCount = std::numeric_limits<int>::max(); },
                                                                   [](auto& clip)
                                                                   {
                                                                       clip.firstFrame = 500001;
                                                                       clip.stepCount = 1;
                                                                   },
                                                                   [](auto& clip)
                                                                   {
                                                                       clip.firstFrame = 2;
                                                                       clip.stepCount = 1;
                                                                       clip.stepSize = std::numeric_limits<int>::max();
                                                                   },
                                                                   [](auto& clip)
                                                                   {
                                                                       clip.firstFrame = 2;
                                                                       clip.sourceFrameCount = 3;
                                                                   },
                                                                   [](auto& clip)
                                                                   {
                                                                       clip.firstFrame = 2;
                                                                       clip.startTimeMs = 1;
                                                                   },
                                                                   [](auto& clip)
                                                                   {
                                                                       clip.firstFrame = 2;
                                                                       clip.frameDurationMs = 50;
                                                                   }};
    for (const auto& change : changes)
    {
        auto project = imported.project;
        change(project.clip);
        EXPECT_THROW(motrack::writeTrackerProject(output, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), original);
    }
    for (const auto* property : {"video", "frameshift", "video_framecount"})
    {
        auto xml = original;
        xml.insert(xml.find("<property name=\"startframe\""), "<property name='" + std::string(property) + "' type='int'>0</property>\n");
        auto legacy = motrack::parseTrackerProject(xml);
        legacy.project.clip.firstFrame = 2;
        EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(legacy.project, legacy.source)), motrack::ProjectError);
    }
    auto wrongType = original;
    const std::string oldType = R"(name="startframe" type="int")";
    wrongType.replace(wrongType.find(oldType), oldType.size(), R"(name="startframe" type="double")");
    auto legacy = motrack::parseTrackerProject(wrongType);
    legacy.project.clip.firstFrame = 2;
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(legacy.project, legacy.source)), motrack::ProjectError);
}

std::string timingSource()
{
    auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk");
    xml.insert(xml.find("<property name=\"coords\""), R"xml(
<property name="clipcontrol" type="object">
<object class="org.opensourcephysics.media.core.StepperClipControl">
<property name="rate" type="double">1.25</property>
<property name="delta_t" type="double">100.0</property>
<property name="frame" type="int">0</property>
<property name="looping" type="boolean">false</property>
<!-- keep controller comment -->
</object>
</property>
)xml");
    return xml;
}

TEST(TrackerWriter, ExportsTimingWithNativeReopenHistoryAndUntouchedPlaybackState)
{
    const auto imported = motrack::parseTrackerProject(timingSource());
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setClipRange(document.project, 2, 8, 2));
    ASSERT_TRUE(edits.setClipTiming(document.project, 250, -500));
    const auto expected = document.project;
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.undo(document.project));
    EXPECT_EQ(motrack::renderTrackerProject(document.project, imported.source), imported.source.xml);
    ASSERT_TRUE(edits.redo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    QTemporaryDir temporary;
    const auto native = temporary.filePath("timing.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = motrack::readWorkspaceDocument(native);
    if (!document.trackerSource)
    {
        FAIL() << "Expected preserved timing source";
    }
    const auto output = temporary.filePath("timing.trk");
    motrack::writeTrackerProject(output, document.project, *document.trackerSource);
    const auto bytes = readBytes(output);
    EXPECT_EQ(motrack::parseLegacyProject(bytes), expected);
    EXPECT_NE(bytes.find(">1.25</property>"), std::string::npos);
    EXPECT_NE(bytes.find("<!-- keep controller comment -->"), std::string::npos);
    EXPECT_EQ(document.project.vectors, imported.project.vectors);
    EXPECT_EQ(document.project.calibration, imported.project.calibration);
}

TEST(TrackerWriter, RefusesUnauditedTimingStateAtomically)
{
    const auto original = timingSource();
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    const auto imported = motrack::parseTrackerProject(original);
    motrack::writeTrackerProject(output, imported.project, imported.source);
    const std::vector<std::pair<std::string, std::string>> changes{
        {"StepperClipControl", "VideoClipControl"},
        {R"(name="startframe" type="int")", R"(name="startframe" type="string")"},
        {R"(name="rate" type="double">1.25)", R"(name="rate" type="double">0)"},
        {R"(name="rate" type="double">1.25)", R"(name="rate" type="string">1.25)"},
        {R"(name="frame" type="int">0)", R"(name="frame" type="int">-1)"},
        {R"(name="looping" type="boolean">false)", R"(name="looping" type="boolean">maybe)"},
        {R"(name="delta_t")", R"(name="future_timing")"},
        {R"(name="rate")", R"(name="delta_t")"},
        {"<!-- keep controller comment -->", R"(<property name="time_source" type="string">data</property>)"},
        {"<!-- keep controller comment -->", R"(<property name="delta_t" type="double">100</property>)"},
        {R"(<property name="startframe")", R"(<property name="frameshift" type="int">0</property><property name="startframe")"},
        {"org.opensourcephysics.cabrillo.tracker.CoordAxes", "org.opensourcephysics.cabrillo.tracker.DynamicParticle"}};
    for (const auto& [before, after] : changes)
    {
        auto xml = original;
        const auto index = xml.find(before);
        ASSERT_NE(index, std::string::npos);
        xml.replace(index, before.size(), after);
        auto project = motrack::parseTrackerProject(xml);
        project.project.clip.frameDurationMs = 250;
        project.project.clip.startTimeMs = -500;
        EXPECT_THROW(motrack::writeTrackerProject(output, project.project, project.source), motrack::ProjectError) << before;
        EXPECT_EQ(readBytes(output), original);
    }
    for (const auto duration : {0.0, -1.0, std::numeric_limits<double>::infinity(), 1e308})
    {
        auto project = imported.project;
        project.clip.frameDurationMs = duration;
        EXPECT_THROW(motrack::writeTrackerProject(output, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), original);
    }
}

TEST(TrackerWriter, RefusesDistortedOriginsAndRangeOnlyTimingOverflowAtomically)
{
    const auto original = timingSource();
    auto imported = motrack::parseTrackerProject(original);
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(destination, imported.project, imported.source);
    for (const auto origin : {-1e16, 1e16})
    {
        auto project = imported.project;
        project.clip.frameDurationMs = 3;
        project.clip.startTimeMs = origin;
        EXPECT_THROW(motrack::writeTrackerProject(destination, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), original);
    }
    auto xml = original;
    for (const auto& [before, after] : std::vector<std::pair<std::string, std::string>>{
             {R"(name="delta_t" type="double">100.0)", R"(name="delta_t" type="double">1e308)"},
             {R"(name="stepcount" type="int">10)", R"(name="stepcount" type="int">1)"}})
    {
        const auto index = xml.find(before);
        ASSERT_NE(index, std::string::npos);
        xml.replace(index, before.size(), after);
    }
    imported = motrack::parseTrackerProject(xml);
    imported.project.clip.stepCount = 3;
    EXPECT_THROW(motrack::writeTrackerProject(destination, imported.project, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), original);
}

TEST(TrackerWriter, TimingOnlyExportRefusesOverflowingLegacyTemporaryFrameCount)
{
    auto xml = timingSource();
    for (const auto& [before, after] :
         std::vector<std::pair<std::string, std::string>>{{R"(name="startframe" type="int">0)", R"(name="startframe" type="int">2)"},
                                                          {R"(name="stepsize" type="int">1)", R"(name="stepsize" type="int">2147483647)"},
                                                          {R"(name="stepcount" type="int">10)", R"(name="stepcount" type="int">1)"}})
    {
        const auto position = xml.find(before);
        ASSERT_NE(position, std::string::npos);
        xml.replace(position, before.size(), after);
    }
    const auto imported = motrack::parseTrackerProject(xml);
    auto project = imported.project;
    project.clip.frameDurationMs = 250;
    project.clip.startTimeMs = -500;
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(output, imported.project, imported.source);
    EXPECT_THROW(motrack::writeTrackerProject(output, project, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(output), xml);
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

TEST(TrackerWriter, ExportsExistingFixedCalibrationThroughNativeReopenAndUndoRedo)
{
    auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk");
    xml.insert(xml.find("<property name=\"xorigin\""), "<property name='future' type='string'>  keep calibration  </property>\n");
    const auto imported = motrack::parseTrackerProject(xml);
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.calibrateDistance(document.project,
                                        {.frame = 0,
                                         .imageDeltaX = 8,
                                         .imageDeltaY = 0,
                                         .knownLength = 2,
                                         .defaultOriginX = 320,
                                         .defaultOriginY = 240,
                                         .unit = document.project.lengthUnit}));
    ASSERT_TRUE(
        edits.setCoordinateAxes(document.project, {.frame = 0, .originX = 100, .originY = 120, .positiveX = 100, .positiveY = 112}));
    const auto expected = document.project;
    const auto exported = motrack::renderTrackerProject(expected, imported.source);
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.undo(document.project));
    EXPECT_EQ(motrack::renderTrackerProject(document.project, imported.source), imported.source.xml);
    ASSERT_TRUE(edits.redo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    EXPECT_EQ(motrack::renderTrackerProject(document.project, imported.source), exported);
    QTemporaryDir temporary;
    const auto native = temporary.filePath("calibration.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = motrack::readWorkspaceDocument(native);
    if (!document.trackerSource)
    {
        FAIL() << "Expected preserved Tracker source.";
    }
    const auto output = temporary.filePath("calibration.trk");
    motrack::writeTrackerProject(output, document.project, *document.trackerSource);
    const auto saved = readBytes(output);
    EXPECT_EQ(motrack::parseLegacyProject(saved), expected);
    EXPECT_NE(saved.find("  keep calibration  "), std::string::npos);
    EXPECT_EQ(document.project.vectors, imported.project.vectors);
}

TEST(TrackerWriter, RejectsUnsafeCalibrationModesTransformsAndSnapping)
{
    const auto original = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk");
    const auto imported = motrack::parseTrackerProject(original);
    if (!imported.project.calibration)
    {
        FAIL() << "Expected source calibration.";
    }
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(output, imported.project, imported.source);
    const std::vector<std::function<void(motrack::Calibration&)>> changes{
        [](auto& value) { value.fixedOrigin = false; },
        [](auto& value) { value.fixedAngle = false; },
        [](auto& value) { value.fixedScale = false; },
        [](auto& value) { value.keyframes.clear(); },
        [](auto& value) { value.keyframes.front().frame = 1; },
        [](auto& value) { value.keyframes.front().scaleX = 0; },
        [](auto& value) { value.keyframes.front().scaleY = 0; },
        [](auto& value) { value.keyframes.front().originX = std::numeric_limits<double>::infinity(); },
        [](auto& value) { value.keyframes.front().angleDegrees = 360; },
        [](auto& value)
        {
            value.keyframes.front().originX = 9;
            value.keyframes.front().originY = 20;
        },
        [](auto& value) { value.keyframes.front().scaleX = value.keyframes.front().scaleY = 1e-200; },
        [](auto& value) { value.keyframes.front().scaleX = value.keyframes.front().scaleY = 1e200; },
        [](auto& value)
        {
            value.keyframes.front().angleDegrees = 45;
            value.keyframes.front().scaleX = value.keyframes.front().scaleY = 2e-162;
        },
        [](auto& value)
        {
            value.keyframes.front().angleDegrees = 45;
            value.keyframes.front().scaleX = 4;
            value.keyframes.front().scaleY = 8;
            value.keyframes.front().originX = 1e308;
            value.keyframes.front().originY = 0;
        },
        [](auto& value)
        {
            value.keyframes.front().scaleX = 1e-100;
            value.keyframes.front().originX = 1e308;
        }};
    for (const auto& change : changes)
    {
        auto project = imported.project;
        auto calibration = *imported.project.calibration;
        change(calibration);
        project.calibration = calibration;
        EXPECT_THROW(motrack::writeTrackerProject(output, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), original);
    }
    // Java's diagonal special case divides before translating and safely accepts this value.
    auto diagonal = imported.project;
    auto calibration = *imported.project.calibration;
    calibration.keyframes.front().originX = 1e308;
    calibration.keyframes.front().originY = 0;
    calibration.keyframes.front().scaleX = 4;
    calibration.keyframes.front().scaleY = 8;
    calibration.keyframes.front().angleDegrees = 0;
    diagonal.calibration = calibration;
    EXPECT_NO_THROW(static_cast<void>(motrack::renderTrackerProject(diagonal, imported.source)));
    auto locked = original;
    const std::string lockProperty = R"(name="locked" type="boolean">false)";
    locked.replace(locked.find(lockProperty), lockProperty.size(), R"(name="locked" type="boolean">true)");
    auto document = motrack::parseTrackerProject(locked);
    document.project.calibration = *imported.project.calibration;
    document.project.calibration->keyframes.front().originX = 100;
    EXPECT_THROW(static_cast<void>(motrack::renderTrackerProject(document.project, document.source)), motrack::ProjectError);
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

TEST(TrackerWriter, RejectsCalibrationCoordinateCancellationAtOriginAndRetainedMarks)
{
    const auto fixture = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk");
    const auto start = fixture.find("<property name=\"coords\"");
    const auto end = fixture.find("<property name=\"tracks\"");
    ASSERT_NE(start, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    ASSERT_LT(start, end);
    auto xml = markedXml();
    xml.insert(xml.find("<property name='tracks'>"), fixture.substr(start, end - start));
    const auto imported = motrack::parseTrackerProject(xml);
    if (!imported.project.calibration)
    {
        FAIL() << "Expected fixed source calibration.";
    }
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    const auto native = temporary.filePath("cancellation.motrack");
    motrack::writeTrackerProject(output, imported.project, imported.source);
    for (const bool rotated : {true, false})
    {
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        auto calibration = *imported.project.calibration;
        calibration.keyframes.front() = {.frame = 0,
                                         .originX = rotated ? 13.5 : 16,
                                         .originY = rotated ? 17.25 : 16,
                                         .angleDegrees = rotated ? 45.0 : 0.0,
                                         .scaleX = 1e-100,
                                         .scaleY = 2e-100};
        document.project.calibration = calibration;
        document.project.pointMasses.front().marks.front() = {
            .frame = 0, .x = rotated ? 13.5 : std::nextafter(16.0, 17.0), .y = rotated ? 17.25 : 16};
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Expected preserved source after native reopen.";
        }
        EXPECT_THROW(motrack::writeTrackerProject(output, document.project, *document.trackerSource), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), xml);
        calibration.keyframes.front().scaleX = 4;
        calibration.keyframes.front().scaleY = 8;
        document.project.calibration = calibration;
        EXPECT_NO_THROW(static_cast<void>(motrack::renderTrackerProject(document.project, *document.trackerSource)));
    }
}

TEST(TrackerWriter, RejectsCalibratedVectorComponentAndMagnitudeOverflow)
{
    auto imported =
        motrack::parseTrackerProject(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk"));
    imported.project.vectors.front().samples = {{.frame = 0, .tailX = -10, .tailY = 20, .componentX = 20, .componentY = -4}};
    imported.project.vectors.back().samples.clear();
    imported = motrack::parseTrackerProject(motrack::renderTrackerProject(imported.project, imported.source));
    if (!imported.project.calibration)
    {
        FAIL() << "Expected fixed source calibration.";
    }
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(output, imported.project, imported.source);
    for (const auto scale : {1e-307, 1e-200})
    {
        auto project = imported.project;
        auto calibration = *imported.project.calibration;
        calibration.keyframes.front() = {.frame = 0, .originX = 0, .originY = 0, .angleDegrees = 0, .scaleX = scale, .scaleY = 1 / scale};
        project.calibration = calibration;
        EXPECT_THROW(motrack::writeTrackerProject(output, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), imported.source.xml);
        calibration.keyframes.front().scaleX = 4;
        calibration.keyframes.front().scaleY = 8;
        project.calibration = calibration;
        EXPECT_NO_THROW(static_cast<void>(motrack::renderTrackerProject(project, imported.source)));
    }
}

TEST(TrackerWriter, ExportsLengthUnitsThroughCalibrationHistoryAndNativeReopen)
{
    const auto imported =
        motrack::parseTrackerProject(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk"));
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.calibrateDistance(document.project,
                                        {.frame = 0,
                                         .imageDeltaX = 4,
                                         .imageDeltaY = 0,
                                         .knownLength = 2,
                                         .defaultOriginX = 320,
                                         .defaultOriginY = 240,
                                         .unit = "\xC2\xB5m"}));
    const auto expected = document.project;
    EXPECT_EQ(expected.calibration, imported.project.calibration);
    ASSERT_TRUE(edits.undo(document.project));
    EXPECT_EQ(document.project, imported.project);
    ASSERT_TRUE(edits.redo(document.project));
    EXPECT_EQ(document.project, expected);
    QTemporaryDir temporary;
    const auto native = temporary.filePath("units.motrack");
    const auto output = temporary.filePath("units.trk");
    motrack::writeWorkspaceDocument(native, document);
    document = motrack::readWorkspaceDocument(native);
    if (!document.trackerSource)
    {
        FAIL() << "Expected source after native reopen.";
    }
    motrack::writeTrackerProject(output, document.project, *document.trackerSource);
    EXPECT_EQ(motrack::parseLegacyProject(readBytes(output)), expected);
    auto saved = motrack::parseTrackerProject(readBytes(output));
    saved.project.lengthUnit = "m & <lab>";
    EXPECT_EQ(motrack::parseLegacyProject(motrack::renderTrackerProject(saved.project, saved.source)), saved.project);
    saved.project.lengthUnit = "\xF0\x9D\x91\x8Em"; // Supplementary letter, not a decimal digit.
    EXPECT_EQ(motrack::parseLegacyProject(motrack::renderTrackerProject(saved.project, saved.source)), saved.project);
}

TEST(TrackerWriter, ExportsMassLabelsWithoutRescalingAfterNativeReopen)
{
    QTemporaryDir temporary;
    const auto native = temporary.filePath("mass-units.motrack");
    const auto output = temporary.filePath("mass-units.trk");
    for (const bool existing : {false, true})
    {
        auto xml = std::string(sourceXml());
        if (existing)
        {
            xml.insert(xml.find("<property name='tracks'>"), "<property name='mass_unit' type='string'>kg</property>");
        }
        const auto imported = motrack::parseTrackerProject(xml);
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setMassUnit(document.project, "\xC2\xB5g"));
        const auto expected = document.project;
        EXPECT_EQ(expected.pointMasses, imported.project.pointMasses);
        EXPECT_EQ(expected.calibration, imported.project.calibration);
        ASSERT_TRUE(edits.undo(document.project));
        EXPECT_EQ(document.project, imported.project);
        ASSERT_TRUE(edits.redo(document.project));
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Expected unit source after native reopen.";
        }
        motrack::writeTrackerProject(output, document.project, *document.trackerSource);
        EXPECT_EQ(motrack::parseLegacyProject(readBytes(output)), expected);
    }
}

TEST(TrackerWriter, RejectsInvalidMassLabelsAndUnpreservableSourceNodes)
{
    const auto imported = motrack::parseTrackerProject(std::string(sourceXml()));
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(output, imported.project, imported.source);
    for (const auto& unit :
         std::vector<std::string>{"", " g", "g ", "g2", "g\n", "]]>g", "g\xC2\x81x", "g\xF0\x9D\x9F\x9A", "\xFF", std::string(257, 'g')})
    {
        auto project = imported.project;
        project.massUnit = unit;
        EXPECT_THROW(motrack::writeTrackerProject(output, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), sourceXml());
    }
    auto xml = std::string(sourceXml());
    xml.insert(xml.find("<property name='tracks'>"), "<property name='mass_unit' type='string'>kg<!-- keep unit --></property>");
    auto commented = motrack::parseTrackerProject(xml);
    motrack::writeTrackerProject(output, commented.project, commented.source);
    commented.project.massUnit = "g";
    EXPECT_THROW(motrack::writeTrackerProject(output, commented.project, commented.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(output), xml);
}

TEST(TrackerWriter, ExportsTimeLabelsWithoutChangingTimingAfterNativeReopen)
{
    QTemporaryDir temporary;
    const auto native = temporary.filePath("time-units.motrack");
    const auto output = temporary.filePath("time-units.trk");
    for (const bool existing : {false, true})
    {
        auto xml = std::string(sourceXml());
        if (existing)
        {
            xml.insert(xml.find("<property name='tracks'>"), "<property name='time_unit' type='string'>s</property>");
        }
        const auto imported = motrack::parseTrackerProject(xml);
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setTimeUnit(document.project, "\xCF\x84"));
        const auto expected = document.project;
        EXPECT_EQ(expected.pointMasses, imported.project.pointMasses);
        EXPECT_EQ(expected.calibration, imported.project.calibration);
        EXPECT_EQ(expected.clip, imported.project.clip);
        ASSERT_TRUE(edits.undo(document.project));
        EXPECT_EQ(document.project, imported.project);
        ASSERT_TRUE(edits.redo(document.project));
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Expected unit source after native reopen.";
        }
        motrack::writeTrackerProject(output, document.project, *document.trackerSource);
        EXPECT_EQ(motrack::parseLegacyProject(readBytes(output)), expected);
    }
}

TEST(TrackerWriter, RejectsInvalidTimeLabelsAndUnpreservableSourceNodes)
{
    const auto imported = motrack::parseTrackerProject(std::string(sourceXml()));
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(output, imported.project, imported.source);
    for (const auto& unit :
         std::vector<std::string>{"", " g", "g ", "g2", "g\n", "]]>g", "g\xC2\x81x", "g\xF0\x9D\x9F\x9A", "\xFF", std::string(257, 'g')})
    {
        auto project = imported.project;
        project.timeUnit = unit;
        EXPECT_THROW(motrack::writeTrackerProject(output, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), sourceXml());
    }
    auto xml = std::string(sourceXml());
    xml.insert(xml.find("<property name='tracks'>"), "<property name='time_unit' type='string'>s<!-- keep unit --></property>");
    auto commented = motrack::parseTrackerProject(xml);
    motrack::writeTrackerProject(output, commented.project, commented.source);
    commented.project.timeUnit = "tick";
    EXPECT_THROW(motrack::writeTrackerProject(output, commented.project, commented.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(output), xml);
}

TEST(TrackerWriter, RejectsInvalidUnitLabelsWithoutReplacingTheDestination)
{
    auto xml = std::string(sourceXml());
    xml.insert(xml.find("<property name='tracks'>"), "<property name='length_unit' type='string'>m</property>");
    const auto imported = motrack::parseTrackerProject(xml);
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(output, imported.project, imported.source);
    for (const auto& unit :
         std::vector<std::string>{"", " m", "m ", "m2", "m\n", "]]>m", "m\xC2\x81x", "m\xF0\x9D\x9F\x9A", "\xFF", std::string(257, 'm')})
    {
        auto project = imported.project;
        project.lengthUnit = unit;
        EXPECT_THROW(motrack::writeTrackerProject(output, project, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), xml);
    }
    auto project = imported.project;
    project.lengthUnit = "cm";
    const auto result = motrack::renderTrackerProject(project, imported.source);
    EXPECT_EQ(motrack::parseLegacyProject(result).lengthUnit, "cm");
    EXPECT_NE(result.find("<![CDATA[cm]]></property>"), std::string::npos);
}

TEST(TrackerWriter, RejectsUnitEditsWithUnpreservableSourceNodes)
{
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    for (const auto* content : {"m<!-- keep unit -->", "<!-- keep unit -->m", "m<?unit keep?>", "<![CDATA[m]]><!-- keep unit -->"})
    {
        auto xml = std::string(sourceXml());
        xml.insert(xml.find("<property name='tracks'>"),
                   std::string("<property name='length_unit' type='string'>") + content + "</property>");
        const auto imported = motrack::parseTrackerProject(xml);
        motrack::writeTrackerProject(output, imported.project, imported.source);
        EXPECT_EQ(readBytes(output), xml);
        auto edited = imported.project;
        edited.lengthUnit = "cm";
        EXPECT_THROW(motrack::writeTrackerProject(output, edited, imported.source), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), xml);
    }
}

TEST(TrackerWriter, ExportsAddedAndEditedCalibrationKeysThroughHistoryAndNativeReopen)
{
    auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/keyed-calibration.trk");
    xml.insert(xml.find(R"(<property name="xorigin" type="double">100.0)"),
               "<property name=\"unknown_key_field\" type=\"string\">keep this</property>\n");
    const auto imported = motrack::parseTrackerProject(xml);
    motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.calibrateDistance(
        document.project,
        {.frame = 4, .imageDeltaX = 8, .imageDeltaY = 0, .knownLength = 1, .defaultOriginX = 0, .defaultOriginY = 0, .unit = "m"}));
    ASSERT_TRUE(
        edits.setCoordinateAxes(document.project, {.frame = 4, .originX = 120, .originY = 140, .positiveX = 128, .positiveY = 140}));
    ASSERT_TRUE(edits.setCoordinateAxes(document.project, {.frame = 3, .originX = 80, .originY = 90, .positiveX = 80, .positiveY = 98}));
    const auto expected = document.project;
    for (int index = 0; index < 3; ++index)
    {
        ASSERT_TRUE(edits.undo(document.project));
    }
    EXPECT_EQ(document.project, imported.project);
    for (int index = 0; index < 3; ++index)
    {
        ASSERT_TRUE(edits.redo(document.project));
    }
    EXPECT_EQ(document.project, expected);
    EXPECT_EQ(document.project.pointMasses, imported.project.pointMasses);
    QTemporaryDir temporary;
    const auto native = temporary.filePath("keyed.motrack");
    const auto output = temporary.filePath("keyed.trk");
    motrack::writeWorkspaceDocument(native, document);
    document = motrack::readWorkspaceDocument(native);
    if (!document.trackerSource || !document.project.calibration)
    {
        FAIL() << "Expected source and calibration after native reopen.";
    }
    motrack::writeTrackerProject(output, document.project, *document.trackerSource);
    const auto saved = readBytes(output);
    EXPECT_EQ(motrack::parseLegacyProject(saved), expected);
    EXPECT_NE(saved.find("keep this"), std::string::npos);
    const std::array<motrack::WorldPoint, 5> world{
        {{.x = -155, .y = 55}, {.x = -17.5, .y = -35}, {.x = -13.75, .y = 7.5}, {.x = -13.75, .y = 7.5}, {.x = -13.75, .y = -50}}};
    for (std::size_t index = 0; index < world.size(); ++index)
    {
        const auto actual = motrack::worldPosition(*document.project.calibration, document.project.pointMasses.front().marks[index]);
        if (!actual)
        {
            FAIL() << "Expected a world coordinate after keyed export.";
        }
        EXPECT_NEAR(actual->x, world[index].x, 1e-10);
        EXPECT_NEAR(actual->y, world[index].y, 1e-10);
    }
}

TEST(TrackerWriter, ExportsAllPartlyFixedCalibrationsThroughHistoryAndNativeReopen)
{
    const std::array<std::array<double, 10>, 6> worlds{{{-35, 17.5, -17.5, -35, -8.75, 4.375, -8.75, 4.375, -8.75, -35},
                                                        {-55, -155, -17.5, -35, -7.5, -13.75, -7.5, -13.75, -13.75, -50},
                                                        {-17.5, -35, -17.5, -35, -4.375, -8.75, -4.375, -8.75, -8.75, -35},
                                                        {-38.75, 6.875, -2.1875, -8.75, -13.75, 3.75, -13.75, 3.75, -3.4375, -12.5},
                                                        {-8.75, 2.1875, -2.1875, -8.75, -8.75, 2.1875, -8.75, 2.1875, -2.1875, -8.75},
                                                        {-6.875, -38.75, -2.1875, -8.75, -3.75, -13.75, -3.75, -13.75, -3.4375, -12.5}}};
    for (int flags = 1; flags < 7; ++flags)
    {
        SCOPED_TRACE(flags);
        const auto xml =
            readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/mixed-calibration-" + QString::number(flags) + ".trk");
        const auto imported = motrack::parseTrackerProject(xml);
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.calibrateDistance(
            document.project,
            {.frame = 4, .imageDeltaX = 8, .imageDeltaY = 0, .knownLength = 1, .defaultOriginX = 0, .defaultOriginY = 0, .unit = "m"}));
        ASSERT_TRUE(
            edits.setCoordinateAxes(document.project, {.frame = 4, .originX = 120, .originY = 140, .positiveX = 128, .positiveY = 140}));
        ASSERT_TRUE(
            edits.setCoordinateAxes(document.project, {.frame = 3, .originX = 80, .originY = 90, .positiveX = 80, .positiveY = 98}));
        const auto expected = document.project;
        for (int i = 0; i < 3; ++i)
            ASSERT_TRUE(edits.undo(document.project));
        EXPECT_EQ(document.project, imported.project);
        for (int i = 0; i < 3; ++i)
            ASSERT_TRUE(edits.redo(document.project));
        QTemporaryDir temporary;
        const auto native = temporary.filePath("mixed.motrack");
        const auto standard = temporary.filePath("mixed.trk");
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource || !document.project.calibration)
        {
            FAIL() << "Native reopen lost partly fixed calibration or original source.";
        }
        motrack::writeTrackerProject(standard, document.project, *document.trackerSource);
        EXPECT_EQ(motrack::parseLegacyProject(readBytes(standard)), expected);
        EXPECT_EQ(document.project.pointMasses, imported.project.pointMasses);
        const auto& calibration = *document.project.calibration;
        EXPECT_EQ(calibration.fixedOrigin, (flags & 1) != 0);
        EXPECT_EQ(calibration.fixedAngle, (flags & 2) != 0);
        EXPECT_EQ(calibration.fixedScale, (flags & 4) != 0);
        EXPECT_EQ(calibration.keyframes.size(), flags == 3 ? 3u : 4u);
        for (std::size_t i = 0; i < document.project.pointMasses.front().marks.size(); ++i)
        {
            const auto actual = motrack::worldPosition(calibration, document.project.pointMasses.front().marks[i]);
            if (!actual)
            {
                FAIL() << "Missing partly fixed world coordinate.";
            }
            EXPECT_NEAR(actual->x, worlds[static_cast<std::size_t>(flags - 1)][2 * i], 1e-10);
            EXPECT_NEAR(actual->y, worlds[static_cast<std::size_t>(flags - 1)][2 * i + 1], 1e-10);
        }
    }
}

TEST(TrackerWriter, RejectsMixedCalibrationFlagChangesAndExcessiveGlobalUpdatesAtomically)
{
    for (int flags = 1; flags < 7; ++flags)
    {
        const auto xml =
            readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/mixed-calibration-" + QString::number(flags) + ".trk");
        const auto imported = motrack::parseTrackerProject(xml);
        QTemporaryDir temporary;
        const auto output = temporary.filePath("existing.trk");
        motrack::writeTrackerProject(output, imported.project, imported.source);
        for (int component : {0, 1, 2})
        {
            auto changed = imported.project;
            if (!changed.calibration)
            {
                FAIL() << "Missing fixture calibration.";
            }
            auto& calibration = *changed.calibration;
            if (component == 0)
                calibration.fixedOrigin = !calibration.fixedOrigin;
            else if (component == 1)
                calibration.fixedAngle = !calibration.fixedAngle;
            else
                calibration.fixedScale = !calibration.fixedScale;
            EXPECT_THROW(motrack::writeTrackerProject(output, changed, imported.source), motrack::ProjectError);
            EXPECT_EQ(readBytes(output), xml);
        }
    }
    auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/mixed-calibration-1.trk");
    for (const auto& [before, after] :
         std::vector<std::pair<std::string, std::string>>{{R"(name="[4]" type="object")", R"(name="[100000]" type="object")"},
                                                          {R"(name="[9]" type="object")", R"(name="[200000]" type="object")"},
                                                          {R"(name="stepcount" type="int">10)", R"(name="stepcount" type="int">400001)"}})
    {
        ASSERT_NE(xml.find(before), std::string::npos);
        xml.replace(xml.find(before), before.size(), after);
    }
    auto imported = motrack::parseTrackerProject(xml);
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    motrack::writeTrackerProject(output, imported.project, imported.source);
    if (!imported.project.calibration)
    {
        FAIL() << "Missing source calibration.";
    }
    imported.project.calibration->keyframes.back().originX += 1;
    // Relative key-to-end ranges total 900003, but fixed-origin setters touch 1200003 frames.
    EXPECT_THROW(motrack::writeTrackerProject(output, imported.project, imported.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(output), xml);
}

TEST(TrackerWriter, RejectsUnsupportedOrUnstableCalibrationKeysWithoutReplacingFiles)
{
    const auto xml = readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/keyed-calibration.trk");
    const auto imported = motrack::parseTrackerProject(xml);
    ASSERT_TRUE(imported.project.calibration.has_value());
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    const auto native = temporary.filePath("invalid.motrack");
    motrack::writeTrackerProject(output, imported.project, imported.source);
    const std::vector<std::function<void(motrack::Calibration&)>> changes{
        [](auto& value) { value.fixedOrigin = true; },
        [](auto& value) { value.fixedAngle = true; },
        [](auto& value) { value.fixedScale = true; },
        [](auto& value) { value.keyframes.erase(value.keyframes.begin() + 1); },
        [](auto& value) { value.keyframes.front().frame = 1; },
        [](auto& value) { value.keyframes.back().frame = 500001; },
        [](auto& value)
        {
            auto key = value.keyframes.back();
            key.frame = 500000;
            value.keyframes.push_back(key); // Individually bounded keys can still cause excessive dense-frame updates.
        },
        [](auto& value) { value.keyframes[1].scaleX = 0; },
        [](auto& value) { value.keyframes[1].angleDegrees = 181; },
        [](auto& value)
        { value.keyframes[1] = {.frame = 4, .originX = 13.5, .originY = 17.25, .angleDegrees = 45, .scaleX = 1e-100, .scaleY = 2e-100}; },
        [](auto& value)
        {
            value.keyframes[1] = {.frame = 4, .originX = 16, .originY = 16, .angleDegrees = 0, .scaleX = 1e-100, .scaleY = 2e-100};
        }};
    for (const auto& change : changes)
    {
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        if (!document.project.calibration)
        {
            FAIL() << "Expected source calibration for the rejection case.";
        }
        change(*document.project.calibration);
        // A mark after the changed key must be evaluated using that key, not frame zero.
        document.project.pointMasses.front().marks[3] = {.frame = 8, .x = std::nextafter(16.0, 17.0), .y = 16};
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Expected source after native reopen.";
        }
        EXPECT_THROW(motrack::writeTrackerProject(output, document.project, *document.trackerSource), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), xml);
    }
    auto withPlaceholder = xml;
    const auto data = withPlaceholder.find("<property name=\"framedata\"");
    const auto index = withPlaceholder.find("</property>\n    </object>", data);
    ASSERT_NE(index, std::string::npos);
    withPlaceholder.insert(index, "<property name=\"[500001]\" type=\"object\">null</property>\n");
    auto unsafe = motrack::parseTrackerProject(withPlaceholder);
    motrack::writeTrackerProject(output, unsafe.project, unsafe.source); // Unchanged bytes still pass through.
    if (!unsafe.project.calibration)
    {
        FAIL() << "Expected calibration around the null placeholder.";
    }
    unsafe.project.calibration->keyframes[1].originX += 1;
    EXPECT_THROW(motrack::writeTrackerProject(output, unsafe.project, unsafe.source), motrack::ProjectError);
    EXPECT_EQ(readBytes(output), withPlaceholder);
}

TEST(TrackerWriter, RejectsVectorReconstructionErrorsAmplifiedByNewOrExistingCalibration)
{
    auto fixture =
        motrack::parseTrackerProject(readBytes(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/independent-vectors.trk"));
    fixture.project.vectors.front().samples = {{.frame = 0, .tailX = 10, .tailY = 20, .componentX = 0, .componentY = -4}};
    fixture.project.vectors.back().samples.clear();
    fixture = motrack::parseTrackerProject(motrack::renderTrackerProject(fixture.project, fixture.source));
    if (!fixture.project.calibration)
    {
        FAIL() << "Expected fixed source calibration.";
    }
    auto calibration = *fixture.project.calibration;
    calibration.keyframes.front() = {.frame = 0, .originX = 0, .originY = 0, .angleDegrees = 0, .scaleX = 1e-100, .scaleY = 1e100};
    QTemporaryDir temporary;
    const auto output = temporary.filePath("existing.trk");
    const auto native = temporary.filePath("reconstruction.motrack");
    for (const bool existing : {false, true})
    {
        auto imported = fixture;
        if (existing)
        {
            imported.project.calibration = calibration;
            imported = motrack::parseTrackerProject(motrack::renderTrackerProject(imported.project, imported.source));
        }
        motrack::writeTrackerProject(output, imported.project, imported.source);
        motrack::WorkspaceDocument document{.project = imported.project, .movieFrameTimes = {}, .trackerSource = imported.source};
        document.project.calibration = calibration;
        document.project.vectors.front().samples.front().componentX = 1e-20;
        motrack::writeWorkspaceDocument(native, document);
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            FAIL() << "Expected preserved source after native reopen.";
        }
        EXPECT_THROW(motrack::writeTrackerProject(output, document.project, *document.trackerSource), motrack::ProjectError);
        EXPECT_EQ(readBytes(output), imported.source.xml);
    }
}

} // namespace
