#include "paired_save.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"
#include "tracker_writer.hpp"
#include "video_reader.hpp"
#include "workspace_document.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <vector>

namespace
{
QString fixture(const QString& name = "linked-movie.trk")
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-mov/" + name;
}

QByteArray bytes(const QString& path)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
        throw motrack::ProjectError("Could not read movie package fixture.");
    return input.readAll();
}

void put(const QString& path, const QByteArray& value)
{
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(value) != value.size())
        throw motrack::ProjectError("Could not write movie package fixture.");
}

motrack::WorkspaceDocument indexedDocument(const QString& path = fixture())
{
    auto document = motrack::importTrackerDocument(path);
    const std::atomic_bool cancelled{false};
    const auto index =
        motrack::buildMovieIndex(QFileInfo(path).dir().filePath("constant-rate.mov"), motrack::VideoOrientation::Encoded, cancelled);
    document.movie = true;
    document.alignMovieToClip = true;
    document.movieFrameTimes = index.frameTimes;
    document.project.clip.sourceFrameCount = index.frameCount;
    return document;
}

TEST(TrackerMoviePackage, RetainsEditedClipAndCompleteMovieAfterNativeReopenAndSourceRemoval)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto source = temporary.filePath("input.trk");
    const auto movie = temporary.filePath("constant-rate.mov");
    ASSERT_TRUE(QFile::copy(fixture(), source));
    ASSERT_TRUE(QFile::copy(fixture("constant-rate.mov"), movie));
    auto document = indexedDocument(source);
    ASSERT_EQ(document.movieFrameTimes, (std::vector<double>{0, 0.125, 0.25, 0.375}));
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5));
    ASSERT_TRUE(edits.markPointMass(document.project, 0, {.frame = 1, .x = 8, .y = 12}));
    ASSERT_TRUE(edits.setClipRange(document.project, 1, 3, 2));
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    const auto native = temporary.filePath("saved.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = {};
    ASSERT_TRUE(QFile::remove(source));
    document = motrack::readWorkspaceDocument(native);
    motrack::retainWorkspaceMediaLocations(document, native);
    const auto before = document.project;
    const auto bindings = document.mediaLocations;
    if (!document.trackerSource)
        FAIL() << "Native reopen lost its source.";
    const auto originalXml = document.trackerSource->xml;
    const auto target = temporary.filePath(QString::fromUtf8("portable é & movie.trz"));
    motrack::writeTrackerMoviePackageCopy(target, document, native);
    EXPECT_EQ(document.project, before);
    EXPECT_EQ(document.mediaLocations, bindings);
    if (!document.trackerSource)
        FAIL() << "Packaging removed the native source.";
    EXPECT_EQ(document.trackerSource->xml, originalXml);
    ASSERT_TRUE(QFile::remove(native));
    ASSERT_TRUE(QFile::remove(movie));
    ASSERT_TRUE(QDir(temporary.path()).mkdir("relocated"));
    const auto relocated = temporary.filePath("relocated/experiment.trz");
    ASSERT_TRUE(QFile::rename(target, relocated));
    auto archive = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(relocated));
    ASSERT_EQ(archive->projects(), QStringList{"experiment.trk"});
    auto reopened = motrack::importTrackerDocument(archive->projectPath("experiment.trk"));
    archive->resolveMedia(reopened.project, "experiment.trk");
    auto expected = before;
    expected.clip.sourceFrameCount.reset();
    expected.clip.mediaReferences = {"media/movie.mov"};
    EXPECT_EQ(reopened.project, expected);
    const auto packagedMovie = QDir(archive->root()).filePath("media/movie.mov");
    EXPECT_EQ(bytes(packagedMovie), bytes(fixture("constant-rate.mov")));
    const std::atomic_bool cancelled{false};
    const auto index = motrack::buildMovieIndex(packagedMovie, motrack::VideoOrientation::Encoded, cancelled);
    ASSERT_EQ(index.frameCount, 4);
    EXPECT_EQ(index.frameTimes, document.movieFrameTimes);
    reopened.movie = true;
    reopened.alignMovieToClip = true;
    reopened.movieFrameTimes = index.frameTimes;
    reopened.project.clip.sourceFrameCount = index.frameCount;
    reopened.archivePayload = archive;
    reopened.archiveMember = "experiment.trk";
    const auto portableNative = temporary.filePath("portable.motrack");
    motrack::writeWorkspaceDocument(portableNative, reopened);
    reopened = {};
    archive.reset();
    ASSERT_TRUE(QFile::remove(relocated));
    reopened = motrack::readWorkspaceDocument(portableNative);
    ASSERT_TRUE(edits.setPointMassValue(reopened.project, 0, 7));
    const auto saved = temporary.filePath("resaved.trz");
    motrack::writeTrackerArchiveDocument(saved, reopened);
    const auto again = motrack::TrackerArchive::open(saved);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(again.projectPath("experiment.trk")).project.pointMasses.at(0).mass, 7);
    EXPECT_EQ(bytes(QDir(again.root()).filePath("media/movie.mov")), bytes(fixture("constant-rate.mov")));
}

TEST(TrackerMoviePackage, RefusesUnsupportedOrUnindexedInputsWithoutReplacingDestination)
{
    for (int kind = 0; kind < 12; ++kind)
    {
        SCOPED_TRACE(kind);
        QTemporaryDir temporary;
        auto document = indexedDocument();
        std::atomic_bool cancelled{kind == 0};
        switch (kind)
        {
        case 1:
            document.trackerSource.reset();
            break;
        case 2:
            document.movie = false;
            break;
        case 3:
            document.movieFrameTimes.clear();
            break;
        case 4:
            document.alignMovieToClip = false;
            break;
        case 5:
            document.useDisplayOrientation = true;
            break;
        case 6:
            document.archiveMember = "experiment.trk";
            break;
        case 7:
            document.project.clip.mediaReferences = {"missing.mov"};
            break;
        case 8:
            document.project.clip.mediaReferences = {fixture("../linked-movie/constant-rate.avi").toStdString()};
            break;
        case 9:
            document.project.clip.frameDurationMs = 250;
            break;
        case 10:
            document.project.pointMasses.front().name = "unsupported rename";
            break;
        default:
            break;
        }
        const auto target = temporary.filePath(kind == 11 ? "wrong.trk" : "saved.trz");
        put(target, "existing destination");
        EXPECT_THROW(motrack::writeTrackerMoviePackage(target, document, fixture(), &cancelled), motrack::ProjectError);
        EXPECT_EQ(bytes(target), "existing destination");
    }
}

TEST(TrackerMoviePackage, RejectsCorruptAndChangedTimingInTheStagedMovie)
{
    for (const bool changedTiming : {false, true})
    {
        SCOPED_TRACE(changedTiming);
        QTemporaryDir temporary;
        auto document = indexedDocument();
        const auto source = temporary.filePath(QString::fromUtf8("changed é.mov"));
        auto payload = bytes(fixture("constant-rate.mov"));
        if (changedTiming)
        {
            const auto atom = payload.indexOf("stts");
            ASSERT_GT(atom, 0);
            payload.replace(atom + 16, 4, QByteArray::fromHex("000000fa")); // 250 ms instead of 125 ms.
        }
        else
            payload = "not a movie";
        put(source, payload);
        if (changedTiming)
        {
            const std::atomic_bool cancelled{false};
            const auto index = motrack::buildMovieIndex(source, motrack::VideoOrientation::Encoded, cancelled);
            ASSERT_EQ(index.frameCount, 4);
            ASSERT_EQ(index.frameTimes, (std::vector<double>{0, 0.25, 0.5, 0.75}));
        }
        document.mediaLocations.insert("constant-rate.mov", source);
        const auto target = temporary.filePath("existing.trz");
        put(target, "original archive");
        EXPECT_THROW(motrack::writeTrackerMoviePackage(target, document, fixture()), motrack::ProjectError);
        EXPECT_EQ(bytes(target), "original archive");
    }
}

TEST(TrackerMoviePackage, CopyHonorsTheSharedSaveLockAndPendingRecovery)
{
    QTemporaryDir temporary;
    auto document = indexedDocument();
    const auto target = temporary.filePath("saved.trz");
    put(target, "original archive");
    QLockFile lock(temporary.filePath(".saved.motrack-save.lock"));
    lock.setStaleLockTime(0);
    ASSERT_TRUE(lock.tryLock(0));
    EXPECT_THROW(motrack::writeTrackerMoviePackageCopy(target, document, fixture()), motrack::SaveDestinationError);
    EXPECT_EQ(bytes(target), "original archive");
    lock.unlock();
    put(temporary.filePath(".saved.motrack-save.json"), "pending recovery");
    EXPECT_THROW(motrack::writeTrackerMoviePackageCopy(target, document, fixture()), motrack::SaveDestinationError);
    EXPECT_EQ(bytes(target), "original archive");
}
} // namespace
