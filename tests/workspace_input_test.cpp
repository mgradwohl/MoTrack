#include "workspace_input.hpp"

#include "archive_fixture.hpp"
#include "main_window.hpp"
#include "tracker_archive.hpp"
#include "tracker_writer.hpp"
#include "workspace_context.hpp"
#include "workspace_document_codec.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

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
    // Tracker opens a .zip experiment exactly as a .trz (ResourceLoader.isJarZipTrz, #224); only its save target differs.
    for (const auto& [source, name] :
         std::initializer_list<std::pair<const char*, const char*>>{{"linked-images/multitab-images.trz", "original.TRZ"},
                                                                    {"linked-mov/archived-movie.trz", "original.TRZ"},
                                                                    {"linked-images/multitab-images.trz", "Library Lab.Zip"},
                                                                    {"linked-mov/archived-movie.trz", "Library Lab.Zip"}})
    {
        SCOPED_TRACE(std::string(source) + " as " + name);
        QTemporaryDir temporary;
        const auto path = temporary.filePath(name);
        ASSERT_TRUE(QFile::copy(fixture(source), path));
        auto prepared = motrack::prepareWorkspaceInput(path);
        ASSERT_TRUE(prepared.group);
        EXPECT_FALSE(prepared.document);
        auto snapshot = prepared.group->prepareSave().snapshot;
        ASSERT_EQ(snapshot.members.size(), 2u);
        EXPECT_EQ(snapshot.archive->projects().size(), 2);
        EXPECT_FALSE(prepared.group->isModified());
        EXPECT_FALSE(prepared.group->requiresPairedSave()); // Import establishes a clean baseline, not an earlier paired save.
        // Tracker never writes into a .zip and exports archives as .trz, so a .zip is not a standard save target.
        EXPECT_EQ(prepared.group->standardSavePath(), motrack::isTrackerZipPath(path) ? QString{} : QFileInfo(path).absoluteFilePath());
        EXPECT_TRUE(prepared.group->nativeSavePath().isEmpty());
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

// The linked-image experiment and its four frames as archive members.
std::vector<motrack::test::ArchiveEntry> imageExperiment(const QByteArray& xml)
{
    std::vector<motrack::test::ArchiveEntry> members{{.name = "experiment.trk", .bytes = xml}};
    for (const auto& [member, file] :
         std::initializer_list<std::pair<const char*, const char*>>{{"images/frame0.png", "linked-images/images/frame0.png"},
                                                                    {"images/frame1.png", "linked-images/images/frame1.png"},
                                                                    {"images/frame2.png", "linked-images/images/frame2.png"},
                                                                    {"images/frame3.png", "linked-images/images/frame3.png"}})
        members.push_back({.name = member, .bytes = bytes(fixture(file))});
    return members;
}

TEST(WorkspaceInput, OpensArchivesWithUnsafeMacAndUnreadableMembersAndKeepsTheirBytesForSaving)
{
    auto members = imageExperiment(bytes(fixture()));
    for (auto& unsafe : motrack::test::unsafeTrackerExportMembers())
        members.push_back(std::move(unsafe));
    members.insert(members.end(),
                   {{.name = "C:/Users/x/images/frame0.png", .bytes = bytes(fixture("linked-images/images/frame0.png"))},
                    {.name = "videos/a.mp4", .bytes = motrack::test::duplicateMovieBytes()},
                    {.name = "__MACOSX/._experiment.trk", .bytes = motrack::test::appleDoubleBytes()},
                    {.name = "broken.trk", .bytes = "invalid xml"}});
    // A .zip experiment follows exactly the same member rules as a .trz (#224).
    for (const auto* name : {"library.trz", "library.zip"})
    {
        SCOPED_TRACE(name);
        QTemporaryDir temporary;
        const auto path = temporary.filePath(name);
        ASSERT_TRUE(motrack::test::writeArchive(path, members));
        auto prepared = motrack::prepareWorkspaceInput(path);
        ASSERT_TRUE(prepared.group);
        ASSERT_EQ(prepared.unreadableProjects.size(), 1);
        EXPECT_EQ(prepared.unreadableProjects.front().member, "broken.trk");
        EXPECT_FALSE(prepared.unreadableProjects.front().reason.isEmpty());
        const auto snapshot = prepared.group->prepareSave().snapshot;
        ASSERT_EQ(snapshot.members.size(), 1u);
        EXPECT_EQ(snapshot.members.front().member, "experiment.trk");
        EXPECT_EQ(snapshot.archive->projects(), (QStringList{"broken.trk", "experiment.trk"}));
        EXPECT_EQ(snapshot.archive->skippedMembers().size(), 5);
        EXPECT_EQ(snapshot.archive->omittedOnSave(), motrack::test::omittedTrackerExportMembers() << "C:/Users/x/images/frame0.png");
        EXPECT_EQ(snapshot.members.front().document.project.clip.mediaReferences,
                  (std::vector<std::string>{"images/frame0.png", "images/frame1.png", "images/frame2.png", "images/frame3.png"}));

        // Standard saving keeps the unreadable project, Finder metadata, Finder icons and the unique info page
        // byte-for-byte; only the duplicate absolute copies are omitted.
        const auto standard = temporary.filePath("saved.trz");
        motrack::writeTrackerArchiveDocuments(standard, std::span(&snapshot.members.front().document, 1));
        const auto saved = motrack::TrackerArchive::open(standard);
        EXPECT_EQ(saved.skippedMembers(),
                  (QStringList{"C:/Users/x/AppData/Local/Temp/tracker1/html/experiment_info.html", "html/Icon\r", "images/Icon\r"}));
        EXPECT_EQ(bytes(QDir(saved.root()).filePath("broken.trk")), "invalid xml");
        ASSERT_TRUE(saved.memberDigests().contains("__MACOSX/._experiment.trk"));
        EXPECT_EQ(saved.memberDigests().value("__MACOSX/._experiment.trk"),
                  snapshot.archive->memberDigests().value("__MACOSX/._experiment.trk"));
        // The native companion embeds the exact original archive, unsafe members included.
        const auto native = temporary.filePath("library.motrack");
        motrack::writeArchiveGroup(native, snapshot);
        const auto reopened = motrack::prepareWorkspaceInput(native);
        EXPECT_EQ(reopened.group->prepareSave().snapshot.archive->sourceDigest(), snapshot.archive->sourceDigest());
        EXPECT_EQ(reopened.group->prepareSave().snapshot.archive->skippedMembers(), snapshot.archive->skippedMembers());
    }
}

TEST(WorkspaceInput, RefusesZipFilesWithoutATrackerProjectOrWithAnUnsafeProjectName)
{
    QTemporaryDir temporary;
    for (const auto* path : {"Lab.zip", "LAB.ZIP", "lab.trz", "lab.trk", "lab.MoTrack"})
        EXPECT_TRUE(motrack::isWorkspaceProjectPath(path)) << path;
    for (const auto* path : {"lab.jar", "lab.zip.mov", "frame.png", "zip"})
        EXPECT_FALSE(motrack::isWorkspaceProjectPath(path)) << path;
    // Tracker warns "Not a Tracker file" for a ZIP without a project. (A ZIP of numbered images at its root would
    // open in Tracker as an image video; MoTrack has no zipped image sequences yet, #347.)
    const auto images = temporary.filePath("frames.zip");
    ASSERT_TRUE(motrack::test::writeArchive(images,
                                            {{.name = "frame0.png", .bytes = bytes(fixture("linked-images/images/frame0.png"))},
                                             {.name = "frame1.png", .bytes = bytes(fixture("linked-images/images/frame1.png"))}}));
    try
    {
        (void)motrack::prepareWorkspaceInput(images);
        FAIL() << "A ZIP without a Tracker project must not open as an empty archive.";
    }
    catch (const motrack::ProjectError& error)
    {
        EXPECT_TRUE(QString::fromUtf8(error.what()).contains("no Tracker project")) << error.what();
    }
    // An unsafe project name refuses a .zip exactly as it refuses a .trz.
    auto members = imageExperiment(bytes(fixture()));
    members.push_back({.name = "D:/a/b.trk", .bytes = bytes(fixture())});
    const auto unsafe = temporary.filePath("unsafe.zip");
    ASSERT_TRUE(motrack::test::writeArchive(unsafe, members));
    EXPECT_THROW((void)motrack::prepareWorkspaceInput(unsafe), motrack::ProjectError);
    // Other archive suffixes are not projects, whatever they hold.
    const auto jar = temporary.filePath("lab.jar");
    ASSERT_TRUE(QFile::copy(fixture("linked-images/multitab-images.trz"), jar));
    EXPECT_THROW((void)motrack::prepareWorkspaceInput(jar), motrack::ProjectError);
}

TEST(WorkspaceInput, RefusesProjectsThatUseAnUnsafeMemberAndOpensTheOthers)
{
    QTemporaryDir temporary;
    auto xml = bytes(fixture());
    xml.replace("images/frame2.png", "C:/Users/x/images/frame2.png");
    auto members = imageExperiment(xml);
    members.push_back({.name = "C:/Users/x/images/frame2.png", .bytes = bytes(fixture("linked-images/images/frame2.png"))});
    const auto path = temporary.filePath("absolute.trz");
    ASSERT_TRUE(motrack::test::writeArchive(path, members));
    try
    {
        (void)motrack::prepareWorkspaceInput(path);
        FAIL() << "An archive whose only project uses an unsafe member must not open.";
    }
    catch (const motrack::ProjectError& error)
    {
        const auto message = QString::fromUtf8(error.what());
        EXPECT_TRUE(message.contains("experiment.trk") && message.contains("unsafe name")) << error.what();
    }
    // Another readable project opens; the unsafe one is reported with its reason and kept for saving.
    members.push_back({.name = "readable.trk", .bytes = bytes(fixture())});
    ASSERT_TRUE(motrack::test::writeArchive(path, members));
    {
        const auto partial = motrack::prepareWorkspaceInput(path);
        const auto snapshot = partial.group->prepareSave().snapshot;
        ASSERT_EQ(snapshot.members.size(), 1u);
        EXPECT_EQ(snapshot.members.front().member, "readable.trk");
        ASSERT_EQ(partial.unreadableProjects.size(), 1);
        EXPECT_EQ(partial.unreadableProjects.front().member, "experiment.trk");
        EXPECT_TRUE(partial.unreadableProjects.front().reason.contains("unsafe name"));
    }
    members.pop_back();
    // An absolute reference to a file outside the archive stays missing, never a local fallback.
    members.pop_back();
    ASSERT_TRUE(motrack::test::writeArchive(path, members));
    const auto prepared = motrack::prepareWorkspaceInput(path);
    EXPECT_EQ(prepared.group->prepareSave().snapshot.members.front().document.project.clip.mediaReferences[2], "");
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
    const auto malformed = temporary.filePath("broken.trz");
    ASSERT_TRUE(motrack::test::writeArchive(malformed, {{"broken.trk", "invalid xml"}, {"other.trk", "<object/>"}}));
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
    if (!a || !b)
        FAIL() << "Expected both bounded input slots.";
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
    if (!request)
        FAIL() << "Expected an input slot for the failed-open test.";
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
        if (!request)
            FAIL() << "Expected an input slot for the completion-lifetime test.";
        if (outcome == 2)
            ASSERT_TRUE(workspace->cancelOpen(*request));
        held.release();
        ASSERT_TRUE(QTest::qWaitFor([&] { return !workspace; }, 5000));
    }
}

TEST(WorkspaceInput, PreparesManyArchiveMembersInOrderWithUniqueIdentities)
{
    QTemporaryDir temporary;
    std::array<QByteArray, 32> names;
    std::vector<motrack::test::ArchiveEntry> entries;
    const auto xml = bytes(fixture());
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        names[i] = QByteArray::number(i).rightJustified(2, '0') + ".trk";
        entries.push_back({.name = names[i].constData(), .bytes = xml});
    }
    const auto path = temporary.filePath("many.trz");
    ASSERT_TRUE(motrack::test::writeArchive(path, entries));
    auto input = motrack::prepareWorkspaceInput(path);
    const auto snapshot = input.group->prepareSave().snapshot;
    ASSERT_EQ(snapshot.members.size(), names.size());
    QSet<QUuid> identities;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        EXPECT_EQ(snapshot.members[i].member, names[i].toStdString());
        EXPECT_EQ(snapshot.members[i].document.archivePayload, snapshot.archive);
        EXPECT_FALSE(snapshot.members[i].id.isNull());
        identities.insert(snapshot.members[i].id);
    }
    EXPECT_EQ(identities.size(), names.size());
}

TEST(WorkspaceInput, MovieMetadataIsPreparedWithoutViewsAndInvalidMoviesFailBeforeAdoption)
{
    QTemporaryDir temporary;
    auto input = motrack::prepareWorkspaceInput(fixture("linked-mov/linked-movie.trk"));
    ASSERT_TRUE(input.document);
    EXPECT_TRUE(input.document->document.movie);
    EXPECT_EQ(input.document->document.movieFrameTimes, (std::vector<double>{0, .125, .25, .375}));
    EXPECT_EQ(input.document->document.project.clip.sourceFrameCount, 4);
    auto document = input.document->document;
    document.movieFrameTimes.clear();
    document.movie = false; // Earlier native groups can contain members never opened in an editor.
    auto group = motrack::prepareWorkspaceInput(fixture("linked-mov/archived-movie.trz"));
    auto snapshot = group.group->prepareSave().snapshot;
    snapshot.members.front().document.movieFrameTimes.clear();
    snapshot.members.front().document.movie = false;
    const auto nativeGroup = temporary.filePath("unindexed.motrack");
    motrack::writeArchiveGroup(nativeGroup, snapshot);
    auto reopened = motrack::prepareWorkspaceInput(nativeGroup);
    const auto movie = reopened.group->prepareSave().snapshot.members.front().document;
    EXPECT_TRUE(movie.movie);
    EXPECT_EQ(movie.movieFrameTimes, (std::vector<double>{0, .125, .25, .375}));
    const auto invalidPath = temporary.filePath("invalid.mov");
    QFile invalid(invalidPath);
    ASSERT_TRUE(invalid.open(QIODevice::WriteOnly));
    ASSERT_EQ(invalid.write("not a movie"), 11);
    invalid.close();
    document.project.clip.mediaReferences = {"invalid.mov"};
    const auto native = temporary.filePath("invalid.motrack");
    motrack::writeWorkspaceDocument(native, document);
    EXPECT_THROW((void)motrack::prepareWorkspaceInput(native), motrack::ProjectError);
}

TEST(WorkspaceInput, MovieTimeBaseIsIndexedAndSavedWithTheFrameTimes)
{
    // The index records the stream time base the decoded frame times were quantized to (#331) and the precision
    // inferred from the timestamps (#335), and the native document keeps both beside them. Documents saved before
    // #331 open with the time base unknown, and documents saved before #335 with the precision unknown.
    QTemporaryDir temporary;
    auto input = motrack::prepareWorkspaceInput(fixture("linked-mov/linked-movie.trk"));
    ASSERT_TRUE(input.document);
    const auto& indexed = input.document->document;
    const auto timeBase = indexed.movieTimeBase;
    ASSERT_TRUE(timeBase.known());
    // The fixture's 1/1000 s tick is already its timestamps' precision.
    ASSERT_TRUE(timeBase.precisionKnown());
    EXPECT_EQ(timeBase.precisionNumerator, timeBase.numerator);
    EXPECT_EQ(timeBase.precisionDenominator, timeBase.denominator);
    const double tick = timeBase.resolutionSeconds();
    for (const auto time : indexed.movieFrameTimes)
    {
        EXPECT_NEAR(time, std::round(time / tick) * tick, 1e-9) << time; // Whole ticks.
    }
    const auto native = temporary.filePath("time-base.motrack");
    motrack::writeWorkspaceDocument(native, indexed);
    EXPECT_EQ(motrack::readWorkspaceDocument(native).movieTimeBase, timeBase);

    const auto rewrite = [&native](const char* key, const QJsonValue& value)
    {
        QFile file(native);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        auto object = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        if (value.isUndefined())
            object.remove(QString::fromLatin1(key));
        else
            object.insert(QString::fromLatin1(key), value);
        const auto bytes = QJsonDocument(object).toJson();
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    // Saved before #335: the time base without a precision, so the tick applies.
    ASSERT_TRUE(rewrite("movieTimestampPrecision", QJsonValue::Undefined));
    const auto beforePrecision = motrack::readWorkspaceDocument(native).movieTimeBase;
    EXPECT_EQ(beforePrecision, (motrack::FrameTimeBase{.numerator = timeBase.numerator, .denominator = timeBase.denominator}));
    EXPECT_FALSE(beforePrecision.precisionKnown());
    EXPECT_EQ(beforePrecision.resolutionSeconds(), tick);
    // A precision is at least the tick and at most max(tick, 1 ms): 1 ms for a declared 1/16000 s (MKV remuxed to
    // MP4, whose frame times are whole milliseconds like these), or the tick itself.
    ASSERT_TRUE(rewrite("movieTimeBase", QJsonArray{1, 16000}));
    for (const auto& valid : {QJsonArray{1, 1000}, QJsonArray{1, 16000}, QJsonArray{2, 4000}})
    {
        ASSERT_TRUE(rewrite("movieTimestampPrecision", valid));
        const auto reopened = motrack::readWorkspaceDocument(native).movieTimeBase;
        EXPECT_EQ(reopened.precisionNumerator, valid.at(0).toInt());
        EXPECT_EQ(reopened.precisionDenominator, valid.at(1).toInt());
    }
    for (const auto& invalid : {QJsonValue(QJsonArray{1, 32000}),
                                QJsonValue(QJsonArray{1, 999}),
                                QJsonValue(QJsonArray{0, 1000}),
                                QJsonValue(QJsonArray{1}),
                                QJsonValue(QJsonArray{1, -1000}),
                                QJsonValue("1/1000")})
    {
        ASSERT_TRUE(rewrite("movieTimestampPrecision", invalid));
        EXPECT_THROW((void)motrack::readWorkspaceDocument(native), motrack::ProjectError);
    }
    ASSERT_TRUE(rewrite("movieTimeBase", QJsonArray{1, 600}));
    ASSERT_TRUE(rewrite("movieTimestampPrecision", QJsonArray{1, 1000})); // Finer than the 1/600 s tick.
    EXPECT_THROW((void)motrack::readWorkspaceDocument(native), motrack::ProjectError);
    // The precision describes the time base's ticks, so it is not accepted without one.
    ASSERT_TRUE(rewrite("movieTimeBase", QJsonValue::Undefined));
    EXPECT_THROW((void)motrack::readWorkspaceDocument(native), motrack::ProjectError);

    // Saved before #331: neither.
    ASSERT_TRUE(rewrite("movieTimestampPrecision", QJsonValue::Undefined));
    const auto earlier = motrack::readWorkspaceDocument(native);
    EXPECT_FALSE(earlier.movieTimeBase.known());
    EXPECT_EQ(earlier.movieFrameTimes, indexed.movieFrameTimes);
    for (const auto& invalid :
         {QJsonValue(QJsonArray{0, 600}), QJsonValue(QJsonArray{1}), QJsonValue(QJsonArray{1, -600}), QJsonValue("1/600")})
    {
        ASSERT_TRUE(rewrite("movieTimeBase", invalid));
        EXPECT_THROW((void)motrack::readWorkspaceDocument(native), motrack::ProjectError);
    }
    // The time base describes frame times, so a document without them neither writes nor accepts one.
    auto stale = indexed;
    stale.movieFrameTimes.clear();
    motrack::writeWorkspaceDocument(native, stale);
    EXPECT_FALSE(motrack::readWorkspaceDocument(native).movieTimeBase.known());
    ASSERT_TRUE(rewrite("movieTimeBase", QJsonArray{1, 600}));
    EXPECT_THROW((void)motrack::readWorkspaceDocument(native), motrack::ProjectError);
}

TEST(WorkspaceInput, SavingAnImageMemberAlsoPreservesAnUnopenedMovieMember)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    ASSERT_TRUE(QFile::copy(fixture("linked-mov/archived-movie.trz"), original));
    auto workspace = std::make_shared<motrack::WorkspaceContext>();
    QSignalSpy opened(workspace.get(), &motrack::WorkspaceContext::inputOpened);
    ASSERT_TRUE(workspace->openProject(original));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !opened.empty(); }, 5000));
    const auto documents = opened.front()[2].value<QList<QUuid>>();
    ASSERT_EQ(documents.size(), 2);
    const auto movie = documents[0];
    const auto image = documents[1];
    ASSERT_EQ(workspace->session().document(movie).archiveMember, "experiment.trk");
    ASSERT_EQ(workspace->session().document(image).archiveMember, "second.trk");
    const auto movieProject = workspace->session().project(movie);
    const auto archive = workspace->session().document(movie).archivePayload;
    ASSERT_TRUE(QFile::remove(original));
    motrack::MainWindow imageView(workspace, image); // No movie editor is ever constructed.
    ASSERT_TRUE(workspace->session().history(image).setPointMassValue(workspace->session().project(image), 0, 29));
    const auto output = temporary.filePath("mixed.trz");
    ASSERT_TRUE(imageView.saveTrackerProject(output));
    auto saved = motrack::prepareWorkspaceInput(temporary.filePath("mixed.motrack"));
    const auto snapshot = saved.group->prepareSave().snapshot;
    EXPECT_EQ(snapshot.members[0].document.project, movieProject);
    EXPECT_TRUE(snapshot.members[0].document.movie);
    EXPECT_EQ(snapshot.members[0].document.movieFrameTimes, (std::vector<double>{0, .125, .25, .375}));
    EXPECT_DOUBLE_EQ(snapshot.members[1].document.project.pointMasses.front().mass, 29);
    EXPECT_EQ(snapshot.archive->sourceDigest(), archive->sourceDigest());
    const auto standard = motrack::TrackerArchive::open(output);
    EXPECT_EQ(bytes(QDir(standard.root()).filePath("constant-rate.mov")), bytes(QDir(archive->root()).filePath("constant-rate.mov")));
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
