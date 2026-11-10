#include "archive_group_session.hpp"

#include "tracker_archive.hpp"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <optional>
#include <stdexcept>

namespace
{

QString fixturePath()
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/multitab-images.trz";
}

std::shared_ptr<const motrack::TrackerArchive> sourceArchive(const QString& path = {})
{
    return std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(path.isEmpty() ? fixturePath() : path));
}

motrack::WorkspaceDocument document(const std::shared_ptr<const motrack::TrackerArchive>& archive, const char* member)
{
    auto result = motrack::importTrackerDocument(archive->projectPath(member));
    archive->resolveMedia(result.project, member);
    result.archivePayload = archive;
    result.archiveMember = member;
    return result;
}

std::unique_ptr<motrack::DocumentSession> session(const std::shared_ptr<const motrack::TrackerArchive>& archive, const char* member)
{
    auto result = std::make_unique<motrack::DocumentSession>();
    result->replace(document(archive, member), fixturePath());
    return result;
}

motrack::ArchiveGroup group()
{
    const auto archive = sourceArchive();
    auto result = motrack::makeArchiveGroup(document(archive, "experiment.trk"));
    (void)motrack::appendArchiveGroupDocument(result, document(archive, "second.trk"));
    return result;
}

void setMass(motrack::ArchiveGroupSession& live, QUuid id, double mass)
{
    if (!live.history(id).setPointMassValue(live.project(id), 0, mass))
        throw std::runtime_error("Could not edit group-session fixture.");
}

TEST(ArchiveGroupSession, PromotionPreservesHistoryIdentityAndStableRecords)
{
    const auto archive = sourceArchive();
    auto first = session(archive, "experiment.trk");
    ASSERT_TRUE(first->history.setPointMassValue(first->document.project, 0, 5));
    const auto identity = first->saveCheckpoint().identity;
    const auto* address = &first->document;
    motrack::ArchiveGroupSession live;
    const auto id = live.promote(first);
    EXPECT_FALSE(first);
    EXPECT_EQ(&live.document(id), address);
    EXPECT_EQ(live.saveCheckpoint().members.front().session.identity, identity);
    EXPECT_TRUE(live.isModified());
    EXPECT_TRUE(live.nativeSavePath().isEmpty());
    EXPECT_EQ(live.sourcePath(), QFileInfo(fixturePath()).absoluteFilePath());
    EXPECT_EQ(live.standardSavePath(), QFileInfo(fixturePath()).absoluteFilePath());
    ASSERT_TRUE(live.history(id).undo(live.project(id)));
    EXPECT_TRUE(live.isModified()); // A group has not yet been persisted, even at the old history head.
    ASSERT_TRUE(live.history(id).redo(live.project(id)));
    auto second = session(archive, "second.trk");
    const auto secondId = live.append(second);
    EXPECT_FALSE(second);
    EXPECT_EQ(&live.document(id), address);
    EXPECT_NE(id, secondId);
    EXPECT_FALSE(live.history(secondId).canUndo());
    EXPECT_DOUBLE_EQ(live.document(id).project.pointMasses.front().mass, 5);
}

TEST(ArchiveGroupSession, RetainsEverySavedExperimentWhileEditingAnotherAndReopening)
{
    for (const bool reverse : {false, true})
    {
        SCOPED_TRACE(reverse);
        QTemporaryDir temporary;
        const auto input = temporary.filePath("input.trz");
        ASSERT_TRUE(QFile::copy(fixturePath(), input));
        const auto target = temporary.filePath(QString::fromUtf8("expériences.trz"));
        QUuid groupId;
        std::vector<QUuid> ids;
        QString ownedSource;
        {
            const auto archive = sourceArchive(input);
            auto first = session(archive, reverse ? "second.trk" : "experiment.trk");
            auto second = session(archive, reverse ? "experiment.trk" : "second.trk");
            motrack::ArchiveGroupSession live;
            const auto a = live.promote(first);
            const auto b = live.append(second);
            ownedSource = archive->sourcePath();
            ASSERT_TRUE(QFile::remove(input));
            setMass(live, a, 5);
            {
                const auto request = live.prepareSave();
                const auto saved = motrack::writePairedArchiveGroup(target, request.snapshot);
                ASSERT_EQ(live.completePairedSave(request.checkpoint, saved), motrack::SaveOutcome::Saved);
            }
            EXPECT_FALSE(live.isModified());
            // No view needs to retain A. Its group record still participates in the later complete snapshot.
            setMass(live, b, 7);
            const auto request = live.prepareSave();
            const auto saved = motrack::writePairedArchiveGroup(target, request.snapshot);
            ASSERT_EQ(live.completePairedSave(request.checkpoint, saved), motrack::SaveOutcome::Saved);
            groupId = request.snapshot.id;
            ids = live.memberIds();
            EXPECT_FALSE(live.isModified());
            ASSERT_TRUE(live.history(b).undo(live.project(b)));
            EXPECT_TRUE(live.isModified());
            ASSERT_TRUE(live.history(b).redo(live.project(b)));
            EXPECT_FALSE(live.isModified());
            EXPECT_EQ(live.nativeSavePath(), saved.nativePath);
            EXPECT_EQ(live.standardSavePath(), saved.standardPath);
        }
        EXPECT_FALSE(QFileInfo::exists(ownedSource));
        const auto native = temporary.filePath(QString::fromUtf8("expériences.motrack"));
        motrack::ArchiveGroupSession reopened;
        reopened.replace(motrack::readArchiveGroup(native), native);
        EXPECT_FALSE(reopened.isModified());
        EXPECT_EQ(reopened.memberIds(), ids);
        EXPECT_EQ(reopened.prepareSave().snapshot.id, groupId);
        EXPECT_DOUBLE_EQ(reopened.document(ids[0]).project.pointMasses.front().mass, 5);
        EXPECT_DOUBLE_EQ(reopened.document(ids[1]).project.pointMasses.front().mass, 7);
        const auto standard = motrack::TrackerArchive::open(target);
        EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(standard.projectPath(reverse ? "second.trk" : "experiment.trk"))
                             .project.pointMasses.front()
                             .mass,
                         5);
        EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(standard.projectPath(reverse ? "experiment.trk" : "second.trk"))
                             .project.pointMasses.front()
                             .mass,
                         7);
    }
}

TEST(ArchiveGroupSession, StandaloneNativePromotionRequiresANewGroupTarget)
{
    QTemporaryDir temporary;
    const auto oldNative = temporary.filePath("standalone.motrack");
    auto incoming = std::make_unique<motrack::DocumentSession>();
    incoming->replace(document(sourceArchive(), "experiment.trk"), oldNative);
    const auto oldMass = incoming->document.project.pointMasses.front().mass;
    motrack::writeWorkspaceDocument(oldNative, incoming->document);
    ASSERT_TRUE(incoming->history.setPointMassValue(incoming->document.project, 0, 6));
    motrack::ArchiveGroupSession live;
    const auto id = live.promote(incoming);
    EXPECT_EQ(live.sourcePath(), oldNative);
    EXPECT_TRUE(live.nativeSavePath().isEmpty());
    EXPECT_TRUE(live.standardSavePath().isEmpty());
    EXPECT_TRUE(live.isModified());
    const auto newNative = temporary.filePath("group.motrack");
    const auto request = live.prepareSave();
    const auto saved = motrack::writeNativeArchiveGroup(newNative, request.snapshot);
    EXPECT_EQ(live.completeSave(request.checkpoint, newNative, saved), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(live.isModified());
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(oldNative).project.pointMasses.front().mass, oldMass);
    EXPECT_DOUBLE_EQ(motrack::readArchiveGroup(newNative).members.front().document.project.pointMasses.front().mass, 6);
    EXPECT_TRUE(live.history(id).canUndo());
}

TEST(ArchiveGroupSession, NewMembershipCannotBeClearedByAnEarlierSave)
{
    QTemporaryDir temporary;
    const auto archive = sourceArchive();
    const auto native = temporary.filePath("group.motrack");
    motrack::ArchiveGroupSession live;
    live.replace(motrack::makeArchiveGroup(document(archive, "experiment.trk")), native);
    const auto firstId = live.memberIds().front();
    const auto* address = &live.document(firstId);
    const auto earlier = live.prepareSave();
    auto second = session(archive, "second.trk");
    const auto secondId = live.append(second);
    EXPECT_FALSE(live.history(secondId).isModified());
    EXPECT_TRUE(live.isModified());
    const auto oldSave = motrack::writeNativeArchiveGroup(native, earlier.snapshot);
    EXPECT_EQ(live.completeSave(earlier.checkpoint, native, oldSave), motrack::SaveOutcome::Stale);
    EXPECT_TRUE(live.isModified());
    EXPECT_EQ(&live.document(firstId), address);
    const auto fresh = live.prepareSave();
    const auto saved = motrack::writeNativeArchiveGroup(native, fresh.snapshot);
    EXPECT_EQ(live.completeSave(fresh.checkpoint, native, saved), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(live.isModified());
    EXPECT_EQ(motrack::readArchiveGroup(native).members.size(), 2u);
}

TEST(ArchiveGroupSession, LateEditsAndReorderedWritesCannotClearAStaleDiskSnapshot)
{
    for (const bool paired : {false, true})
        for (const bool oldWrittenLast : {false, true})
            for (const bool oldCallbackLast : {false, true})
            {
                SCOPED_TRACE(paired);
                SCOPED_TRACE(oldWrittenLast);
                SCOPED_TRACE(oldCallbackLast);
                QTemporaryDir temporary;
                const auto native = temporary.filePath("group.motrack");
                const auto standard = temporary.filePath("group.trz");
                motrack::ArchiveGroupSession live;
                live.replace(group(), native);
                const auto id = live.memberIds().front();
                setMass(live, id, 5);
                const auto old = live.prepareSave();
                setMass(live, id, 7);
                const auto current = live.prepareSave();
                std::array<motrack::NativeSaveResult, 2> natives;
                std::array<motrack::PairedSaveResult, 2> pairs;
                const std::array requests{&old, &current};
                const std::array order = oldWrittenLast ? std::array{1, 0} : std::array{0, 1};
                for (const auto i : order)
                {
                    if (paired)
                        pairs[i] = motrack::writePairedArchiveGroup(standard, requests[i]->snapshot);
                    else
                        natives[i] = motrack::writeNativeArchiveGroup(native, requests[i]->snapshot);
                }
                const auto complete = [&](int i)
                {
                    return paired ? live.completePairedSave(requests[i]->checkpoint, pairs[i])
                                  : live.completeSave(requests[i]->checkpoint, native, natives[i]);
                };
                (void)complete(oldCallbackLast ? 1 : 0);
                (void)complete(oldCallbackLast ? 0 : 1);
                EXPECT_EQ(live.isModified(), oldWrittenLast);
                EXPECT_DOUBLE_EQ(motrack::readArchiveGroup(native).members.front().document.project.pointMasses.front().mass,
                                 oldWrittenLast ? 5 : 7);
                if (oldWrittenLast)
                {
                    ASSERT_TRUE(live.history(id).undo(live.project(id)));
                    EXPECT_TRUE(live.isModified());
                    ASSERT_TRUE(live.history(id).redo(live.project(id)));
                }
                const auto fresh = live.prepareSave();
                const auto saved = motrack::writePairedArchiveGroup(standard, fresh.snapshot);
                EXPECT_EQ(live.completePairedSave(fresh.checkpoint, saved), motrack::SaveOutcome::Saved);
                EXPECT_FALSE(live.isModified());
            }
}

TEST(ArchiveGroupSession, NativeOnlySavingRetainsOutstandingTrackerEditsAndMemberMediaBindings)
{
    QTemporaryDir temporary;
    auto original = group();
    const auto id = original.members.front().id;
    original.members.front().document.mediaLocations.insert("retained-reference", "retained-location");
    motrack::ArchiveGroupSession live;
    live.replace(std::move(original), temporary.filePath("before.motrack"));
    const auto standard = temporary.filePath("after.trz");
    const auto first = live.prepareSave();
    const auto paired = motrack::writePairedArchiveGroup(standard, first.snapshot);
    ASSERT_EQ(live.completePairedSave(first.checkpoint, paired), motrack::SaveOutcome::Saved);
    ASSERT_TRUE(live.requiresPairedSave());
    setMass(live, id, 7);
    const auto second = live.prepareSave();
    const auto nativeOnly = temporary.filePath("native-only.motrack");
    const auto saved = motrack::writeNativeArchiveGroup(nativeOnly, second.snapshot);
    EXPECT_EQ(live.completeSave(second.checkpoint, nativeOnly, saved), motrack::SaveOutcome::Saved);
    EXPECT_EQ(live.nativeSavePath(), nativeOnly);
    EXPECT_FALSE(live.history(id).isModified());
    EXPECT_TRUE(live.history(id).isModified(motrack::SaveTarget::Tracker));
    EXPECT_TRUE(live.isModified());
    EXPECT_EQ(live.document(id).mediaLocations.value("retained-reference"), "retained-location");
    EXPECT_EQ(live.completeSave(live.saveCheckpoint(), temporary.filePath("failed.motrack"), {}), motrack::SaveOutcome::Failed);
    EXPECT_EQ(live.nativeSavePath(), nativeOnly);
    EXPECT_EQ(live.completePairedSave(live.saveCheckpoint(), {.status = motrack::PairedSaveStatus::Cancelled}),
              motrack::SaveOutcome::Failed);
    EXPECT_EQ(live.standardSavePath(), standard);
}

TEST(ArchiveGroupSession, RecoveryNeverMarksCurrentMembersCleanOrRevivesCompletedRecords)
{
    constexpr std::array BOUNDARIES{motrack::PairedSaveProgress::RecoveryRecorded,
                                    motrack::PairedSaveProgress::StandardSaved,
                                    motrack::PairedSaveProgress::NativeSaved};
    for (const auto boundary : BOUNDARIES)
    {
        SCOPED_TRACE(static_cast<int>(boundary));
        QTemporaryDir temporary;
        const auto native = temporary.filePath("saved.motrack");
        const auto standard = temporary.filePath("saved.trz");
        motrack::ArchiveGroupSession live;
        live.replace(group(), native);
        const auto id = live.memberIds().back();
        const auto old = live.prepareSave();
        std::atomic_bool cancelled = false;
        const auto partial = motrack::writePairedArchiveGroup(standard,
                                                              old.snapshot,
                                                              &cancelled,
                                                              [&](auto progress)
                                                              {
                                                                  if (progress == boundary)
                                                                      cancelled = true;
                                                              });
        ASSERT_EQ(live.completePairedSave(old.checkpoint, partial), motrack::SaveOutcome::Partial);
        EXPECT_FALSE(live.recoveryPath().isEmpty());
        EXPECT_TRUE(live.isModified());
        setMass(live, id, 9);
        const auto beforeRecovery = live.prepareSave();
        const auto checkpoint = live.saveCheckpoint();
        const auto recovered = motrack::recoverPairedDocuments(live.recoveryPath(), nullptr, {}, partial.transactionId);
        ASSERT_EQ(recovered.status, motrack::PairedSaveStatus::Saved);
        EXPECT_EQ(live.completePairedRecovery(checkpoint, live.recoveryPath(), recovered), motrack::SaveOutcome::Recovered);
        EXPECT_TRUE(live.recoveryPath().isEmpty());
        EXPECT_TRUE(live.isModified());
        EXPECT_EQ(live.completePairedSave(old.checkpoint, partial), motrack::SaveOutcome::Stale);
        EXPECT_TRUE(live.recoveryPath().isEmpty());
        const auto delayed = motrack::writePairedArchiveGroup(standard, beforeRecovery.snapshot);
        EXPECT_EQ(live.completePairedSave(beforeRecovery.checkpoint, delayed), motrack::SaveOutcome::Stale);
        EXPECT_TRUE(live.isModified());
        const auto fresh = live.prepareSave();
        const auto saved = motrack::writePairedArchiveGroup(standard, fresh.snapshot);
        EXPECT_EQ(live.completePairedSave(fresh.checkpoint, saved), motrack::SaveOutcome::Saved);
        EXPECT_FALSE(live.isModified());
        EXPECT_DOUBLE_EQ(motrack::readArchiveGroup(native).members.back().document.project.pointMasses.front().mass, 9);
    }
}

TEST(ArchiveGroupSession, CopiedPersistentIdsAndReplacedMembersHaveIndependentRuntimeCheckpoints)
{
    QTemporaryDir temporary;
    const auto original = group();
    motrack::ArchiveGroupSession first;
    motrack::ArchiveGroupSession second;
    const auto native = temporary.filePath("first.motrack");
    const auto other = temporary.filePath("second.motrack");
    first.replace(original, native);
    second.replace(original, other);
    const auto old = first.prepareSave();
    const auto saved = motrack::writeNativeArchiveGroup(native, old.snapshot);
    EXPECT_EQ(second.completeSave(old.checkpoint, native, saved), motrack::SaveOutcome::Replaced);
    EXPECT_EQ(second.nativeSavePath(), other);
    EXPECT_FALSE(second.isModified());
    const auto id = original.members.front().id;
    auto replacement = original.members.front().document;
    replacement.project.pointMasses.front().mass = 11;
    first.replaceMember(id, std::move(replacement));
    EXPECT_FALSE(first.history(id).isModified());
    EXPECT_TRUE(first.isModified());
    EXPECT_NE(first.saveCheckpoint().members.front().session.identity, old.checkpoint.members.front().session.identity);
    EXPECT_EQ(first.completeSave(old.checkpoint, native, saved), motrack::SaveOutcome::Stale);
    EXPECT_TRUE(first.isModified());
    const auto fresh = first.prepareSave();
    EXPECT_EQ(first.completeSave(fresh.checkpoint, native, motrack::writeNativeArchiveGroup(native, fresh.snapshot)),
              motrack::SaveOutcome::Saved);
    EXPECT_FALSE(first.isModified());
    first.replace(original, other);
    EXPECT_EQ(first.completeSave(fresh.checkpoint, native, saved), motrack::SaveOutcome::Replaced);
    EXPECT_EQ(first.nativeSavePath(), other);
}

TEST(ArchiveGroupSession, FailedTransfersAndInvalidReplacementPreserveExistingOwnersAndEdits)
{
    QTemporaryDir temporary;
    motrack::ArchiveGroupSession live;
    const auto archive = sourceArchive();
    auto first = session(archive, "experiment.trk");
    const auto id = live.promote(first);
    setMass(live, id, 5);
    const auto before = live.saveCheckpoint();
    std::unique_ptr<motrack::DocumentSession> empty;
    EXPECT_THROW((void)live.promote(empty), motrack::ProjectError);
    EXPECT_THROW((void)live.append(empty), motrack::ProjectError);
    auto duplicate = session(archive, "experiment.trk");
    EXPECT_THROW((void)live.append(duplicate), motrack::ProjectError);
    EXPECT_TRUE(duplicate);
    auto foreign = session(sourceArchive(), "second.trk");
    ASSERT_TRUE(foreign->history.setPointMassValue(foreign->document.project, 0, 7));
    const auto foreignIdentity = foreign->saveCheckpoint().identity;
    EXPECT_THROW((void)live.append(foreign), motrack::ProjectError);
    ASSERT_TRUE(foreign);
    EXPECT_EQ(foreign->saveCheckpoint().identity, foreignIdentity);
    EXPECT_TRUE(foreign->history.isModified());
    EXPECT_THROW(live.replaceMember(id, foreign->document), motrack::ProjectError);
    auto invalid = group();
    invalid.members[1].id = invalid.members[0].id;
    EXPECT_THROW(live.replace(invalid, temporary.filePath("bad.motrack")), motrack::ProjectError);
    EXPECT_THROW(live.replace(group(), temporary.filePath("bad.trz")), motrack::ProjectError);
    EXPECT_EQ(live.saveCheckpoint().group.identity, before.group.identity);
    EXPECT_EQ(live.memberIds().size(), 1u);
    EXPECT_DOUBLE_EQ(live.document(id).project.pointMasses.front().mass, 5);
    EXPECT_THROW((void)live.document(QUuid{}), motrack::ProjectError);
    EXPECT_THROW((void)live.project(QUuid{}), motrack::ProjectError);
    EXPECT_THROW((void)live.history(QUuid{}), motrack::ProjectError);
    auto second = session(archive, "second.trk");
    (void)live.append(second);
    EXPECT_THROW((void)live.append(duplicate), motrack::ProjectError);
    EXPECT_TRUE(duplicate);
}

TEST(ArchiveGroupSession, PendingStandaloneRecoveryMustBeResolvedBeforeTransfer)
{
    QTemporaryDir temporary;
    auto incoming = session(sourceArchive(), "experiment.trk");
    const auto checkpoint = incoming->saveCheckpoint();
    std::atomic_bool cancelled = false;
    const auto partial = motrack::writePairedDocuments(temporary.filePath("standalone.trz"),
                                                       incoming->document,
                                                       incoming->sourcePath(),
                                                       &cancelled,
                                                       [&](auto progress)
                                                       {
                                                           if (progress == motrack::PairedSaveProgress::StandardSaved)
                                                               cancelled = true;
                                                       });
    ASSERT_EQ(incoming->completePairedSave(checkpoint, partial), motrack::SaveOutcome::Partial);
    motrack::ArchiveGroupSession live;
    EXPECT_THROW((void)live.promote(incoming), motrack::ProjectError);
    ASSERT_TRUE(incoming);
    EXPECT_FALSE(incoming->recoveryPath().isEmpty());
    const auto recovered = motrack::recoverPairedDocuments(incoming->recoveryPath(), nullptr, {}, partial.transactionId);
    ASSERT_EQ(incoming->completePairedRecovery(incoming->saveCheckpoint(), incoming->recoveryPath(), recovered),
              motrack::SaveOutcome::Recovered);
    const auto native = temporary.filePath("standalone.motrack");
    const auto saved = motrack::writeNativeDocument(native, incoming->document);
    ASSERT_EQ(incoming->completeSave(incoming->saveCheckpoint(), native, saved), motrack::SaveOutcome::Saved);
    const auto id = live.promote(incoming);
    EXPECT_TRUE(live.requiresPairedSave());
    EXPECT_TRUE(live.isModified());
    EXPECT_TRUE(live.history(id).isModified(motrack::SaveTarget::Tracker));
}

TEST(ArchiveGroupSession, AppendingAMemberRetainsItsOutstandingTrackerSaveObligation)
{
    QTemporaryDir temporary;
    const auto archive = sourceArchive();
    motrack::ArchiveGroupSession live;
    live.replace(motrack::makeArchiveGroup(document(archive, "experiment.trk")), temporary.filePath("before.motrack"));
    auto second = session(archive, "second.trk");
    const auto pair = motrack::writePairedDocuments(temporary.filePath("member.trz"), second->document, second->sourcePath());
    ASSERT_EQ(second->completePairedSave(second->saveCheckpoint(), pair), motrack::SaveOutcome::Saved);
    ASSERT_TRUE(second->history.setPointMassValue(second->document.project, 0, 7));
    const auto memberNative = temporary.filePath("member-only.motrack");
    const auto savedMember = motrack::writeNativeDocument(memberNative, second->document);
    ASSERT_EQ(second->completeSave(second->saveCheckpoint(), memberNative, savedMember), motrack::SaveOutcome::Saved);
    ASSERT_TRUE(second->isModified());
    const auto id = live.append(second);
    EXPECT_TRUE(live.requiresPairedSave());
    const auto request = live.prepareSave();
    const auto native = temporary.filePath("group.motrack");
    const auto saved = motrack::writeNativeArchiveGroup(native, request.snapshot);
    EXPECT_EQ(live.completeSave(request.checkpoint, native, saved), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(live.history(id).isModified());
    EXPECT_TRUE(live.history(id).isModified(motrack::SaveTarget::Tracker));
    EXPECT_TRUE(live.isModified());
    const auto fresh = live.prepareSave();
    const auto complete = motrack::writePairedArchiveGroup(temporary.filePath("group.trz"), fresh.snapshot);
    EXPECT_EQ(live.completePairedSave(fresh.checkpoint, complete), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(live.isModified());
}

TEST(ArchiveGroupSession, FrozenWorkerSnapshotsOutliveGroupReplacement)
{
    QTemporaryDir temporary;
    motrack::ArchiveGroupSession live;
    EXPECT_FALSE(live.isModified());
    EXPECT_TRUE(live.memberIds().empty());
    EXPECT_THROW((void)live.prepareSave(), motrack::ProjectError);
    const auto empty = live.saveCheckpoint();
    EXPECT_EQ(live.completeSave(empty, "unused.motrack", {}), motrack::SaveOutcome::Replaced);
    EXPECT_EQ(live.completePairedSave(empty, {}), motrack::SaveOutcome::Replaced);
    EXPECT_EQ(live.completePairedRecovery(empty, "unused", {}), motrack::SaveOutcome::Replaced);
    live.replace(group(), temporary.filePath("source.motrack"));
    const auto id = live.memberIds().front();
    setMass(live, id, 5);
    std::optional<motrack::ArchiveGroupSession::SaveRequest> request = live.prepareSave();
    const auto ownedSource = request->snapshot.archive->sourcePath();
    live.replace(group(), temporary.filePath("other.motrack"));
    EXPECT_TRUE(QFileInfo::exists(ownedSource));
    const auto target = temporary.filePath("snapshot.motrack");
    const auto saved = motrack::writeNativeArchiveGroup(target, request->snapshot);
    EXPECT_EQ(live.completeSave(request->checkpoint, target, saved), motrack::SaveOutcome::Replaced);
    request.reset();
    EXPECT_FALSE(QFileInfo::exists(ownedSource));
    EXPECT_DOUBLE_EQ(motrack::readArchiveGroup(target).members.front().document.project.pointMasses.front().mass, 5);
}

} // namespace
