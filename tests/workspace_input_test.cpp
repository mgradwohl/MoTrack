#include "workspace_input.hpp"

#include "archive_fixture.hpp"
#include "main_window.hpp"
#include "tracker_archive.hpp"
#include "workspace_context.hpp"
#include "workspace_document_codec.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>

namespace
{

QString fixture(const char* name = "linked-images/linked-images.trk")
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/" + name;
}

QByteArray bytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

// Occupy only the two existing analysis workers, with finite waits and cleanup on failed assertions.
class HeldAnalysis
{
  public:
    explicit HeldAnalysis(std::shared_ptr<motrack::WorkspaceServices> services) : m_Services(std::move(services))
    {
        for (int i = 0; i < 2; ++i)
            m_Services->analysisPool().start(
                [state = m_State]
                {
                    state->entered.release();
                    (void)state->release.tryAcquire(1, 5000);
                });
    }
    ~HeldAnalysis()
    {
        release();
        EXPECT_TRUE(m_Services->analysisPool().waitForDone(5000));
    }
    bool ready()
    {
        return m_State->entered.tryAcquire(2, 2000);
    }
    void release()
    {
        m_State->release.release(2);
    }

  private:
    struct State
    {
        QSemaphore entered;
        QSemaphore release;
    };
    std::shared_ptr<motrack::WorkspaceServices> m_Services;
    std::shared_ptr<State> m_State = std::make_shared<State>();
};

TEST(WorkspaceInput, StandaloneTrackerAndNativeInputsRetainPhysicalMediaAndIndependentHistory)
{
    QTemporaryDir temporary;
    auto prepared = motrack::prepareWorkspaceInput(fixture());
    ASSERT_TRUE(prepared.document);
    EXPECT_FALSE(prepared.group);
    const auto original = prepared.document->document.project;
    ASSERT_FALSE(original.clip.mediaReferences.empty());
    const auto reference = original.clip.mediaReferences.front();
    const auto media = motrack::workspaceMediaPath(prepared.document->document, prepared.mediaProjectPath, reference);
    EXPECT_TRUE(QFileInfo::exists(media));
    motrack::WorkspaceSession workspace;
    const auto opened = motrack::adoptWorkspaceInput(workspace, prepared);
    EXPECT_FALSE(prepared.document);
    const auto id = opened.documents.front();
    ASSERT_TRUE(workspace.history(id).setPointMassValue(workspace.project(id), 0, 9));
    auto second = motrack::prepareWorkspaceInput(fixture());
    const auto other = motrack::adoptWorkspaceInput(workspace, second, motrack::WorkspaceActivation::KeepCurrent);
    EXPECT_EQ(workspace.activeDocument(), id);
    EXPECT_NE(opened.source, other.source);
    EXPECT_EQ(workspace.project(other.documents.front()), original);
    EXPECT_FALSE(workspace.history(other.documents.front()).canUndo());
    const auto path = temporary.filePath(QString::fromUtf8("résultats-測定.MOTRACK"));
    auto snapshot = workspace.document(id);
    motrack::rebaseWorkspaceMedia(snapshot, workspace.mediaProjectPath(id), path);
    motrack::writeWorkspaceDocument(path, snapshot);
    auto native = motrack::prepareWorkspaceInput(path);
    ASSERT_TRUE(native.document);
    EXPECT_FALSE(native.document->isModified());
    EXPECT_DOUBLE_EQ(native.document->document.project.pointMasses.front().mass, 9);
    EXPECT_EQ(motrack::workspaceMediaPath(
                  native.document->document, native.mediaProjectPath, native.document->document.project.clip.mediaReferences.front()),
              media);
    ASSERT_TRUE(workspace.history(id).undo(workspace.project(id)));
    EXPECT_EQ(workspace.project(id), original);
}

TEST(WorkspaceInput, CompleteImageAndMovieArchivesKeepAllMembersAndSourceAfterOriginalRemoval)
{
    for (const auto* source : {"linked-images/multitab-images.trz", "linked-mov/archived-movie.trz"})
    {
        SCOPED_TRACE(source);
        QTemporaryDir temporary;
        const auto path = temporary.filePath("original.TRZ");
        ASSERT_TRUE(QFile::copy(fixture(source), path));
        auto prepared = motrack::prepareWorkspaceInput(path);
        ASSERT_TRUE(prepared.group);
        EXPECT_FALSE(prepared.document);
        auto snapshot = prepared.group->prepareSave().snapshot;
        ASSERT_EQ(snapshot.members.size(), 2u);
        EXPECT_EQ(snapshot.archive->projects().size(), 2);
        EXPECT_FALSE(prepared.group->isModified());
        EXPECT_FALSE(prepared.group->requiresPairedSave()); // Import establishes a clean baseline, not an earlier paired save.
        EXPECT_EQ(prepared.group->standardSavePath(), QFileInfo(path).absoluteFilePath());
        const auto digest = snapshot.archive->sourceDigest();
        for (const auto& member : snapshot.members)
        {
            EXPECT_EQ(member.document.archivePayload, snapshot.archive);
            EXPECT_EQ(member.document.archiveMember, member.member);
            EXPECT_TRUE(member.document.alignMovieToClip);
            ASSERT_FALSE(member.document.project.clip.mediaReferences.empty());
            EXPECT_TRUE(QFileInfo::exists(
                QDir(snapshot.archive->root()).filePath(QString::fromStdString(member.document.project.clip.mediaReferences.front()))));
        }
        ASSERT_TRUE(QFile::remove(path));
        const auto nativePath = temporary.filePath("complete.motrack");
        motrack::writeArchiveGroup(nativePath, snapshot);
        auto native = motrack::prepareWorkspaceInput(nativePath);
        ASSERT_TRUE(native.group);
        const auto reopened = native.group->prepareSave().snapshot;
        EXPECT_EQ(reopened.id, snapshot.id);
        EXPECT_EQ(reopened.archive->sourceDigest(), digest);
        for (std::size_t i = 0; i < snapshot.members.size(); ++i)
        {
            EXPECT_EQ(reopened.members[i].id, snapshot.members[i].id);
            EXPECT_EQ(reopened.members[i].document.project, snapshot.members[i].document.project);
        }
        motrack::WorkspaceSession workspace;
        const auto opened = motrack::adoptWorkspaceInput(workspace, prepared);
        EXPECT_EQ(opened.documents.size(), 2u);
        EXPECT_EQ(workspace.tabs(), opened.documents);
        EXPECT_EQ(workspace.sourcePath(opened.source), QFileInfo(path).absoluteFilePath());
    }
}

TEST(WorkspaceInput, NativeGroupKeepsExactSavedMembershipOrderAndEdits)
{
    QTemporaryDir temporary;
    auto prepared = motrack::prepareWorkspaceInput(fixture("linked-images/multitab-images.trz"));
    auto snapshot = prepared.group->prepareSave().snapshot;
    std::ranges::reverse(snapshot.members);
    snapshot.members.front().document.project.pointMasses.front().mass = 13;
    const auto path = temporary.filePath("reordered.motrack");
    motrack::writeArchiveGroup(path, snapshot);
    auto reopened = motrack::prepareWorkspaceInput(path);
    const auto saved = reopened.group->prepareSave().snapshot;
    EXPECT_EQ(saved.id, snapshot.id);
    EXPECT_EQ(saved.members[0].id, snapshot.members[0].id);
    EXPECT_EQ(saved.members[1].id, snapshot.members[1].id);
    EXPECT_DOUBLE_EQ(saved.members[0].document.project.pointMasses.front().mass, 13);
    snapshot.members.pop_back(); // Unopened payload members are not invented as live records on native reopen.
    motrack::writeArchiveGroup(path, snapshot);
    reopened = motrack::prepareWorkspaceInput(path);
    EXPECT_EQ(reopened.group->memberIds().size(), 1u);
    EXPECT_EQ(reopened.group->prepareSave().snapshot.archive->projects().size(), 2);
}

TEST(WorkspaceInput, LegacyStandaloneArchiveCompanionIsNotImplicitlyPromoted)
{
    QTemporaryDir temporary;
    auto prepared = motrack::prepareWorkspaceInput(fixture("linked-images/multitab-images.trz"));
    const auto document = prepared.group->prepareSave().snapshot.members.front().document;
    const auto path = temporary.filePath("single.motrack");
    motrack::writeWorkspaceDocument(path, document);
    auto reopened = motrack::prepareWorkspaceInput(path);
    ASSERT_TRUE(reopened.document);
    EXPECT_FALSE(reopened.group);
    EXPECT_EQ(reopened.document->document.project, document.project);
    EXPECT_EQ(reopened.document->document.archiveMember, document.archiveMember);
    EXPECT_EQ(reopened.document->document.archivePayload->projects().size(), 2);
    EXPECT_EQ(reopened.mediaProjectPath, QDir(reopened.document->document.archivePayload->root()).absoluteFilePath("archive.trk"));
}

TEST(WorkspaceInput, OlderExternalArchiveCompanionRetainsPayloadWithoutInventingMemberIdentity)
{
    QTemporaryDir temporary;
    const auto archivePath = temporary.filePath("source.trz");
    ASSERT_TRUE(QFile::copy(fixture("linked-images/multitab-images.trz"), archivePath));
    auto prepared = motrack::prepareWorkspaceInput(archivePath);
    const auto document = prepared.group->prepareSave().snapshot.members.front().document;
    auto metadata = motrack::detail::encodeWorkspaceDocument(document);
    metadata.insert("version", 4);
    metadata.remove("embeddedArchive");
    metadata.remove("archiveMember");
    metadata.insert("archive", "source.trz");
    const auto path = temporary.filePath("old.motrack");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const auto json = QJsonDocument(metadata).toJson();
    ASSERT_EQ(file.write(json), json.size());
    file.close();
    auto reopened = motrack::prepareWorkspaceInput(path);
    ASSERT_TRUE(reopened.document);
    EXPECT_FALSE(reopened.group);
    EXPECT_FALSE(reopened.document->document.archiveMember);
    EXPECT_TRUE(reopened.document->document.archiveReference.empty());
    EXPECT_EQ(reopened.document->document.project, document.project);
    ASSERT_TRUE(QFile::remove(archivePath));
    const auto copy = temporary.filePath("embedded.motrack");
    motrack::writeWorkspaceDocument(copy, reopened.document->document);
    auto embedded = motrack::prepareWorkspaceInput(copy);
    EXPECT_EQ(embedded.document->document.archivePayload->sourceDigest(), document.archivePayload->sourceDigest());
    EXPECT_THROW((void)motrack::prepareWorkspaceInput(path), motrack::ProjectError);
}

TEST(WorkspaceInput, RejectsInvalidOrCancelledInputsAndAmbiguousAdoptionWithoutChangingLiveWork)
{
    QTemporaryDir temporary;
    motrack::WorkspaceSession workspace;
    auto first = motrack::prepareWorkspaceInput(fixture());
    const auto opened = motrack::adoptWorkspaceInput(workspace, first);
    const auto id = opened.documents.front();
    ASSERT_TRUE(workspace.history(id).setPointMassValue(workspace.project(id), 0, 17));
    const auto tracking = workspace.beginJob(id, motrack::WorkspaceJob::AutoTrack);
    const auto malformed = temporary.filePath("partial.trz");
    ASSERT_TRUE(motrack::test::writeArchive(malformed, {{"valid.trk", bytes(fixture())}, {"broken.trk", "invalid xml"}}));
    EXPECT_THROW((void)motrack::prepareWorkspaceInput(malformed), motrack::ProjectError);
    EXPECT_THROW((void)motrack::prepareWorkspaceInput(temporary.filePath("missing.trk")), motrack::ProjectError);
    const auto unsupported = temporary.filePath("input.txt");
    ASSERT_TRUE(QFile::copy(fixture(), unsupported));
    EXPECT_THROW((void)motrack::prepareWorkspaceInput(unsupported), motrack::ProjectError);
    const auto invalidNative = temporary.filePath("invalid.motrack");
    ASSERT_TRUE(QFile::copy(unsupported, invalidNative));
    EXPECT_THROW((void)motrack::prepareWorkspaceInput(invalidNative), motrack::ProjectError);
    const std::atomic_bool cancelled{true};
    EXPECT_THROW((void)motrack::prepareWorkspaceInput(fixture(), &cancelled), motrack::ProjectError);
    motrack::PreparedWorkspaceInput invalid;
    EXPECT_THROW((void)motrack::adoptWorkspaceInput(workspace, invalid), motrack::ProjectError);
    invalid = motrack::prepareWorkspaceInput(fixture());
    auto group = motrack::prepareWorkspaceInput(fixture("linked-images/multitab-images.trz"));
    invalid.group = std::move(group.group);
    EXPECT_THROW((void)motrack::adoptWorkspaceInput(workspace, invalid), motrack::ProjectError);
    EXPECT_TRUE(invalid.document);
    EXPECT_TRUE(invalid.group);
    EXPECT_EQ(workspace.tabs(), opened.documents);
    EXPECT_DOUBLE_EQ(workspace.project(id).pointMasses.front().mass, 17);
    EXPECT_TRUE(workspace.history(id).canUndo());
    EXPECT_TRUE(workspace.accepts(tracking));
}

TEST(WorkspaceInput, AsyncOpeningBoundsPendingInputsAndCancelsOnlyTheSelectedRequest)
{
    motrack::WorkspaceContext workspace;
    auto first = motrack::prepareWorkspaceInput(fixture());
    const auto existing = motrack::adoptWorkspaceInput(workspace.session(), first);
    const auto id = existing.documents.front();
    ASSERT_TRUE(workspace.session().history(id).setPointMassValue(workspace.session().project(id), 0, 19));
    const auto tracking = workspace.session().beginJob(id, motrack::WorkspaceJob::AutoTrack);
    QSignalSpy opened(&workspace, &motrack::WorkspaceContext::inputOpened);
    QSignalSpy cancelled(&workspace, &motrack::WorkspaceContext::inputCancelled);
    QSignalSpy failed(&workspace, &motrack::WorkspaceContext::inputFailed);
    HeldAnalysis held(workspace.services());
    ASSERT_TRUE(held.ready());
    const auto a = workspace.openProject(fixture());
    const auto b = workspace.openProject(fixture("linked-images/multitab-images.trz"), motrack::WorkspaceActivation::KeepCurrent);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_FALSE(workspace.openProject(fixture()));
    EXPECT_TRUE(workspace.cancelOpen(*a));
    EXPECT_FALSE(workspace.cancelOpen(QUuid::createUuid()));
    EXPECT_FALSE(workspace.openProject(fixture())); // A cancelled worker still occupies its slot.
    EXPECT_EQ(workspace.session().tabs(), existing.documents);
    held.release();
    ASSERT_TRUE(QTest::qWaitFor([&] { return opened.size() == 1 && cancelled.size() == 1; }, 5000));
    EXPECT_EQ(opened.front()[0].toUuid(), *b);
    EXPECT_EQ(opened.front()[2].value<QList<QUuid>>().size(), 2);
    EXPECT_EQ(cancelled.front()[0].toUuid(), *a);
    EXPECT_TRUE(failed.empty());
    EXPECT_FALSE(workspace.cancelOpen(*a));
    EXPECT_EQ(workspace.session().tabs().size(), 3u);
    EXPECT_EQ(workspace.session().activeDocument(), id);
    EXPECT_TRUE(workspace.session().accepts(tracking));
    EXPECT_TRUE(workspace.session().isModified(existing.source));
    EXPECT_DOUBLE_EQ(workspace.session().project(id).pointMasses.front().mass, 19);
    ASSERT_TRUE(workspace.openProject(fixture()));
    ASSERT_TRUE(QTest::qWaitFor([&] { return opened.size() == 2; }, 5000));
    EXPECT_NE(workspace.session().activeDocument(), id);
}

TEST(WorkspaceInput, FailedAsyncOpenRetainsDocumentsAndReleasesItsSlotForRetry)
{
    QTemporaryDir temporary;
    motrack::WorkspaceContext workspace;
    auto first = motrack::prepareWorkspaceInput(fixture());
    const auto existing = motrack::adoptWorkspaceInput(workspace.session(), first);
    QSignalSpy failed(&workspace, &motrack::WorkspaceContext::inputFailed);
    QSignalSpy opened(&workspace, &motrack::WorkspaceContext::inputOpened);
    const auto request = workspace.openProject(temporary.filePath("missing.trk"));
    ASSERT_TRUE(request);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !failed.empty(); }, 5000));
    EXPECT_EQ(failed.front()[0].toUuid(), *request);
    EXPECT_FALSE(failed.front()[1].toString().isEmpty());
    EXPECT_EQ(workspace.session().tabs(), existing.documents);
    EXPECT_FALSE(workspace.cancelOpen(*request));
    ASSERT_TRUE(workspace.openProject(fixture()));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !opened.empty(); }, 5000));
}

TEST(WorkspaceInput, DestroyingContextCancelsQueuedOpenWithoutBorrowingItsRegistry)
{
    const auto services = std::make_shared<motrack::WorkspaceServices>();
    auto workspace = std::make_unique<motrack::WorkspaceContext>(services);
    HeldAnalysis held(services);
    ASSERT_TRUE(held.ready());
    ASSERT_TRUE(workspace->openProject(fixture("linked-images/multitab-images.trz")));
    workspace.reset();
    held.release();
    EXPECT_TRUE(services->analysisPool().waitForDone(5000));
    QCoreApplication::processEvents();
}

TEST(WorkspaceInput, CompletionObserversMayDestroyTheirContextOnSuccessFailureOrCancellation)
{
    QTemporaryDir temporary;
    for (int outcome = 0; outcome < 3; ++outcome)
    {
        SCOPED_TRACE(outcome);
        const auto services = std::make_shared<motrack::WorkspaceServices>();
        auto workspace = std::make_unique<motrack::WorkspaceContext>(services);
        HeldAnalysis held(services);
        ASSERT_TRUE(held.ready());
        QObject::connect(workspace.get(), &motrack::WorkspaceContext::inputOpened, [&] { workspace.reset(); });
        QObject::connect(workspace.get(), &motrack::WorkspaceContext::inputFailed, [&] { workspace.reset(); });
        QObject::connect(workspace.get(), &motrack::WorkspaceContext::inputCancelled, [&] { workspace.reset(); });
        const auto request = workspace->openProject(outcome == 1 ? temporary.filePath("missing.trk") : fixture());
        ASSERT_TRUE(request);
        if (outcome == 2)
            ASSERT_TRUE(workspace->cancelOpen(*request));
        held.release();
        ASSERT_TRUE(QTest::qWaitFor([&] { return !workspace; }, 5000));
    }
}

TEST(WorkspaceInput, AsyncArchiveCanBindEditorsAndSaveEditsFromBothMembers)
{
    QTemporaryDir temporary;
    auto workspace = std::make_shared<motrack::WorkspaceContext>();
    QSignalSpy opened(workspace.get(), &motrack::WorkspaceContext::inputOpened);
    ASSERT_TRUE(workspace->openProject(fixture("linked-images/multitab-images.trz")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !opened.empty(); }, 5000));
    const auto documents = opened.front()[2].value<QList<QUuid>>();
    ASSERT_EQ(documents.size(), 2);
    motrack::MainWindow first(workspace, documents[0]);
    motrack::MainWindow second(workspace, documents[1]);
    for (qsizetype i = 0; i < documents.size(); ++i)
        ASSERT_TRUE(workspace->session().history(documents[i]).setPointMassValue(workspace->session().project(documents[i]), 0, 23 + i));
    const auto path = temporary.filePath("edited.motrack");
    ASSERT_TRUE(first.saveWorkspace(path));
    auto saved = motrack::prepareWorkspaceInput(path);
    const auto snapshot = saved.group->prepareSave().snapshot;
    EXPECT_DOUBLE_EQ(snapshot.members[0].document.project.pointMasses.front().mass, 23);
    EXPECT_DOUBLE_EQ(snapshot.members[1].document.project.pointMasses.front().mass, 24);
}

} // namespace
