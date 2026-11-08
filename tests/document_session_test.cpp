#include "document_session.hpp"

#include "tracker_archive.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <vector>

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
    EXPECT_TRUE(session.standardSavePath().isEmpty());
    session.replace({}, temporary.filePath("image.trk"), motrack::DocumentSourceKind::Media);
    EXPECT_TRUE(session.standardSavePath().isEmpty());
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

TEST(DocumentSession, NativeOnlySavingCannotClearAnOutstandingStandardCheckpoint)
{
    QTemporaryDir temporary;
    motrack::DocumentSession session;
    const auto source = fixturePath("point-mass.trk");
    session.replace(motrack::importTrackerDocument(source), source);
    EXPECT_EQ(session.standardSavePath(), QFileInfo(source).absoluteFilePath());
    EXPECT_FALSE(session.requiresPairedSave());
    ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 5));
    const motrack::PairedSaveResult saved{.status = motrack::PairedSaveStatus::Saved,
                                          .standardPath = temporary.filePath("saved.trk"),
                                          .nativePath = temporary.filePath("saved.motrack"),
                                          .standardCurrent = true,
                                          .nativeCurrent = true};
    EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), saved), motrack::SaveOutcome::Saved);
    EXPECT_TRUE(session.requiresPairedSave());
    EXPECT_FALSE(session.isModified());
    ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 7));
    EXPECT_EQ(session.completeSave(session.saveCheckpoint(), saved.nativePath, true), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(session.history.isModified());
    EXPECT_TRUE(session.history.isModified(motrack::SaveTarget::Tracker));
    EXPECT_TRUE(session.isModified());
    ASSERT_TRUE(session.history.undo(session.document.project));
    EXPECT_FALSE(session.history.isModified(motrack::SaveTarget::Tracker));
    EXPECT_TRUE(session.history.isModified());
    EXPECT_TRUE(session.isModified());
    ASSERT_TRUE(session.history.redo(session.document.project));
    EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), saved), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(session.isModified());
    ASSERT_TRUE(session.history.undo(session.document.project));
    EXPECT_TRUE(session.isModified());
    ASSERT_TRUE(session.history.redo(session.document.project));
    EXPECT_FALSE(session.isModified());
}

TEST(DocumentSession, PartialWritesAndCleanupFailuresKeepThePairDirty)
{
    QTemporaryDir temporary;
    for (const bool nativeSaved : {false, true})
    {
        SCOPED_TRACE(nativeSaved);
        motrack::DocumentSession session;
        const auto source = fixturePath("point-mass.trk");
        session.replace(motrack::importTrackerDocument(source), source);
        ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 5));
        const auto checkpoint = session.saveCheckpoint();
        const motrack::PairedSaveResult partial{.status = motrack::PairedSaveStatus::RecoveryRequired,
                                                .standardPath = temporary.filePath("saved.trk"),
                                                .nativePath = temporary.filePath("saved.motrack"),
                                                .recoveryPath = temporary.filePath(".saved.motrack-save.json"),
                                                .standardCurrent = true,
                                                .nativeCurrent = nativeSaved};
        EXPECT_EQ(session.completePairedSave(checkpoint, partial), motrack::SaveOutcome::Partial);
        EXPECT_FALSE(session.history.isModified(motrack::SaveTarget::Tracker));
        EXPECT_EQ(session.history.isModified(), !nativeSaved);
        EXPECT_TRUE(session.isModified());
        EXPECT_EQ(session.recoveryPath(), partial.recoveryPath);
        EXPECT_EQ(session.completePairedRecovery(session.saveCheckpoint(), partial.recoveryPath, partial), motrack::SaveOutcome::Partial);
        EXPECT_TRUE(session.isModified());
        auto complete = partial;
        complete.status = motrack::PairedSaveStatus::Saved;
        complete.nativeCurrent = true;
        complete.recoveryPath.clear();
        EXPECT_EQ(session.completePairedRecovery(session.saveCheckpoint(), "another record", complete), motrack::SaveOutcome::Replaced);
        EXPECT_EQ(session.completePairedRecovery(session.saveCheckpoint(), partial.recoveryPath, complete),
                  motrack::SaveOutcome::Recovered);
        EXPECT_TRUE(session.recoveryPath().isEmpty());
        EXPECT_TRUE(session.isModified());
        EXPECT_TRUE(session.history.isModified());
        EXPECT_TRUE(session.history.isModified(motrack::SaveTarget::Tracker));
        EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), complete), motrack::SaveOutcome::Saved);
        EXPECT_FALSE(session.isModified());
    }
}

TEST(DocumentSession, RefusesStaleAndReplacedPairCompletions)
{
    QTemporaryDir temporary;
    motrack::DocumentSession session;
    const auto source = fixturePath("point-mass.trk");
    session.replace(motrack::importTrackerDocument(source), source);
    const auto checkpoint = session.saveCheckpoint();
    const motrack::PairedSaveResult saved{.status = motrack::PairedSaveStatus::Saved,
                                          .standardPath = temporary.filePath("saved.trk"),
                                          .nativePath = temporary.filePath("saved.motrack"),
                                          .standardCurrent = true,
                                          .nativeCurrent = true};
    ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 5));
    for (const auto status : {motrack::PairedSaveStatus::Failed, motrack::PairedSaveStatus::Cancelled})
    {
        EXPECT_EQ(session.completePairedSave(checkpoint, {.status = status}), motrack::SaveOutcome::Failed);
        EXPECT_FALSE(session.requiresPairedSave());
        EXPECT_TRUE(session.isModified());
        EXPECT_TRUE(session.nativeSavePath().isEmpty());
    }
    EXPECT_EQ(session.completePairedSave(checkpoint, saved), motrack::SaveOutcome::Stale);
    EXPECT_TRUE(session.isModified());
    ASSERT_TRUE(session.history.undo(session.document.project));
    EXPECT_TRUE(session.isModified());
    session.replace({}, temporary.filePath("replacement.motrack"));
    EXPECT_EQ(session.completePairedSave(checkpoint, saved), motrack::SaveOutcome::Replaced);
    EXPECT_EQ(session.completePairedRecovery(checkpoint, ".saved.motrack-save.json", saved), motrack::SaveOutcome::Replaced);
    EXPECT_FALSE(session.requiresPairedSave());
    EXPECT_TRUE(session.standardSavePath().isEmpty());
    EXPECT_TRUE(session.recoveryPath().isEmpty());
    EXPECT_FALSE(session.isModified());
}

TEST(DocumentSession, RetainsInterruptedSavesAcrossDifferentDestinations)
{
    QTemporaryDir temporary;
    const auto source = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
    motrack::DocumentSession session;
    session.replace(motrack::importTrackerDocument(source), source);
    ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 5));
    std::vector<motrack::PairedSaveResult> pending;
    for (const auto* name : {"first.trk", "second.trk"})
    {
        std::atomic_bool cancelled{false};
        pending.push_back(motrack::writePairedDocuments(temporary.filePath(name),
                                                        session.document,
                                                        source,
                                                        &cancelled,
                                                        [&cancelled](auto state)
                                                        { cancelled = state == motrack::PairedSaveProgress::StandardSaved; }));
        ASSERT_EQ(pending.back().status, motrack::PairedSaveStatus::RecoveryRequired);
        EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), pending.back()), motrack::SaveOutcome::Partial);
        EXPECT_EQ(session.recoveryPath(), pending.front().recoveryPath);
    }
    // Repeated completion of one record does not create a second pending entry.
    EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), pending.back()), motrack::SaveOutcome::Partial);
    const auto current = motrack::writePairedDocuments(temporary.filePath("current.trk"), session.document, source);
    ASSERT_EQ(current.status, motrack::PairedSaveStatus::Saved);
    EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), current), motrack::SaveOutcome::Partial);
    EXPECT_TRUE(session.isModified());
    EXPECT_EQ(session.recoveryPath(), pending.front().recoveryPath);
    for (const auto& interrupted : pending)
    {
        EXPECT_EQ(session.recoveryPath(), interrupted.recoveryPath);
        EXPECT_TRUE(QFileInfo::exists(interrupted.recoveryPath));
        const auto recovered = motrack::recoverPairedDocuments(interrupted.recoveryPath);
        ASSERT_EQ(recovered.status, motrack::PairedSaveStatus::Saved);
        EXPECT_EQ(session.completePairedRecovery(session.saveCheckpoint(), interrupted.recoveryPath, recovered),
                  motrack::SaveOutcome::Recovered);
        EXPECT_FALSE(QFileInfo::exists(interrupted.recoveryPath));
        EXPECT_EQ(session.standardSavePath(), current.standardPath);
        EXPECT_EQ(session.nativeSavePath(), current.nativePath);
        EXPECT_TRUE(session.isModified());
    }
    EXPECT_TRUE(session.recoveryPath().isEmpty());
    const auto saved = motrack::writePairedDocuments(current.standardPath, session.document, source);
    ASSERT_EQ(saved.status, motrack::PairedSaveStatus::Saved);
    EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), saved), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(session.isModified());
}

TEST(DocumentSession, RecoveryPreservesANativeOnlySaveAsTarget)
{
    QTemporaryDir temporary;
    const auto source = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
    motrack::DocumentSession session;
    session.replace(motrack::importTrackerDocument(source), source);
    std::atomic_bool cancelled{false};
    const auto partial =
        motrack::writePairedDocuments(temporary.filePath("paired.trk"),
                                      session.document,
                                      source,
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
    ASSERT_EQ(session.completePairedSave(session.saveCheckpoint(), partial), motrack::SaveOutcome::Partial);
    const auto recoveryCheckpoint = session.saveCheckpoint();
    const auto recovered = motrack::recoverPairedDocuments(partial.recoveryPath);
    ASSERT_EQ(recovered.status, motrack::PairedSaveStatus::Saved);
    ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 7));
    const auto copy = temporary.filePath("copy.motrack");
    motrack::writeWorkspaceDocument(copy, session.document);
    ASSERT_EQ(session.completeSave(session.saveCheckpoint(), copy, true), motrack::SaveOutcome::Saved);
    EXPECT_EQ(session.completePairedRecovery(recoveryCheckpoint, partial.recoveryPath, recovered), motrack::SaveOutcome::Recovered);
    EXPECT_EQ(session.nativeSavePath(), copy);
    EXPECT_EQ(session.standardSavePath(), partial.standardPath);
    EXPECT_TRUE(session.recoveryPath().isEmpty());
    EXPECT_TRUE(session.isModified());
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(copy).project.pointMasses.at(0).mass, 7);
}

TEST(DocumentSession, DelayedRecoveryCannotRemoveANewerTransactionAtTheSamePath)
{
    QTemporaryDir temporary;
    const auto source = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
    const auto target = temporary.filePath("paired.trk");
    motrack::DocumentSession session;
    session.replace(motrack::importTrackerDocument(source), source);
    std::atomic_bool cancelled{false};
    const auto interrupt = [&cancelled](auto state)
    {
        cancelled = state == motrack::PairedSaveProgress::StandardSaved;
    };
    const auto first = motrack::writePairedDocuments(target, session.document, source, &cancelled, interrupt);
    ASSERT_EQ(first.status, motrack::PairedSaveStatus::RecoveryRequired);
    ASSERT_EQ(session.completePairedSave(session.saveCheckpoint(), first), motrack::SaveOutcome::Partial);
    const auto firstCheckpoint = session.saveCheckpoint();
    const auto firstRecovery = motrack::recoverPairedDocuments(first.recoveryPath);
    ASSERT_EQ(firstRecovery.status, motrack::PairedSaveStatus::Saved);
    ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 7));
    cancelled = false;
    const auto second = motrack::writePairedDocuments(target, session.document, source, &cancelled, interrupt);
    ASSERT_EQ(second.status, motrack::PairedSaveStatus::RecoveryRequired);
    ASSERT_EQ(first.recoveryPath, second.recoveryPath);
    ASSERT_NE(first.transactionId, second.transactionId);
    ASSERT_EQ(session.completePairedSave(session.saveCheckpoint(), second), motrack::SaveOutcome::Partial);
    EXPECT_EQ(session.completePairedRecovery(firstCheckpoint, first.recoveryPath, firstRecovery), motrack::SaveOutcome::Recovered);
    EXPECT_EQ(session.recoveryPath(), second.recoveryPath);
    EXPECT_TRUE(QFileInfo::exists(second.recoveryPath));
    const auto current = motrack::writePairedDocuments(temporary.filePath("current.trk"), session.document, source);
    ASSERT_EQ(current.status, motrack::PairedSaveStatus::Saved);
    EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), current), motrack::SaveOutcome::Partial);
    EXPECT_TRUE(session.isModified());
    const auto secondCheckpoint = session.saveCheckpoint();
    // Even a current checkpoint cannot apply another transaction's successful result.
    EXPECT_EQ(session.completePairedRecovery(secondCheckpoint, second.recoveryPath, firstRecovery), motrack::SaveOutcome::Stale);
    const auto secondRecovery = motrack::recoverPairedDocuments(second.recoveryPath);
    ASSERT_EQ(secondRecovery.status, motrack::PairedSaveStatus::Saved);
    EXPECT_EQ(session.completePairedRecovery(secondCheckpoint, second.recoveryPath, secondRecovery), motrack::SaveOutcome::Recovered);
    EXPECT_TRUE(session.recoveryPath().isEmpty());
    EXPECT_TRUE(session.isModified());
    const auto saved = motrack::writePairedDocuments(current.standardPath, session.document, source);
    ASSERT_EQ(saved.status, motrack::PairedSaveStatus::Saved);
    EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), saved), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(session.isModified());
}

TEST(DocumentSession, BlockedRetriesCannotRestoreAnOlderSaveTargetOrInvalidateNewerSavePoints)
{
    QTemporaryDir temporary;
    const auto source = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
    const auto target = temporary.filePath("interrupted.trk");
    motrack::DocumentSession session;
    session.replace(motrack::importTrackerDocument(source), source);
    std::atomic_bool cancelled{false};
    const auto partial =
        motrack::writePairedDocuments(target,
                                      session.document,
                                      source,
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
    ASSERT_EQ(session.completePairedSave(session.saveCheckpoint(), partial), motrack::SaveOutcome::Partial);
    const auto retryCheckpoint = session.saveCheckpoint();
    const auto blocked = motrack::writePairedDocuments(target, session.document, source);
    ASSERT_EQ(blocked.status, motrack::PairedSaveStatus::RecoveryRequired);
    ASSERT_FALSE(blocked.standardCurrent || blocked.nativeCurrent);
    ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 7));
    const auto current = motrack::writePairedDocuments(temporary.filePath("current.trk"), session.document, source);
    ASSERT_EQ(current.status, motrack::PairedSaveStatus::Saved);
    ASSERT_EQ(session.completePairedSave(session.saveCheckpoint(), current), motrack::SaveOutcome::Partial);
    EXPECT_FALSE(session.history.isModified(motrack::SaveTarget::Tracker));
    EXPECT_FALSE(session.history.isModified());
    EXPECT_EQ(session.completePairedSave(retryCheckpoint, blocked), motrack::SaveOutcome::Partial);
    EXPECT_EQ(session.standardSavePath(), current.standardPath);
    EXPECT_EQ(session.nativeSavePath(), current.nativePath);
    EXPECT_FALSE(session.history.isModified(motrack::SaveTarget::Tracker));
    EXPECT_FALSE(session.history.isModified());
    EXPECT_EQ(session.recoveryPath(), partial.recoveryPath);
    EXPECT_TRUE(session.isModified());
    const auto recoveryCheckpoint = session.saveCheckpoint();
    const auto recovered = motrack::recoverPairedDocuments(partial.recoveryPath);
    ASSERT_EQ(recovered.status, motrack::PairedSaveStatus::Saved);
    EXPECT_EQ(session.completePairedRecovery(recoveryCheckpoint, partial.recoveryPath, recovered), motrack::SaveOutcome::Recovered);
    EXPECT_EQ(session.standardSavePath(), current.standardPath);
    EXPECT_EQ(session.nativeSavePath(), current.nativePath);
    const auto saved = motrack::writePairedDocuments(session.standardSavePath(), session.document, source);
    ASSERT_EQ(saved.status, motrack::PairedSaveStatus::Saved);
    EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), saved), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(session.isModified());
}

TEST(DocumentSession, RetainsNewTransactionsWhoseCheckpointsPredateAnOlderSaveRegistration)
{
    QTemporaryDir temporary;
    const auto source = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
    const auto target = temporary.filePath("paired.trk");
    motrack::DocumentSession session;
    session.replace(motrack::importTrackerDocument(source), source);
    const auto firstCheckpoint = session.saveCheckpoint();
    const auto secondCheckpoint = session.saveCheckpoint();
    ASSERT_TRUE(secondCheckpoint.recoveryTransactions.isEmpty());
    std::atomic_bool cancelled{false};
    const auto interrupt = [&cancelled](auto state)
    {
        cancelled = state == motrack::PairedSaveProgress::StandardSaved;
    };
    const auto first = motrack::writePairedDocuments(target, session.document, source, &cancelled, interrupt);
    ASSERT_EQ(session.completePairedSave(firstCheckpoint, first), motrack::SaveOutcome::Partial);
    const auto recoveryCheckpoint = session.saveCheckpoint();
    const auto firstRecovery = motrack::recoverPairedDocuments(first.recoveryPath);
    ASSERT_EQ(firstRecovery.status, motrack::PairedSaveStatus::Saved);
    ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 7));
    cancelled = false;
    const auto second = motrack::writePairedDocuments(target, session.document, source, &cancelled, interrupt);
    ASSERT_EQ(second.status, motrack::PairedSaveStatus::RecoveryRequired);
    EXPECT_EQ(session.completePairedSave(secondCheckpoint, second), motrack::SaveOutcome::Partial);
    EXPECT_EQ(session.completePairedRecovery(recoveryCheckpoint, session.recoveryPath(), firstRecovery), motrack::SaveOutcome::Recovered);
    EXPECT_EQ(session.recoveryPath(), second.recoveryPath);
    EXPECT_EQ(session.saveCheckpoint().recoveryTransactions.value(second.recoveryPath), second.transactionId);
    const auto other = motrack::writePairedDocuments(temporary.filePath("other.trk"), session.document, source);
    ASSERT_EQ(other.status, motrack::PairedSaveStatus::Saved);
    EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), other), motrack::SaveOutcome::Partial);
    EXPECT_TRUE(session.isModified());
    EXPECT_TRUE(QFileInfo::exists(second.recoveryPath));
    const auto secondRecovery = motrack::recoverPairedDocuments(second.recoveryPath);
    EXPECT_EQ(session.completePairedRecovery(session.saveCheckpoint(), session.recoveryPath(), secondRecovery),
              motrack::SaveOutcome::Recovered);
    const auto saved = motrack::writePairedDocuments(other.standardPath, session.document, source);
    EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), saved), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(session.isModified());
}

TEST(DocumentSession, OriginalInterruptedResultsCannotResurrectTransactionsCompletedThroughARetry)
{
    QTemporaryDir temporary;
    const auto source = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
    const auto target = temporary.filePath("paired.trk");
    motrack::DocumentSession session;
    session.replace(motrack::importTrackerDocument(source), source);
    const auto originalCheckpoint = session.saveCheckpoint();
    std::atomic_bool cancelled{false};
    const auto first =
        motrack::writePairedDocuments(target,
                                      session.document,
                                      source,
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    ASSERT_EQ(first.status, motrack::PairedSaveStatus::RecoveryRequired);
    const auto retry = motrack::writePairedDocuments(target, session.document, source);
    ASSERT_EQ(retry.transactionId, first.transactionId);
    ASSERT_EQ(session.completePairedSave(session.saveCheckpoint(), retry), motrack::SaveOutcome::Partial);
    const auto recovered = motrack::recoverPairedDocuments(first.recoveryPath);
    ASSERT_EQ(session.completePairedRecovery(session.saveCheckpoint(), session.recoveryPath(), recovered), motrack::SaveOutcome::Recovered);
    EXPECT_EQ(session.completePairedSave(originalCheckpoint, first), motrack::SaveOutcome::Stale);
    EXPECT_TRUE(session.recoveryPath().isEmpty());
    EXPECT_TRUE(session.saveCheckpoint().recoveryTransactions.isEmpty());
    const auto saved = motrack::writePairedDocuments(temporary.filePath("other.trk"), session.document, source);
    EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), saved), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(session.isModified());
    session.replace(motrack::importTrackerDocument(source), source);
    EXPECT_EQ(session.completePairedSave(originalCheckpoint, first), motrack::SaveOutcome::Replaced);
}

TEST(DocumentSession, DelayedBlockedRetriesCannotReplaceNewerOrClearedRecoveryIdentities)
{
    QTemporaryDir temporary;
    const auto source = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
    const auto target = temporary.filePath("paired.trk");
    motrack::DocumentSession session;
    session.replace(motrack::importTrackerDocument(source), source);
    std::atomic_bool cancelled{false};
    const auto interrupt = [&cancelled](auto state)
    {
        cancelled = state == motrack::PairedSaveProgress::StandardSaved;
    };
    const auto first = motrack::writePairedDocuments(target, session.document, source, &cancelled, interrupt);
    ASSERT_EQ(session.completePairedSave(session.saveCheckpoint(), first), motrack::SaveOutcome::Partial);
    const auto blockedCheckpoint = session.saveCheckpoint();
    const auto blocked = motrack::writePairedDocuments(target, session.document, source);
    ASSERT_EQ(blocked.transactionId, first.transactionId);
    const auto firstRecovery = motrack::recoverPairedDocuments(first.recoveryPath);
    ASSERT_EQ(firstRecovery.status, motrack::PairedSaveStatus::Saved);
    EXPECT_EQ(session.completePairedRecovery(session.saveCheckpoint(), session.recoveryPath(), firstRecovery),
              motrack::SaveOutcome::Recovered);
    EXPECT_EQ(session.completePairedSave(blockedCheckpoint, blocked), motrack::SaveOutcome::Stale);
    EXPECT_TRUE(session.recoveryPath().isEmpty());
    ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 7));
    cancelled = false;
    const auto second = motrack::writePairedDocuments(target, session.document, source, &cancelled, interrupt);
    ASSERT_EQ(session.completePairedSave(session.saveCheckpoint(), second), motrack::SaveOutcome::Partial);
    EXPECT_EQ(session.completePairedSave(blockedCheckpoint, blocked), motrack::SaveOutcome::Stale);
    EXPECT_EQ(session.saveCheckpoint().recoveryTransactions.value(second.recoveryPath), second.transactionId);
    const auto secondRecovery = motrack::recoverPairedDocuments(second.recoveryPath);
    EXPECT_EQ(session.completePairedRecovery(session.saveCheckpoint(), session.recoveryPath(), secondRecovery),
              motrack::SaveOutcome::Recovered);
    EXPECT_TRUE(session.recoveryPath().isEmpty());
    EXPECT_TRUE(session.saveCheckpoint().recoveryTransactions.isEmpty());
}

TEST(DocumentSession, ReconcilesExternallyReplacedAndCompletedRecoveryRecordsWithoutMarkingClean)
{
    for (const bool replaced : {false, true})
    {
        SCOPED_TRACE(replaced);
        QTemporaryDir temporary;
        const auto source = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
        const auto target = temporary.filePath("paired.trk");
        motrack::DocumentSession session;
        session.replace(motrack::importTrackerDocument(source), source);
        std::atomic_bool cancelled{false};
        const auto interrupt = [&cancelled](auto state)
        {
            cancelled = state == motrack::PairedSaveProgress::StandardSaved;
        };
        const auto first = motrack::writePairedDocuments(target, session.document, source, &cancelled, interrupt);
        ASSERT_EQ(session.completePairedSave(session.saveCheckpoint(), first), motrack::SaveOutcome::Partial);
        ASSERT_EQ(motrack::recoverPairedDocuments(first.recoveryPath).status, motrack::PairedSaveStatus::Saved);
        QString secondTransaction;
        if (replaced)
        {
            cancelled = false;
            const auto second = motrack::writePairedDocuments(target, session.document, source, &cancelled, interrupt);
            ASSERT_EQ(second.status, motrack::PairedSaveStatus::RecoveryRequired);
            secondTransaction = second.transactionId;
        }
        const auto checkpoint = session.saveCheckpoint();
        const auto recovery = replaced ? motrack::recoverPairedDocuments(first.recoveryPath, nullptr, {}, first.transactionId)
                                       : motrack::recoverPairedDocuments(first.recoveryPath);
        if (!replaced)
        {
            EXPECT_TRUE(recovery.transactionId.isEmpty());
            EXPECT_GT(recovery.observation, first.observation);
        }
        const auto outcome = session.completePairedRecovery(checkpoint, session.recoveryPath(), recovery);
        if (replaced)
        {
            EXPECT_EQ(outcome, motrack::SaveOutcome::Partial);
            EXPECT_EQ(session.saveCheckpoint().recoveryTransactions.value(first.recoveryPath), secondTransaction);
            const auto confirmed = motrack::recoverPairedDocuments(first.recoveryPath, nullptr, {}, secondTransaction);
            EXPECT_EQ(session.completePairedRecovery(session.saveCheckpoint(), session.recoveryPath(), confirmed),
                      motrack::SaveOutcome::Recovered);
        }
        else
            EXPECT_EQ(outcome, motrack::SaveOutcome::Recovered);
        EXPECT_TRUE(session.recoveryPath().isEmpty());
        EXPECT_TRUE(session.isModified());
        EXPECT_EQ(session.standardSavePath(), target);
        const auto saved = motrack::writePairedDocuments(target, session.document, source);
        EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), saved), motrack::SaveOutcome::Saved);
        EXPECT_FALSE(session.isModified());
    }
}

TEST(DocumentSession, OlderWriteCallbacksCannotReplaceNewerMediaBindings)
{
    for (const bool newestFirst : {false, true})
    {
        SCOPED_TRACE(newestFirst);
        QTemporaryDir temporary;
        const auto source = temporary.filePath("input.trk");
        const auto fixture = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
        ASSERT_TRUE(QFile::copy(fixture, source));
        motrack::DocumentSession session;
        session.replace(motrack::importTrackerDocument(source), source);
        const auto checkpoint = session.saveCheckpoint();
        auto snapshot = session.document;
        std::vector<motrack::PairedSaveResult> results;
        for (const auto& directory : {QString("first"), QString("second")})
        {
            ASSERT_TRUE(QDir(temporary.path()).mkpath(directory));
#ifndef _WIN32
            const auto alias = temporary.filePath("images");
            if (QFileInfo(alias).isSymLink())
                ASSERT_TRUE(QFile::remove(alias));
            ASSERT_TRUE(QFile::link(temporary.filePath(directory), alias));
#endif
            for (const auto& reference : snapshot.project.clip.mediaReferences)
            {
                const auto name = QFileInfo(QString::fromStdString(reference)).fileName();
                const auto path = temporary.filePath(directory + '/' + name);
                ASSERT_TRUE(QFile::copy(QFileInfo(fixture).absolutePath() + "/images/" + name, path));
#ifdef _WIN32
                // Windows symlink creation requires additional privileges. Exercise the same
                // resolved snapshot results using explicit runtime locations on this platform.
                snapshot.mediaLocations.insert(QString::fromStdString(reference), path);
#endif
            }
            results.push_back(motrack::writePairedDocuments(temporary.filePath(directory + ".trk"), snapshot, source));
            ASSERT_EQ(results.back().status, motrack::PairedSaveStatus::Saved);
        }
        ASSERT_GT(results.back().observation, results.front().observation);
        for (const auto index : newestFirst ? std::vector<std::size_t>{1, 0} : std::vector<std::size_t>{0, 1})
            static_cast<void>(session.completePairedSave(checkpoint, results.at(index)));
        EXPECT_EQ(session.document.mediaLocations, results.back().mediaLocations);
#ifndef _WIN32
        ASSERT_TRUE(QFile::remove(temporary.filePath("images")));
#endif
        ASSERT_TRUE(QFile::remove(source));
        for (const auto& reference : snapshot.project.clip.mediaReferences)
            ASSERT_TRUE(QFile::remove(temporary.filePath("first/" + QFileInfo(QString::fromStdString(reference)).fileName())));
        ASSERT_TRUE(QDir(temporary.path()).rmdir("first"));
        const auto saved = motrack::writePairedDocuments(temporary.filePath("final.trk"), session.document, source);
        ASSERT_EQ(saved.status, motrack::PairedSaveStatus::Saved);
        const auto reopened = motrack::readWorkspaceDocument(saved.nativePath);
        EXPECT_EQ(reopened.project.clip.mediaReferences.front(), "second/frame0.png");
        EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), saved), motrack::SaveOutcome::Saved);
        EXPECT_FALSE(session.isModified());
    }
}

TEST(DocumentSession, RecoveryOfAnOlderRecordedSnapshotCannotClearNewEdits)
{
    QTemporaryDir temporary;
    const auto source = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
    motrack::DocumentSession session;
    session.replace(motrack::importTrackerDocument(source), source);
    ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 5));
    const auto checkpoint = session.saveCheckpoint();
    std::atomic_bool cancelled{false};
    const auto partial =
        motrack::writePairedDocuments(temporary.filePath("saved.trk"),
                                      session.document,
                                      source,
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
    EXPECT_EQ(session.completePairedSave(checkpoint, partial), motrack::SaveOutcome::Partial);
    ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 7));
    const auto recovery = motrack::recoverPairedDocuments(partial.recoveryPath);
    ASSERT_EQ(recovery.status, motrack::PairedSaveStatus::Saved);
    EXPECT_EQ(session.completePairedRecovery(session.saveCheckpoint(), partial.recoveryPath, recovery), motrack::SaveOutcome::Recovered);
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(recovery.nativePath).project.pointMasses.at(0).mass, 5);
    EXPECT_DOUBLE_EQ(session.document.project.pointMasses.at(0).mass, 7);
    EXPECT_TRUE(session.isModified());
    const auto current = motrack::writePairedDocuments(recovery.standardPath, session.document, source);
    ASSERT_EQ(current.status, motrack::PairedSaveStatus::Saved);
    EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), current), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(session.isModified());
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(current.nativePath).project.pointMasses.at(0).mass, 7);
}

TEST(DocumentSession, ReorderedSuccessfulWritesCannotClearAStaleDiskSnapshot)
{
    for (const bool paired : {false, true})
    {
        SCOPED_TRACE(paired);
        QTemporaryDir temporary;
        const auto source = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
        const auto target = temporary.filePath("paired.trk");
        const auto native = temporary.filePath("paired.motrack");
        motrack::DocumentSession session;
        session.replace(motrack::importTrackerDocument(source), source);
        ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 5));
        const auto oldCheckpoint = session.saveCheckpoint();
        const auto oldSnapshot = session.document;
        ASSERT_TRUE(session.history.setPointMassValue(session.document.project, 0, 7));
        const auto currentCheckpoint = session.saveCheckpoint();
        if (paired)
        {
            const auto current = motrack::writePairedDocuments(target, session.document, source);
            const auto old = motrack::writePairedDocuments(target, oldSnapshot, source);
            ASSERT_EQ(current.status, motrack::PairedSaveStatus::Saved);
            ASSERT_EQ(old.status, motrack::PairedSaveStatus::Saved);
            EXPECT_EQ(session.completePairedSave(oldCheckpoint, old), motrack::SaveOutcome::Stale);
            EXPECT_EQ(session.completePairedSave(currentCheckpoint, current), motrack::SaveOutcome::Stale);
            EXPECT_TRUE(session.history.isModified(motrack::SaveTarget::Tracker));
            EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 5);
        }
        else
        {
            motrack::writeWorkspaceDocument(native, session.document);
            motrack::writeWorkspaceDocument(native, oldSnapshot);
            EXPECT_EQ(session.completeSave(oldCheckpoint, native, true), motrack::SaveOutcome::Stale);
            EXPECT_EQ(session.completeSave(currentCheckpoint, native, true), motrack::SaveOutcome::Stale);
        }
        EXPECT_TRUE(session.isModified());
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(native).project.pointMasses.at(0).mass, 5);
        const auto freshCheckpoint = session.saveCheckpoint();
        if (paired)
        {
            const auto fresh = motrack::writePairedDocuments(target, session.document, source);
            EXPECT_EQ(session.completePairedSave(freshCheckpoint, fresh), motrack::SaveOutcome::Saved);
        }
        else
        {
            motrack::writeWorkspaceDocument(native, session.document);
            EXPECT_EQ(session.completeSave(freshCheckpoint, native, true), motrack::SaveOutcome::Saved);
        }
        EXPECT_FALSE(session.isModified());
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(native).project.pointMasses.at(0).mass, 7);
    }
}

TEST(DocumentSession, ReconcilesUnknownRecoveryRecordsWithoutResurrectingOlderObservations)
{
    for (const bool newerUnknown : {false, true})
    {
        SCOPED_TRACE(newerUnknown);
        QTemporaryDir temporary;
        const auto source = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
        const auto target = temporary.filePath("paired.trk");
        motrack::DocumentSession session;
        session.replace(motrack::importTrackerDocument(source), source);
        const auto checkpoint = session.saveCheckpoint();
        std::atomic_bool cancelled{false};
        const auto partial =
            motrack::writePairedDocuments(target,
                                          session.document,
                                          source,
                                          &cancelled,
                                          [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
        ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
        QFile journal(partial.recoveryPath);
        ASSERT_TRUE(journal.open(QIODevice::ReadOnly));
        const auto original = journal.readAll();
        journal.close();
        ASSERT_TRUE(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(journal.write("invalid journal"), 15);
        journal.close();
        const auto unknown = motrack::writePairedDocuments(target, session.document, source);
        ASSERT_EQ(unknown.status, motrack::PairedSaveStatus::RecoveryRequired);
        ASSERT_TRUE(unknown.transactionId.isEmpty());
        ASSERT_EQ(session.completePairedSave(checkpoint, unknown), motrack::SaveOutcome::Partial);
        const auto recoveryCheckpoint = session.saveCheckpoint();
        ASSERT_TRUE(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(journal.write(original), original.size());
        journal.close();
        ASSERT_EQ(motrack::recoverPairedDocuments(partial.recoveryPath).status, motrack::PairedSaveStatus::Saved);
        const auto absent = motrack::recoverPairedDocuments(partial.recoveryPath);
        ASSERT_EQ(absent.status, motrack::PairedSaveStatus::RecoveryNotNeeded);
        if (newerUnknown)
        {
            ASSERT_TRUE(journal.open(QIODevice::WriteOnly));
            ASSERT_EQ(journal.write("another unknown journal"), 23);
            journal.close();
            const auto newer = motrack::writePairedDocuments(target, session.document, source);
            ASSERT_EQ(newer.status, motrack::PairedSaveStatus::RecoveryRequired);
            ASSERT_GT(newer.observation, absent.observation);
            ASSERT_EQ(session.completePairedSave(session.saveCheckpoint(), newer), motrack::SaveOutcome::Partial);
            EXPECT_EQ(session.completePairedRecovery(recoveryCheckpoint, partial.recoveryPath, absent), motrack::SaveOutcome::Stale);
            EXPECT_EQ(session.recoveryPath(), partial.recoveryPath);
            ASSERT_TRUE(journal.remove());
            const auto nowAbsent = motrack::recoverPairedDocuments(partial.recoveryPath);
            EXPECT_EQ(session.completePairedRecovery(session.saveCheckpoint(), partial.recoveryPath, nowAbsent),
                      motrack::SaveOutcome::Recovered);
        }
        else
            EXPECT_EQ(session.completePairedRecovery(recoveryCheckpoint, partial.recoveryPath, absent), motrack::SaveOutcome::Recovered);
        EXPECT_TRUE(session.recoveryPath().isEmpty());
        EXPECT_TRUE(session.isModified());
        const auto freshCheckpoint = session.saveCheckpoint();
        const auto saved = motrack::writePairedDocuments(target, session.document, source);
        ASSERT_EQ(session.completePairedSave(freshCheckpoint, saved), motrack::SaveOutcome::Saved);
        EXPECT_EQ(session.completePairedSave(checkpoint, unknown), motrack::SaveOutcome::Stale);
        EXPECT_EQ(session.completePairedSave(checkpoint, partial), motrack::SaveOutcome::Stale);
        EXPECT_TRUE(session.recoveryPath().isEmpty());
        EXPECT_FALSE(session.isModified());
    }
}

TEST(DocumentSession, ANewObservationOfARestoredJournalRemainsPending)
{
    QTemporaryDir temporary;
    const auto source = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
    const auto target = temporary.filePath("paired.trk");
    motrack::DocumentSession session;
    session.replace(motrack::importTrackerDocument(source), source);
    std::atomic_bool cancelled{false};
    const auto partial =
        motrack::writePairedDocuments(target,
                                      session.document,
                                      source,
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    ASSERT_EQ(session.completePairedSave(session.saveCheckpoint(), partial), motrack::SaveOutcome::Partial);
    QFile journal(partial.recoveryPath);
    ASSERT_TRUE(journal.open(QIODevice::ReadOnly));
    const auto original = journal.readAll();
    journal.close();
    const auto recovered = motrack::recoverPairedDocuments(partial.recoveryPath);
    ASSERT_EQ(session.completePairedRecovery(session.saveCheckpoint(), partial.recoveryPath, recovered), motrack::SaveOutcome::Recovered);
    ASSERT_TRUE(journal.open(QIODevice::WriteOnly));
    ASSERT_EQ(journal.write(original), original.size());
    journal.close();
    const auto restored = motrack::writePairedDocuments(target, session.document, source);
    ASSERT_EQ(restored.transactionId, partial.transactionId);
    ASSERT_GT(restored.observation, recovered.observation);
    EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), restored), motrack::SaveOutcome::Partial);
    EXPECT_EQ(session.recoveryPath(), partial.recoveryPath);
    EXPECT_TRUE(session.isModified());
    const auto cleaned = motrack::recoverPairedDocuments(partial.recoveryPath);
    ASSERT_EQ(cleaned.status, motrack::PairedSaveStatus::Saved);
    EXPECT_EQ(session.completePairedRecovery(session.saveCheckpoint(), partial.recoveryPath, cleaned), motrack::SaveOutcome::Recovered);
    EXPECT_TRUE(session.recoveryPath().isEmpty());
}

TEST(DocumentSession, NewerRecoveryFailuresSurviveAnOlderSuccessfulRecoveryCallback)
{
    for (const bool unreadableJournal : {false, true})
    {
        for (const bool successFirst : {false, true})
        {
            SCOPED_TRACE(unreadableJournal);
            SCOPED_TRACE(successFirst);
            QTemporaryDir temporary;
            const auto source = fixturePath("../corpus/fixtures/linked-images/linked-images.trk");
            const auto target = temporary.filePath("paired.trk");
            motrack::DocumentSession session;
            session.replace(motrack::importTrackerDocument(source), source);
            std::atomic_bool cancelled{false};
            const auto partial = motrack::writePairedDocuments(target,
                                                               session.document,
                                                               source,
                                                               &cancelled,
                                                               [&cancelled](auto state)
                                                               { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
            ASSERT_EQ(session.completePairedSave(session.saveCheckpoint(), partial), motrack::SaveOutcome::Partial);
            const auto checkpoint = session.saveCheckpoint();
            QFile journal(partial.recoveryPath);
            ASSERT_TRUE(journal.open(QIODevice::ReadOnly));
            const auto originalJournal = journal.readAll();
            journal.close();
            const auto recovered = motrack::recoverPairedDocuments(partial.recoveryPath, nullptr, {}, partial.transactionId);
            ASSERT_EQ(recovered.status, motrack::PairedSaveStatus::Saved);
            QFile native(partial.nativePath);
            ASSERT_TRUE(native.open(QIODevice::ReadOnly));
            const auto originalNative = native.readAll();
            native.close();
            ASSERT_TRUE(journal.open(QIODevice::WriteOnly));
            const auto restored = unreadableJournal ? QByteArray("corrupt journal") : originalJournal;
            ASSERT_EQ(journal.write(restored), restored.size());
            journal.close();
            if (!unreadableJournal)
            {
                ASSERT_TRUE(native.open(QIODevice::WriteOnly | QIODevice::Truncate));
                ASSERT_EQ(native.write("external change"), 15);
                native.close();
            }
            const auto failed = motrack::recoverPairedDocuments(partial.recoveryPath, nullptr, {}, partial.transactionId);
            ASSERT_EQ(failed.status, motrack::PairedSaveStatus::RecoveryRequired);
            ASSERT_GT(failed.observation, recovered.observation);
            EXPECT_EQ(failed.transactionId.isEmpty(), unreadableJournal);
            if (successFirst)
                EXPECT_EQ(session.completePairedRecovery(checkpoint, partial.recoveryPath, recovered), motrack::SaveOutcome::Recovered);
            EXPECT_EQ(session.completePairedRecovery(checkpoint, partial.recoveryPath, failed), motrack::SaveOutcome::Partial);
            if (!successFirst)
                EXPECT_EQ(session.completePairedRecovery(checkpoint, partial.recoveryPath, recovered),
                          unreadableJournal ? motrack::SaveOutcome::Recovered : motrack::SaveOutcome::Stale);
            EXPECT_EQ(session.recoveryPath(), partial.recoveryPath);
            EXPECT_TRUE(QFileInfo::exists(partial.recoveryPath));
            const auto other = motrack::writePairedDocuments(temporary.filePath("other.trk"), session.document, source);
            ASSERT_EQ(other.status, motrack::PairedSaveStatus::Saved);
            EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), other), motrack::SaveOutcome::Partial);
            EXPECT_TRUE(session.isModified());
            ASSERT_TRUE(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
            ASSERT_EQ(journal.write(originalJournal), originalJournal.size());
            journal.close();
            ASSERT_TRUE(native.open(QIODevice::WriteOnly | QIODevice::Truncate));
            ASSERT_EQ(native.write(originalNative), originalNative.size());
            native.close();
            const auto finished = motrack::recoverPairedDocuments(partial.recoveryPath);
            ASSERT_EQ(finished.status, motrack::PairedSaveStatus::Saved);
            EXPECT_EQ(session.completePairedRecovery(session.saveCheckpoint(), partial.recoveryPath, finished),
                      motrack::SaveOutcome::Recovered);
            EXPECT_TRUE(session.recoveryPath().isEmpty());
            const auto current = motrack::writePairedDocuments(other.standardPath, session.document, source);
            EXPECT_EQ(session.completePairedSave(session.saveCheckpoint(), current), motrack::SaveOutcome::Saved);
            EXPECT_FALSE(session.isModified());
        }
    }
}

} // namespace
