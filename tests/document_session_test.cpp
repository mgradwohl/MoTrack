#include "document_session.hpp"

#include "tracker_archive.hpp"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

namespace
{

QString fixturePath(const QString& name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + '/' + name;
}

TEST(DocumentSession, MediaOriginsDoNotInferAnExistingProjectSaveTarget)
{
    QTemporaryDir temporary;
    motrack::DocumentSession session;
    const auto media = temporary.filePath("image.motrack");
    session.replace({}, media, motrack::DocumentSourceKind::Media);
    EXPECT_EQ(session.sourcePath(), media);
    EXPECT_TRUE(session.nativeSavePath().isEmpty());
    session.replace({}, media);
    EXPECT_EQ(session.nativeSavePath(), media);
}

TEST(DocumentSession, IsolatesDataHistoriesAndSaveCheckpointsBetweenDocuments)
{
    QTemporaryDir temporary;
    motrack::DocumentSession first;
    motrack::DocumentSession second;
    const auto source = fixturePath("point-mass.trk");
    first.replace(motrack::importTrackerDocument(source), source);
    second.replace(motrack::importTrackerDocument(source), source);
    EXPECT_EQ(first.sourcePath(), QFileInfo(source).absoluteFilePath());
    EXPECT_TRUE(first.nativeSavePath().isEmpty());
    ASSERT_TRUE(first.history.renamePointMass(first.document.project, 0, "first"));
    ASSERT_TRUE(second.history.renamePointMass(second.document.project, 0, "second"));
    const auto secondRevision = second.history.revision();
    const auto checkpoint = first.saveCheckpoint();
    const auto destination = temporary.filePath("first.motrack");
    EXPECT_EQ(second.completeSave(checkpoint, destination, true), motrack::SaveOutcome::Replaced);
    EXPECT_EQ(second.history.revision(), secondRevision);
    EXPECT_TRUE(second.history.isModified());
    EXPECT_TRUE(second.nativeSavePath().isEmpty());
    EXPECT_EQ(first.completeSave(checkpoint, destination, true), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(first.history.isModified());
    EXPECT_EQ(first.nativeSavePath(), destination);
    EXPECT_EQ(first.sourcePath(), QFileInfo(source).absoluteFilePath());
    ASSERT_TRUE(first.history.undo(first.document.project));
    EXPECT_TRUE(first.history.isModified());
    EXPECT_EQ(second.document.project.pointMasses.front().name, "second");
    EXPECT_FALSE(second.history.canRedo());
    ASSERT_TRUE(first.history.redo(first.document.project));
    EXPECT_FALSE(first.history.isModified());
    EXPECT_EQ(first.document.project.pointMasses.front().name, "first");
}

TEST(DocumentSession, RejectsOldCompletionsAfterReplacementAndResetsOnlyTheReplacedDocument)
{
    QTemporaryDir temporary;
    motrack::DocumentSession session;
    session.replace({}, temporary.filePath("old.motrack"));
    ASSERT_TRUE(session.history.createPointMass(session.document.project, "old"));
    const auto checkpoint = session.saveCheckpoint();
    const auto destination = temporary.filePath("new.MOTRACK");
    session.replace({}, destination);
    EXPECT_FALSE(session.history.canUndo());
    EXPECT_FALSE(session.history.isModified());
    EXPECT_TRUE(session.document.project.pointMasses.empty());
    EXPECT_EQ(session.nativeSavePath(), destination);
    for (const bool success : {false, true})
    {
        EXPECT_EQ(session.completeSave(checkpoint, temporary.filePath("old-save.motrack"), success), motrack::SaveOutcome::Replaced);
        EXPECT_EQ(session.nativeSavePath(), destination);
        EXPECT_FALSE(session.history.isModified());
    }
    session.replace({}, temporary.filePath("new.trk"));
    EXPECT_TRUE(session.nativeSavePath().isEmpty());
}

TEST(DocumentSession, FailedSavesRetainTheCheckpointAndSuccessfulStaleSavesInvalidateIt)
{
    QTemporaryDir temporary;
    motrack::DocumentSession session;
    const auto source = fixturePath("point-mass.trk");
    session.replace(motrack::importTrackerDocument(source), source);
    const auto original = temporary.filePath("original.motrack");
    EXPECT_EQ(session.completeSave(session.saveCheckpoint(), original, true), motrack::SaveOutcome::Saved);
    const auto stale = session.saveCheckpoint();
    ASSERT_TRUE(session.history.renamePointMass(session.document.project, 0, "edited"));
    const auto attempted = temporary.filePath("save-as.motrack");
    EXPECT_EQ(session.completeSave(stale, attempted, false), motrack::SaveOutcome::Failed);
    EXPECT_EQ(session.nativeSavePath(), original);
    ASSERT_TRUE(session.history.undo(session.document.project));
    EXPECT_FALSE(session.history.isModified());
    ASSERT_TRUE(session.history.redo(session.document.project));
    EXPECT_EQ(session.completeSave(stale, attempted, true), motrack::SaveOutcome::Stale);
    EXPECT_EQ(session.nativeSavePath(), attempted);
    EXPECT_TRUE(session.history.isModified());
    ASSERT_TRUE(session.history.undo(session.document.project));
    EXPECT_TRUE(session.history.isModified());
    EXPECT_EQ(session.completeSave(session.saveCheckpoint(), attempted, true), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(session.history.isModified());
}

TEST(DocumentSession, AnOwnedSaveSnapshotOutlivesReplacementAndRetainsArchiveAndSource)
{
    QTemporaryDir temporary;
    const auto source = fixturePath("archive.trz");
    motrack::DocumentSession session;
    auto archive = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(source));
    auto imported = motrack::importTrackerDocument(archive->projectPath("image-sequence.trk"));
    imported.archivePayload = archive;
    imported.archiveMember = "image-sequence.trk";
    session.replace(std::move(imported), source);
    ASSERT_TRUE(session.history.renamePointMass(session.document.project, 0, "saved snapshot"));
    auto snapshot = session.document;
    const auto checkpoint = session.saveCheckpoint();
    const auto ownedPath = archive->sourcePath();
    const auto extracted = archive->root();
    archive.reset();
    session.replace({}, temporary.filePath("replacement.trk"));
    EXPECT_FALSE(session.document.trackerSource);
    EXPECT_FALSE(session.document.archivePayload);
    EXPECT_FALSE(session.document.archiveMember);
    ASSERT_TRUE(QFileInfo::exists(ownedPath));
    const auto destination = temporary.filePath("snapshot.motrack");
    motrack::writeWorkspaceDocument(destination, snapshot);
    EXPECT_EQ(session.completeSave(checkpoint, destination, true), motrack::SaveOutcome::Replaced);
    EXPECT_TRUE(session.nativeSavePath().isEmpty());
    snapshot = {};
    EXPECT_FALSE(QFileInfo::exists(ownedPath));
    EXPECT_FALSE(QFileInfo::exists(extracted));
    const auto reopened = motrack::readWorkspaceDocument(destination);
    EXPECT_EQ(reopened.project.pointMasses.front().name, "saved snapshot");
    EXPECT_TRUE(reopened.trackerSource);
    EXPECT_TRUE(reopened.archivePayload);
    EXPECT_EQ(reopened.archiveMember, "image-sequence.trk");
}

} // namespace
