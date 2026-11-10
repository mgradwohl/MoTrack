#include "workspace_session.hpp"

#include "tracker_archive.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <utility>

namespace
{

QString fixturePath(const char* name = "linked-images.trk")
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/" + name;
}

std::unique_ptr<motrack::DocumentSession> standalone()
{
    auto result = std::make_unique<motrack::DocumentSession>();
    auto document = motrack::importTrackerDocument(fixturePath());
    motrack::retainWorkspaceMediaLocations(document, fixturePath());
    result->replace(std::move(document), fixturePath());
    return result;
}

motrack::WorkspaceDocument memberDocument(const std::shared_ptr<const motrack::TrackerArchive>& archive, const char* member)
{
    auto document = motrack::importTrackerDocument(archive->projectPath(member));
    archive->resolveMedia(document.project, member);
    document.archivePayload = archive;
    document.archiveMember = member;
    return document;
}

std::unique_ptr<motrack::ArchiveGroupSession> archiveSession(bool allMembers = true)
{
    const auto archive = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(fixturePath("multitab-images.trz")));
    auto prepared = motrack::makeArchiveGroup(memberDocument(archive, "experiment.trk"));
    if (allMembers)
        (void)motrack::appendArchiveGroupDocument(prepared, memberDocument(archive, "second.trk"));
    auto result = std::make_unique<motrack::ArchiveGroupSession>();
    result->replace(std::move(prepared), fixturePath("multitab-images.trz"), motrack::ArchiveGroupSource::Tracker);
    return result;
}

motrack::WorkspaceSession::OpenedSource addStandalone(motrack::WorkspaceSession& workspace,
                                                      motrack::WorkspaceActivation activation = motrack::WorkspaceActivation::SelectNew)
{
    auto incoming = standalone();
    return workspace.addDocument(incoming, fixturePath(), activation);
}

void setMass(motrack::WorkspaceSession& workspace, QUuid id, double mass)
{
    if (!workspace.history(id).setPointMassValue(workspace.project(id), 0, mass))
        throw std::runtime_error("Could not edit workspace fixture.");
}

TEST(WorkspaceSession, SelectionOrderAndHiddenRecordsPreserveIndependentHistories)
{
    motrack::WorkspaceSession workspace;
    EXPECT_TRUE(workspace.tabs().empty());
    EXPECT_FALSE(workspace.activeDocument());
    auto incoming = standalone();
    ASSERT_TRUE(incoming->history.setPointMassValue(incoming->document.project, 0, 5));
    const auto* address = &incoming->document;
    const auto a = workspace.addDocument(incoming, fixturePath(), motrack::WorkspaceActivation::KeepCurrent);
    EXPECT_FALSE(incoming);
    const auto first = a.documents.front();
    EXPECT_EQ(workspace.activeDocument(), first);
    const auto b = addStandalone(workspace, motrack::WorkspaceActivation::KeepCurrent);
    const auto second = b.documents.front();
    EXPECT_EQ(workspace.activeDocument(), first);
    const auto c = addStandalone(workspace);
    const auto third = c.documents.front();
    EXPECT_EQ(workspace.activeDocument(), third);
    EXPECT_EQ(&workspace.document(first), address);
    EXPECT_NE(a.source, b.source); // Identical file paths do not alias independently opened records.
    EXPECT_NE(first, second);
    EXPECT_EQ(workspace.sourceId(first), a.source);
    EXPECT_TRUE(workspace.isModified(a.source));
    EXPECT_FALSE(workspace.isModified(b.source));
    setMass(workspace, second, 7);
    ASSERT_TRUE(workspace.history(first).undo(workspace.project(first)));
    EXPECT_FALSE(workspace.isModified(a.source));
    EXPECT_TRUE(workspace.isModified(b.source));
    ASSERT_TRUE(workspace.history(first).redo(workspace.project(first)));
    EXPECT_DOUBLE_EQ(workspace.document(first).project.pointMasses.front().mass, 5);
    EXPECT_DOUBLE_EQ(workspace.document(second).project.pointMasses.front().mass, 7);
    workspace.moveDocument(third, 0);
    EXPECT_EQ(workspace.tabs(), (std::vector<QUuid>{third, first, second}));
    workspace.moveDocument(third, 2);
    workspace.moveDocument(third, 2);
    EXPECT_EQ(workspace.tabs(), (std::vector<QUuid>{first, second, third}));
    workspace.showDocument(first);
    workspace.hideDocument(second);
    EXPECT_EQ(workspace.activeDocument(), first);
    workspace.hideDocument(first);
    EXPECT_EQ(workspace.activeDocument(), third);
    workspace.hideDocument(first);
    workspace.showDocument(first);
    EXPECT_EQ(&workspace.document(first), address);
    EXPECT_TRUE(workspace.history(first).canUndo());
    workspace.hideDocument(first); // Closing the rightmost tab selects its previous neighbor.
    EXPECT_EQ(workspace.activeDocument(), third);
    workspace.hideDocument(third);
    EXPECT_FALSE(workspace.activeDocument());
    EXPECT_TRUE(workspace.tabs().empty());
    EXPECT_FALSE(workspace.releaseSource(a.source));
    EXPECT_TRUE(workspace.releaseSource(a.source, motrack::WorkspaceDiscard::Confirmed));
    EXPECT_TRUE(workspace.releaseSource(c.source));
    EXPECT_TRUE(workspace.isModified(b.source));
    workspace.showDocument(second);
    EXPECT_EQ(workspace.activeDocument(), second);
    EXPECT_FALSE(workspace.releaseSource(b.source, motrack::WorkspaceDiscard::Confirmed));
}

TEST(WorkspaceSession, JobsFollowDocumentIdentityThroughSelectionAndGroupViewChanges)
{
    motrack::WorkspaceSession workspace;
    auto group = archiveSession();
    const auto opened = workspace.addGroup(group, motrack::WorkspaceActivation::KeepCurrent);
    const auto a = opened.documents[0];
    const auto b = opened.documents[1];
    auto otherGroup = archiveSession();
    const auto other = workspace.addGroup(otherGroup);
    const auto c = other.documents[0];
    const auto track = workspace.beginJob(a, motrack::WorkspaceJob::AutoTrack);
    const auto oldFrame = workspace.beginJob(a, motrack::WorkspaceJob::Frame);
    const auto index = workspace.beginJob(a, motrack::WorkspaceJob::MediaIndex);
    const auto newerFrame = workspace.beginJob(a, motrack::WorkspaceJob::Frame);
    EXPECT_TRUE(oldFrame.cancelled->load());
    EXPECT_FALSE(workspace.finishJob(oldFrame));
    EXPECT_TRUE(workspace.accepts(newerFrame));
    const auto otherTrack = workspace.beginJob(b, motrack::WorkspaceJob::AutoTrack);
    const auto unrelated = workspace.beginJob(c, motrack::WorkspaceJob::AutoTrack);
    workspace.showDocument(b);
    workspace.moveDocument(a, 3);
    workspace.hideDocument(b);
    workspace.cancelJobs(b); // Closing B's view must not stop A in the same archive (#69).
    EXPECT_FALSE(workspace.accepts(otherTrack));
    EXPECT_TRUE(workspace.accepts(track));
    EXPECT_TRUE(workspace.accepts(index));
    workspace.hideDocument(a);
    EXPECT_TRUE(workspace.accepts(track)); // Retaining a hidden record is independent of cancellation policy.
    EXPECT_TRUE(workspace.finishJob(index));
    EXPECT_FALSE(workspace.finishJob(index));
    EXPECT_TRUE(workspace.accepts(track));
    auto wrongDocument = track;
    wrongDocument.document = c;
    EXPECT_FALSE(workspace.accepts(wrongDocument));
    EXPECT_FALSE(workspace.accepts({}));
    workspace.cancelSourceWork(opened.source);
    EXPECT_FALSE(workspace.accepts(track));
    EXPECT_FALSE(workspace.accepts(newerFrame));
    EXPECT_TRUE(workspace.accepts(unrelated));
    EXPECT_TRUE(workspace.releaseSource(opened.source));
    EXPECT_FALSE(workspace.accepts(track));
    EXPECT_TRUE(workspace.accepts(unrelated));
}

TEST(WorkspaceSession, RefusedInputsAndUnknownIdsPreserveIncomingOwnersAndCurrentState)
{
    motrack::WorkspaceSession workspace;
    const auto existing = addStandalone(workspace);
    const auto id = existing.documents.front();
    const auto missing = QUuid::createUuid();
    auto incoming = standalone();
    const auto* address = incoming.get();
    EXPECT_THROW((void)workspace.addDocument(incoming, {}), motrack::ProjectError);
    EXPECT_EQ(incoming.get(), address);
    EXPECT_THROW((void)workspace.appendMember(existing.source, incoming), motrack::ProjectError);
    EXPECT_EQ(incoming.get(), address);
    std::unique_ptr<motrack::DocumentSession> empty;
    EXPECT_THROW((void)workspace.addDocument(empty, fixturePath()), motrack::ProjectError);
    auto uninitialized = std::make_unique<motrack::DocumentSession>();
    EXPECT_THROW((void)workspace.addDocument(uninitialized, fixturePath()), motrack::ProjectError);
    auto emptyGroup = std::make_unique<motrack::ArchiveGroupSession>();
    EXPECT_THROW((void)workspace.addGroup(emptyGroup), motrack::ProjectError);
    std::unique_ptr<motrack::ArchiveGroupSession> nullGroup;
    EXPECT_THROW((void)workspace.addGroup(nullGroup), motrack::ProjectError);
    EXPECT_THROW((void)workspace.document(missing), motrack::ProjectError);
    EXPECT_THROW((void)workspace.project(missing), motrack::ProjectError);
    EXPECT_THROW((void)workspace.history(missing), motrack::ProjectError);
    EXPECT_THROW((void)workspace.isModified(missing), motrack::ProjectError);
    EXPECT_THROW(workspace.showDocument(missing), motrack::ProjectError);
    EXPECT_THROW(workspace.hideDocument(missing), motrack::ProjectError);
    EXPECT_THROW(workspace.moveDocument(id, 1), motrack::ProjectError);
    EXPECT_THROW((void)workspace.releaseSource(missing), motrack::ProjectError);
    EXPECT_THROW((void)workspace.beginSave(missing), motrack::ProjectError);
    EXPECT_THROW((void)workspace.beginRecovery(missing), motrack::ProjectError);
    EXPECT_THROW(workspace.cancelJobs(missing), motrack::ProjectError);
    EXPECT_THROW(workspace.cancelSourceWork(missing), motrack::ProjectError);
    EXPECT_EQ(workspace.tabs(), existing.documents);
    EXPECT_EQ(workspace.activeDocument(), id);
    EXPECT_FALSE(workspace.isModified(existing.source));
}

TEST(WorkspaceSession, FreshTrackerGroupStartsCleanAndRejectsWrongSourceKinds)
{
    auto incoming = archiveSession();
    EXPECT_FALSE(incoming->isModified());
    EXPECT_TRUE(incoming->nativeSavePath().isEmpty());
    EXPECT_EQ(incoming->standardSavePath(), QFileInfo(fixturePath("multitab-images.trz")).absoluteFilePath());
    const auto prepared = incoming->prepareSave();
    const auto identity = prepared.checkpoint.group.identity;
    EXPECT_THROW(incoming->replace(prepared.snapshot, "wrong.motrack", motrack::ArchiveGroupSource::Tracker), motrack::ProjectError);
    EXPECT_THROW(incoming->replace(prepared.snapshot, "wrong.trz"), motrack::ProjectError);
    EXPECT_EQ(incoming->saveCheckpoint().group.identity, identity);
    motrack::WorkspaceSession workspace;
    const auto opened = workspace.addGroup(incoming);
    EXPECT_FALSE(incoming);
    EXPECT_FALSE(workspace.isModified(opened.source));
    EXPECT_FALSE(workspace.requiresPairedSave(opened.source)); // Paired mode starts with an actual paired save.
    EXPECT_TRUE(workspace.nativeSavePath(opened.source).isEmpty());
    EXPECT_TRUE(workspace.recoveryPath(opened.source).isEmpty());
    EXPECT_FALSE(workspace.beginRecovery(opened.documents[0]));
    setMass(workspace, opened.documents[1], 9);
    EXPECT_TRUE(workspace.isModified(opened.source));
    ASSERT_TRUE(workspace.history(opened.documents[1]).undo(workspace.project(opened.documents[1])));
    EXPECT_FALSE(workspace.isModified(opened.source));
}

TEST(WorkspaceSession, SavingHiddenArchiveMembersRetainsBothEditsAfterNativeReopen)
{
    for (const bool reverse : {false, true})
    {
        SCOPED_TRACE(reverse);
        QTemporaryDir temporary;
        motrack::WorkspaceSession workspace;
        auto group = archiveSession();
        const auto originalIds = group->memberIds();
        const auto opened = workspace.addGroup(group);
        const auto a = opened.documents[reverse ? 1 : 0];
        const auto b = opened.documents[reverse ? 0 : 1];
        const auto* stable = &workspace.document(a);
        const auto unrelated = addStandalone(workspace);
        const auto target = temporary.filePath(QString::fromUtf8("expériences.trz"));
        setMass(workspace, a, 5);
        const auto first = workspace.beginSave(a);
        if (!first.has_value())
            FAIL() << "Expected an available workspace operation.";
        EXPECT_EQ(first->operation.source, opened.source);
        ASSERT_EQ(workspace.completePairedSave(*first, motrack::writeWorkspacePair(*first, target)), motrack::SaveOutcome::Saved);
        EXPECT_FALSE(workspace.isModified(opened.source));
        workspace.hideDocument(a);
        setMass(workspace, b, 7);
        const auto second = workspace.beginSave(b);
        if (!second.has_value())
            FAIL() << "Expected an available workspace operation.";
        EXPECT_EQ(std::get<motrack::ArchiveGroupSession::SaveRequest>(second->snapshot).snapshot.members.size(), 2);
        ASSERT_EQ(workspace.completePairedSave(*second, motrack::writeWorkspacePair(*second, target)), motrack::SaveOutcome::Saved);
        EXPECT_EQ(&workspace.document(a), stable);
        EXPECT_FALSE(workspace.isModified(opened.source));
        EXPECT_FALSE(workspace.isModified(unrelated.source));
        ASSERT_TRUE(workspace.history(b).undo(workspace.project(b)));
        EXPECT_TRUE(workspace.isModified(opened.source));
        ASSERT_TRUE(workspace.history(b).redo(workspace.project(b)));
        EXPECT_FALSE(workspace.isModified(opened.source));
        const auto native = workspace.nativeSavePath(opened.source);
        EXPECT_EQ(workspace.standardSavePath(opened.source), target);
        workspace.hideDocument(b);
        ASSERT_TRUE(workspace.releaseSource(opened.source));
        EXPECT_EQ(workspace.completePairedSave(*second, {}), motrack::SaveOutcome::Replaced);
        auto reopened = std::make_unique<motrack::ArchiveGroupSession>();
        reopened->replace(motrack::readArchiveGroup(native), native);
        EXPECT_EQ(reopened->memberIds(), originalIds);
        const auto fresh = workspace.addGroup(reopened);
        EXPECT_NE(fresh.source, opened.source);
        EXPECT_NE(fresh.documents, opened.documents);
        EXPECT_DOUBLE_EQ(workspace.document(fresh.documents[reverse ? 1 : 0]).project.pointMasses.front().mass, 5);
        EXPECT_DOUBLE_EQ(workspace.document(fresh.documents[reverse ? 0 : 1]).project.pointMasses.front().mass, 7);
        EXPECT_FALSE(workspace.isModified(fresh.source));
    }
}

TEST(WorkspaceSession, AppendingMemberPreservesHistoryAndInvalidatesEarlierWholeGroupSave)
{
    QTemporaryDir temporary;
    motrack::WorkspaceSession workspace;
    auto group = archiveSession(false);
    const auto archive = group->document(group->memberIds().front()).archivePayload;
    const auto opened = workspace.addGroup(group);
    const auto first = opened.documents.front();
    const auto* stable = &workspace.document(first);
    const auto earlier = workspace.beginSave(first);
    if (!earlier.has_value())
        FAIL() << "Expected an available workspace operation.";
    auto other = std::make_unique<motrack::DocumentSession>();
    other->replace(memberDocument(archive, "second.trk"), fixturePath("multitab-images.trz"));
    ASSERT_TRUE(other->history.setPointMassValue(other->document.project, 0, 7));
    const auto second = workspace.appendMember(opened.source, other, motrack::WorkspaceActivation::KeepCurrent);
    EXPECT_FALSE(other);
    EXPECT_EQ(workspace.activeDocument(), first);
    EXPECT_EQ(&workspace.document(first), stable);
    EXPECT_EQ(workspace.sourceId(second), opened.source);
    EXPECT_TRUE(workspace.history(second).canUndo());
    auto duplicate = std::make_unique<motrack::DocumentSession>();
    duplicate->replace(memberDocument(archive, "second.trk"), fixturePath("multitab-images.trz"));
    const auto* rejected = duplicate.get();
    EXPECT_THROW((void)workspace.appendMember(opened.source, duplicate), motrack::ProjectError);
    EXPECT_EQ(duplicate.get(), rejected);
    EXPECT_EQ(workspace.tabs().size(), 2);
    const auto target = temporary.filePath("group.motrack");
    EXPECT_EQ(workspace.completeSave(*earlier, target, motrack::writeWorkspaceNative(*earlier, target)), motrack::SaveOutcome::Stale);
    EXPECT_TRUE(workspace.isModified(opened.source));
    const auto fresh = workspace.beginSave(second);
    if (!fresh.has_value())
        FAIL() << "Expected an available workspace operation.";
    ASSERT_EQ(workspace.completePairedSave(*fresh, motrack::writeWorkspacePair(*fresh, temporary.filePath("group.trz"))),
              motrack::SaveOutcome::Saved);
    EXPECT_FALSE(workspace.isModified(opened.source));
    EXPECT_DOUBLE_EQ(motrack::readArchiveGroup(target).members[1].document.project.pointMasses.front().mass, 7);
}

TEST(WorkspaceSession, StandaloneSaveAsRebasesMediaAndCompletesOnlyItsOwnHistory)
{
    QTemporaryDir temporary;
    motrack::WorkspaceSession workspace;
    const auto a = addStandalone(workspace);
    const auto b = addStandalone(workspace);
    setMass(workspace, a.documents[0], 5);
    setMass(workspace, b.documents[0], 7);
    EXPECT_FALSE(workspace.requiresPairedSave(a.source));
    EXPECT_TRUE(workspace.nativeSavePath(a.source).isEmpty());
    EXPECT_TRUE(workspace.recoveryPath(a.source).isEmpty());
    EXPECT_EQ(workspace.standardSavePath(a.source), QFileInfo(fixturePath()).absoluteFilePath());
    const auto nativePath = temporary.filePath("relocated.motrack");
    const auto native = workspace.beginSave(a.documents[0]);
    if (!native.has_value())
        FAIL() << "Expected an available workspace operation.";
    const auto originalReferences = workspace.document(a.documents[0]).project.clip.mediaReferences;
    const auto result = motrack::writeWorkspaceNative(*native, nativePath);
    EXPECT_EQ(workspace.completeSave(*native, nativePath, result), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(workspace.isModified(a.source)); // Explicit native-only mode, before any paired save.
    EXPECT_TRUE(workspace.isModified(b.source));
    EXPECT_EQ(workspace.document(a.documents[0]).project.clip.mediaReferences, originalReferences);
    const auto reopened = motrack::readWorkspaceDocument(nativePath);
    EXPECT_DOUBLE_EQ(reopened.project.pointMasses.front().mass, 5);
    ASSERT_EQ(reopened.project.clip.mediaReferences.size(), originalReferences.size());
    for (const auto& reference : reopened.project.clip.mediaReferences)
        EXPECT_TRUE(QFileInfo::exists(motrack::workspaceMediaPath(reopened, nativePath, reference)));
    const auto pair = workspace.beginSave(a.documents[0]);
    if (!pair.has_value())
        FAIL() << "Expected an available workspace operation.";
    const auto standardPath = temporary.filePath("relocated.trk");
    ASSERT_EQ(workspace.completePairedSave(*pair, motrack::writeWorkspacePair(*pair, standardPath)), motrack::SaveOutcome::Saved);
    EXPECT_FALSE(workspace.isModified(a.source));
    EXPECT_TRUE(workspace.isModified(b.source));
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(standardPath).project.pointMasses.front().mass, 5);
    EXPECT_EQ(workspace.nativeSavePath(a.source), nativePath);
    EXPECT_EQ(workspace.standardSavePath(a.source), standardPath);
    EXPECT_TRUE(workspace.requiresPairedSave(a.source));
    setMass(workspace, a.documents[0], 9);
    const auto nativeAfterPair = workspace.beginSave(a.documents[0]);
    if (!nativeAfterPair.has_value())
        FAIL() << "Expected an available workspace operation.";
    ASSERT_EQ(workspace.completeSave(*nativeAfterPair, nativePath, motrack::writeWorkspaceNative(*nativeAfterPair, nativePath)),
              motrack::SaveOutcome::Saved);
    EXPECT_TRUE(workspace.isModified(a.source)); // The established Tracker target still has mass 5.
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(standardPath).project.pointMasses.front().mass, 5);
    EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(nativePath).project.pointMasses.front().mass, 9);
}

TEST(WorkspaceSession, BoundsPendingSnapshotsAndKeepsCancelledWritesOwnedUntilCompletion)
{
    QTemporaryDir temporary;
    motrack::WorkspaceSession workspace;
    const auto a = addStandalone(workspace);
    const auto b = addStandalone(workspace);
    const auto first = workspace.beginSave(a.documents[0]);
    const auto second = workspace.beginSave(b.documents[0]);
    if (!first.has_value() || !second.has_value())
        FAIL() << "Expected an available workspace operation.";
    EXPECT_FALSE(workspace.beginSave(a.documents[0]));
    EXPECT_FALSE(workspace.beginRecovery(a.documents[0]));
    workspace.hideDocument(a.documents[0]);
    EXPECT_FALSE(workspace.releaseSource(a.source, motrack::WorkspaceDiscard::Confirmed));
    workspace.cancelSourceWork(a.source);
    EXPECT_TRUE(first->operation.cancelled->load());
    EXPECT_FALSE(second->operation.cancelled->load());
    EXPECT_FALSE(workspace.releaseSource(a.source, motrack::WorkspaceDiscard::Confirmed));
    EXPECT_THROW((void)motrack::writeWorkspaceNative(*first, temporary.filePath("cancelled.motrack")), std::exception);
    EXPECT_EQ(workspace.completeSave(*first, temporary.filePath("cancelled.motrack"), {}), motrack::SaveOutcome::Failed);
    EXPECT_FALSE(QFileInfo::exists(temporary.filePath("cancelled.motrack")));
    EXPECT_EQ(workspace.completeSave(*first, {}, {}), motrack::SaveOutcome::Replaced);
    EXPECT_TRUE(workspace.releaseSource(a.source));
    const auto resumed = workspace.beginSave(b.documents[0]);
    if (!resumed.has_value())
        FAIL() << "Expected an available workspace operation.";
    EXPECT_FALSE(workspace.beginSave(b.documents[0]));
    EXPECT_EQ(workspace.completePairedSave(*resumed, {}), motrack::SaveOutcome::Failed);
    EXPECT_EQ(workspace.completePairedSave(*second, {}), motrack::SaveOutcome::Failed);
    EXPECT_FALSE(workspace.isModified(b.source));
}

TEST(WorkspaceSession, ReorderedCompletionUsesPhysicalWriteOrderAndKeepsLateEditsDirty)
{
    for (const bool groupMode : {false, true})
    {
        SCOPED_TRACE(groupMode);
        QTemporaryDir temporary;
        motrack::WorkspaceSession workspace;
        auto group = archiveSession();
        const auto opened = groupMode ? workspace.addGroup(group) : addStandalone(workspace);
        const auto id = opened.documents.front();
        setMass(workspace, id, 5);
        const auto old = workspace.beginSave(id);
        setMass(workspace, id, 7);
        const auto newer = workspace.beginSave(id);
        if (!old.has_value() || !newer.has_value())
            FAIL() << "Expected an available workspace operation.";
        const auto target = temporary.filePath(groupMode ? "pair.trz" : "pair.trk");
        const auto newResult = motrack::writeWorkspacePair(*newer, target);
        const auto oldResult = motrack::writeWorkspacePair(*old, target);
        EXPECT_EQ(workspace.completePairedSave(*newer, newResult), motrack::SaveOutcome::Saved);
        EXPECT_EQ(workspace.completePairedSave(*old, oldResult), motrack::SaveOutcome::Stale);
        EXPECT_TRUE(workspace.isModified(opened.source));
        EXPECT_DOUBLE_EQ(workspace.document(id).project.pointMasses.front().mass, 7);
        const auto native = workspace.nativeSavePath(opened.source);
        const auto diskMass = groupMode ? motrack::readArchiveGroup(native).members.front().document.project.pointMasses.front().mass
                                        : motrack::readWorkspaceDocument(native).project.pointMasses.front().mass;
        EXPECT_DOUBLE_EQ(diskMass, 5);
        const auto final = workspace.beginSave(id);
        if (!final.has_value())
            FAIL() << "Expected an available workspace operation.";
        ASSERT_EQ(workspace.completePairedSave(*final, motrack::writeWorkspacePair(*final, target)), motrack::SaveOutcome::Saved);
        EXPECT_FALSE(workspace.isModified(opened.source));
    }
}

TEST(WorkspaceSession, CancellationAfterPhysicalCommitStillAppliesTheReturnedResult)
{
    for (const bool groupMode : {false, true})
        for (const bool paired : {false, true})
        {
            SCOPED_TRACE(groupMode);
            SCOPED_TRACE(paired);
            QTemporaryDir temporary;
            motrack::WorkspaceSession workspace;
            auto group = archiveSession();
            const auto opened = groupMode ? workspace.addGroup(group) : addStandalone(workspace);
            const auto id = opened.documents.front();
            setMass(workspace, id, 5);
            const auto request = workspace.beginSave(id);
            if (!request.has_value())
                FAIL() << "Expected an available workspace operation.";
            const auto target = temporary.filePath(paired ? (groupMode ? "committed.trz" : "committed.trk") : "committed.motrack");
            motrack::NativeSaveResult native;
            motrack::PairedSaveResult pair;
            if (paired)
                pair = motrack::writeWorkspacePair(*request, target);
            else
                native = motrack::writeWorkspaceNative(*request, target);
            workspace.cancelSourceWork(opened.source); // Owner sees cancellation after the worker committed.
            EXPECT_TRUE(request->operation.cancelled->load());
            for (const auto document : opened.documents)
                workspace.hideDocument(document);
            EXPECT_FALSE(workspace.releaseSource(opened.source, motrack::WorkspaceDiscard::Confirmed));
            const auto outcome = paired ? workspace.completePairedSave(*request, pair) : workspace.completeSave(*request, target, native);
            EXPECT_EQ(outcome, motrack::SaveOutcome::Saved);
            EXPECT_TRUE(QFileInfo::exists(target));
            EXPECT_FALSE(workspace.isModified(opened.source));
            EXPECT_TRUE(workspace.releaseSource(opened.source));
        }
}

TEST(WorkspaceSession, RecoveryRoutesWholeSourceAndNeverMarksCurrentEditsSaved)
{
    for (const bool groupMode : {false, true})
    {
        SCOPED_TRACE(groupMode);
        QTemporaryDir temporary;
        motrack::WorkspaceSession workspace;
        auto group = archiveSession();
        const auto opened = groupMode ? workspace.addGroup(group) : addStandalone(workspace);
        const auto id = opened.documents.front();
        const auto unrelated = addStandalone(workspace);
        setMass(workspace, id, 5);
        const auto save = workspace.beginSave(id);
        if (!save.has_value())
            FAIL() << "Expected an available workspace operation.";
        const auto target = temporary.filePath(groupMode ? "recover.trz" : "recover.trk");
        const auto partial = motrack::writeWorkspacePair(*save,
                                                         target,
                                                         [](auto progress)
                                                         {
                                                             if (progress == motrack::PairedSaveProgress::StandardSaved)
                                                                 throw std::runtime_error("Interrupted after standard commit.");
                                                         });
        ASSERT_EQ(workspace.completePairedSave(*save, partial), motrack::SaveOutcome::Partial);
        EXPECT_FALSE(workspace.recoveryPath(opened.source).isEmpty());
        setMass(workspace, id, 7);
        const auto cancelledRecovery = workspace.beginRecovery(id);
        if (!cancelledRecovery.has_value())
            FAIL() << "Expected an available workspace operation.";
        workspace.cancelSourceWork(opened.source);
        const auto cancelledResult = motrack::recoverWorkspacePair(*cancelledRecovery);
        EXPECT_EQ(cancelledResult.status, motrack::PairedSaveStatus::RecoveryRequired);
        EXPECT_EQ(workspace.completeRecovery(*cancelledRecovery, cancelledResult), motrack::SaveOutcome::Partial);
        EXPECT_FALSE(workspace.recoveryPath(opened.source).isEmpty());
        const auto recovery = workspace.beginRecovery(opened.documents.back());
        if (!recovery.has_value())
            FAIL() << "Expected an available workspace operation.";
        EXPECT_EQ(recovery->operation.source, opened.source);
        EXPECT_EQ(recovery->transaction, partial.transactionId);
        const auto other = workspace.beginSave(unrelated.documents.front());
        if (!other.has_value())
            FAIL() << "Expected an available workspace operation.";
        EXPECT_FALSE(workspace.beginRecovery(id));
        for (const auto document : opened.documents)
            workspace.hideDocument(document);
        EXPECT_FALSE(workspace.releaseSource(opened.source, motrack::WorkspaceDiscard::Confirmed));
        ASSERT_EQ(workspace.completeRecovery(*recovery, motrack::recoverWorkspacePair(*recovery)), motrack::SaveOutcome::Recovered);
        EXPECT_EQ(workspace.completeRecovery(*recovery, {}), motrack::SaveOutcome::Replaced);
        EXPECT_TRUE(workspace.recoveryPath(opened.source).isEmpty());
        EXPECT_TRUE(workspace.isModified(opened.source));
        EXPECT_FALSE(workspace.isModified(unrelated.source));
        EXPECT_EQ(workspace.completePairedSave(*other, {}), motrack::SaveOutcome::Failed);
        const auto fresh = workspace.beginSave(id);
        if (!fresh.has_value())
            FAIL() << "Expected an available workspace operation.";
        ASSERT_EQ(workspace.completePairedSave(*fresh, motrack::writeWorkspacePair(*fresh, target)), motrack::SaveOutcome::Saved);
        EXPECT_FALSE(workspace.isModified(opened.source));
        EXPECT_TRUE(workspace.releaseSource(opened.source));
    }
}

TEST(WorkspaceSession, DestructionCancelsTokensWhileWorkerSnapshotsRetainArchiveOwnership)
{
    QTemporaryDir temporary;
    std::optional<motrack::WorkspaceSaveRequest> request;
    motrack::WorkspaceJobToken token;
    std::weak_ptr<const motrack::TrackerArchive> archive;
    QString ownedPath;
    {
        motrack::WorkspaceSession workspace;
        auto group = archiveSession();
        const auto opened = workspace.addGroup(group);
        const auto id = opened.documents.front();
        archive = workspace.document(id).archivePayload;
        ownedPath = archive.lock()->sourcePath();
        token = workspace.beginJob(id, motrack::WorkspaceJob::AutoTrack);
        request = workspace.beginSave(id);
        if (!request.has_value())
            FAIL() << "Expected an available workspace operation.";
    }
    EXPECT_TRUE(token.cancelled->load());
    EXPECT_TRUE(request->operation.cancelled->load());
    EXPECT_FALSE(archive.expired());
    EXPECT_TRUE(QFileInfo::exists(ownedPath));
    const auto cancelled = motrack::writeWorkspacePair(*request, temporary.filePath("cancelled.trz"));
    EXPECT_EQ(cancelled.status, motrack::PairedSaveStatus::Cancelled);
    EXPECT_FALSE(QFileInfo::exists(temporary.filePath("cancelled.trz")));
    motrack::WorkspaceSession replacement;
    auto group = archiveSession();
    const auto opened = replacement.addGroup(group);
    EXPECT_FALSE(replacement.accepts(token));
    EXPECT_EQ(replacement.completePairedSave(*request, cancelled), motrack::SaveOutcome::Replaced);
    EXPECT_FALSE(replacement.isModified(opened.source));
    request.reset();
    EXPECT_TRUE(archive.expired());
    EXPECT_FALSE(QFileInfo::exists(ownedPath));
}

TEST(WorkspaceSession, RestoreSavedExperimentUsesActualStalePairAndKeepsOtherMembersAndJobs)
{
    for (const bool grouped : {false, true})
    {
        SCOPED_TRACE(grouped);
        QTemporaryDir temporary;
        motrack::WorkspaceSession workspace;
        auto group = archiveSession();
        const auto opened = grouped ? workspace.addGroup(group) : addStandalone(workspace);
        const auto id = opened.documents.front();
        const auto unrelated = addStandalone(workspace);
        const auto other = grouped ? opened.documents.back() : unrelated.documents.front();
        const auto target = temporary.filePath(grouped ? "saved.trz" : "saved.trk");
        setMass(workspace, id, 7);
        const auto request = workspace.beginSave(id);
        if (!request)
            FAIL() << "Expected a save request.";
        setMass(workspace, id, 9);
        setMass(workspace, other, 13);
        EXPECT_FALSE(workspace.canRestoreSavedDocument(id));
        const auto result = motrack::writeWorkspacePair(*request, target);
        ASSERT_EQ(result.status, motrack::PairedSaveStatus::Saved);
        EXPECT_EQ(workspace.completePairedSave(*request, result), motrack::SaveOutcome::Stale);
        EXPECT_TRUE(workspace.canRestoreSavedDocument(id));
        EXPECT_FALSE(workspace.restoreSavedDocument(id)); // The visible view must be detached first.
        const auto ownJob = workspace.beginJob(id, motrack::WorkspaceJob::AutoTrack);
        const auto otherJob = workspace.beginJob(other, motrack::WorkspaceJob::AutoTrack);
        const auto revision = workspace.history(other).revision();
        const auto identity = workspace.documentCheckpoint(id).identity;
        const auto archive = workspace.document(id).archivePayload;
        workspace.hideDocument(id);
        ASSERT_TRUE(workspace.restoreSavedDocument(id));
        EXPECT_DOUBLE_EQ(workspace.project(id).pointMasses.front().mass, 7);
        EXPECT_DOUBLE_EQ(workspace.project(other).pointMasses.front().mass, 13);
        EXPECT_EQ(workspace.documentCheckpoint(id).identity, identity);
        EXPECT_EQ(workspace.document(id).archivePayload, archive);
        EXPECT_EQ(workspace.history(other).revision(), revision);
        EXPECT_TRUE(workspace.history(other).canUndo());
        EXPECT_FALSE(workspace.accepts(ownJob));
        EXPECT_TRUE(workspace.accepts(otherJob));
        EXPECT_EQ(workspace.isModified(opened.source), grouped);
        const auto fresh = workspace.beginSave(id);
        if (!fresh)
            FAIL() << "Expected a save request.";
        ASSERT_EQ(workspace.completePairedSave(*fresh, motrack::writeWorkspacePair(*fresh, target)), motrack::SaveOutcome::Saved);
        const auto saved = grouped ? motrack::readArchiveGroup(temporary.filePath("saved.motrack")).members.front().document
                                   : motrack::readWorkspaceDocument(temporary.filePath("saved.motrack"));
        EXPECT_DOUBLE_EQ(saved.project.pointMasses.front().mass, 7);
        if (grouped)
            EXPECT_DOUBLE_EQ(
                motrack::readArchiveGroup(temporary.filePath("saved.motrack")).members.back().document.project.pointMasses.front().mass,
                13);
    }
}

TEST(WorkspaceSession, RestoreRefusesDivergentPairAndRecoveryUntilFreshCommonSnapshot)
{
    for (const bool grouped : {false, true})
    {
        SCOPED_TRACE(grouped);
        QTemporaryDir temporary;
        motrack::WorkspaceSession workspace;
        auto group = archiveSession();
        const auto source = grouped ? workspace.addGroup(group) : addStandalone(workspace);
        const auto id = source.documents.front();
        const auto target = temporary.filePath(grouped ? "saved.trz" : "saved.trk");
        const auto save = [&]
        {
            const auto request = workspace.beginSave(id);
            if (!request)
                throw std::runtime_error("Expected a save request.");
            return workspace.completePairedSave(*request, motrack::writeWorkspacePair(*request, target));
        };
        setMass(workspace, id, 7);
        ASSERT_EQ(save(), motrack::SaveOutcome::Saved);
        setMass(workspace, id, 9);
        const auto native = workspace.beginSave(id);
        if (!native)
            FAIL() << "Expected a native save request.";
        const auto copy = temporary.filePath("different.motrack");
        ASSERT_EQ(workspace.completeSave(*native, copy, motrack::writeWorkspaceNative(*native, copy)), motrack::SaveOutcome::Saved);
        workspace.hideDocument(id);
        EXPECT_FALSE(workspace.canRestoreSavedDocument(id));
        EXPECT_FALSE(workspace.restoreSavedDocument(id));
        EXPECT_DOUBLE_EQ(workspace.project(id).pointMasses.front().mass, 9);
        const auto partial = workspace.beginSave(id);
        if (!partial)
            FAIL() << "Expected a paired save request.";
        const auto result = motrack::writeWorkspacePair(*partial,
                                                        target,
                                                        [](auto progress)
                                                        {
                                                            if (progress == motrack::PairedSaveProgress::StandardSaved)
                                                                throw std::runtime_error("Interrupt after standard write.");
                                                        });
        ASSERT_EQ(workspace.completePairedSave(*partial, result), motrack::SaveOutcome::Partial);
        EXPECT_FALSE(workspace.restoreSavedDocument(id));
        const auto recovery = workspace.beginRecovery(id);
        if (!recovery)
            FAIL() << "Expected a recovery request.";
        ASSERT_EQ(workspace.completeRecovery(*recovery, motrack::recoverWorkspacePair(*recovery)), motrack::SaveOutcome::Recovered);
        EXPECT_FALSE(workspace.canRestoreSavedDocument(id));
        EXPECT_FALSE(workspace.restoreSavedDocument(id)); // Recovery publishes bytes, not a captured model.
        ASSERT_EQ(save(), motrack::SaveOutcome::Saved);
        setMass(workspace, id, 11);
        ASSERT_TRUE(workspace.restoreSavedDocument(id));
        EXPECT_DOUBLE_EQ(workspace.project(id).pointMasses.front().mass, 9);
        EXPECT_FALSE(workspace.isModified(source.source));
    }
}

} // namespace
