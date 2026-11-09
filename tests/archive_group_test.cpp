#include "archive_group.hpp"

#include "archive_fixture.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"
#include "tracker_writer.hpp"
#include "workspace_storage.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <functional>
#include <limits>

namespace
{

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

std::shared_ptr<const motrack::TrackerArchive> sourceArchive()
{
    const auto path = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/multitab-images.trz";
    return std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(path));
}

motrack::WorkspaceDocument document(const std::shared_ptr<const motrack::TrackerArchive>& archive, const char* member)
{
    auto result = motrack::importTrackerDocument(archive->projectPath(member));
    archive->resolveMedia(result.project, member);
    result.archivePayload = archive;
    result.archiveMember = member;
    return result;
}

motrack::ArchiveGroup group()
{
    const auto archive = sourceArchive();
    auto result = motrack::makeArchiveGroup(document(archive, "experiment.trk"));
    (void)motrack::appendArchiveGroupDocument(result, document(archive, "second.trk"));
    return result;
}

TEST(ArchiveGroup, RetainsBothEditedMembersAfterRelocationAndStandardExport)
{
    QTemporaryDir temporary;
    const auto firstPath = temporary.filePath("first.motrack");
    const auto relocatedPath = temporary.filePath(QString::fromUtf8("réouverture.motrack"));
    auto original = group();
    const auto archiveBytes = readBytes(original.archive->sourcePath());
    const auto groupId = original.id;
    const auto firstId = original.members[0].id;
    const auto secondId = original.members[1].id;
    const auto& firstOriginalSource = original.members[0].document.trackerSource;
    const auto& secondOriginalSource = original.members[1].document.trackerSource;
    ASSERT_TRUE(firstOriginalSource);
    ASSERT_TRUE(secondOriginalSource);
    const auto firstSource = firstOriginalSource->xml;
    const auto secondSource = secondOriginalSource->xml;
    motrack::TrackEdits firstEdits;
    ASSERT_TRUE(firstEdits.setPointMassValue(original.members[0].document.project, 0, 5));
    motrack::writeArchiveGroup(firstPath, original);
    ASSERT_TRUE(firstEdits.undo(original.members[0].document.project));
    ASSERT_TRUE(firstEdits.redo(original.members[0].document.project));
    motrack::TrackEdits secondEdits;
    ASSERT_TRUE(secondEdits.setPointMassValue(original.members[1].document.project, 0, 7));
    motrack::writeArchiveGroup(firstPath, original);
    EXPECT_THROW((void)motrack::readWorkspaceDocument(firstPath), motrack::ProjectError);
    original = {};
    ASSERT_TRUE(QFile::rename(firstPath, relocatedPath));
    auto reopened = motrack::readArchiveGroup(relocatedPath);
    ASSERT_EQ(reopened.members.size(), 2u);
    EXPECT_EQ(reopened.id, groupId);
    EXPECT_EQ(reopened.members[0].id, firstId);
    EXPECT_EQ(reopened.members[1].id, secondId);
    EXPECT_EQ(reopened.members[0].document.archivePayload, reopened.archive);
    EXPECT_EQ(reopened.members[1].document.archivePayload, reopened.archive);
    EXPECT_EQ(readBytes(reopened.archive->sourcePath()), archiveBytes);
    const auto& firstReopenedSource = reopened.members[0].document.trackerSource;
    const auto& secondReopenedSource = reopened.members[1].document.trackerSource;
    ASSERT_TRUE(firstReopenedSource);
    ASSERT_TRUE(secondReopenedSource);
    EXPECT_EQ(firstReopenedSource->xml, firstSource);
    EXPECT_EQ(secondReopenedSource->xml, secondSource);
    EXPECT_DOUBLE_EQ(reopened.members[0].document.project.pointMasses.front().mass, 5);
    EXPECT_DOUBLE_EQ(reopened.members[1].document.project.pointMasses.front().mass, 7);
    std::vector<motrack::WorkspaceDocument> documents;
    documents.reserve(reopened.members.size());
    for (const auto& member : reopened.members)
        documents.push_back(member.document);
    const auto standard = temporary.filePath("edited.trz");
    motrack::writeTrackerArchiveDocuments(standard, documents);
    const auto exported = motrack::TrackerArchive::open(standard);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(exported.projectPath("experiment.trk")).project.pointMasses.front().mass, 5);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(exported.projectPath("second.trk")).project.pointMasses.front().mass, 7);
    const auto copy = motrack::readArchiveGroup(relocatedPath);
    EXPECT_EQ(copy.id, reopened.id);
    EXPECT_NE(copy.archive, reopened.archive);
    EXPECT_EQ(copy.archive->sourceDigest(), reopened.archive->sourceDigest());
}

TEST(ArchiveGroup, PromotesKnownMemberCompanionsWithoutJoiningEqualDigestOwners)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("member.motrack");
    const auto source = sourceArchive();
    motrack::writeWorkspaceDocument(path, document(source, "experiment.trk"));
    auto promoted = motrack::makeArchiveGroup(motrack::readWorkspaceDocument(path));
    EXPECT_NE(promoted.archive, source);
    EXPECT_EQ(promoted.archive->sourceDigest(), source->sourceDigest());
    const auto id = promoted.id;
    const auto memberId = promoted.members.front().id;
    EXPECT_THROW((void)motrack::appendArchiveGroupDocument(promoted, document(source, "second.trk")), motrack::ProjectError);
    EXPECT_THROW((void)motrack::appendArchiveGroupDocument(promoted, document(promoted.archive, "experiment.trk")), motrack::ProjectError);
    EXPECT_EQ(promoted.id, id);
    ASSERT_EQ(promoted.members.size(), 1u);
    EXPECT_EQ(promoted.members.front().id, memberId);
    const auto addedId = motrack::appendArchiveGroupDocument(promoted, document(promoted.archive, "second.trk"));
    EXPECT_EQ(promoted.members.back().id, addedId);
    EXPECT_NE(addedId, memberId);
    EXPECT_THROW((void)motrack::appendArchiveGroupDocument(promoted, document(promoted.archive, "second.trk")), motrack::ProjectError);
    EXPECT_THROW((void)motrack::readArchiveGroup(path), motrack::ProjectError);
    auto unknown = document(source, "experiment.trk");
    unknown.archiveMember.reset();
    EXPECT_THROW((void)motrack::makeArchiveGroup(unknown), motrack::ProjectError);
    unknown.archiveMember = "experiment.trk";
    unknown.trackerSource.reset();
    EXPECT_THROW((void)motrack::makeArchiveGroup(unknown), motrack::ProjectError);
}

TEST(ArchiveGroup, PreservesUntouchedExperimentsAndOpaqueContentAfterOriginalRemoval)
{
    QTemporaryDir temporary;
    const auto originalPath = temporary.filePath("original.trz");
    const auto nativePath = temporary.filePath("group.motrack");
    const QByteArray xml = "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>"
                           "<property name='tracks'><property name='[0]'>"
                           "<object class='org.opensourcephysics.cabrillo.tracker.PointMass'>"
                           "<property name='name' type='string'>mass</property>"
                           "<property name='mass' type='double'>1.0</property>"
                           "</object></property></property></object>";
    const QByteArray untouched = "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>"
                                 "<property name='future'><object class='UnknownSettings'/></property></object>";
    ASSERT_TRUE(motrack::test::writeArchive(
        originalPath, {{"a.trk", xml}, {"b.trk", xml}, {"untouched.trk", untouched}, {"opaque.bin", QByteArray("a\0b", 3)}}));
    const auto originalBytes = readBytes(originalPath);
    {
        const auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(originalPath));
        auto snapshot = motrack::makeArchiveGroup(document(payload, "a.trk"));
        (void)motrack::appendArchiveGroupDocument(snapshot, document(payload, "b.trk"));
        snapshot.members[0].document.project.pointMasses.front().mass = 5;
        snapshot.members[1].document.project.pointMasses.front().mass = 7;
        motrack::writeArchiveGroup(nativePath, snapshot);
    }
    ASSERT_TRUE(QFile::remove(originalPath));
    const auto reopened = motrack::readArchiveGroup(nativePath);
    EXPECT_EQ(readBytes(reopened.archive->sourcePath()), originalBytes);
    const std::vector<motrack::WorkspaceDocument> documents{reopened.members[1].document, reopened.members[0].document};
    const auto standard = temporary.filePath("edited.trz");
    motrack::writeTrackerArchiveDocuments(standard, documents);
    const auto exported = motrack::TrackerArchive::open(standard);
    EXPECT_EQ(readBytes(exported.projectPath("untouched.trk")), untouched);
    EXPECT_EQ(readBytes(QDir(exported.root()).filePath("opaque.bin")), QByteArray("a\0b", 3));
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(exported.projectPath("a.trk")).project.pointMasses.front().mass, 5);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(exported.projectPath("b.trk")).project.pointMasses.front().mass, 7);
}

TEST(ArchiveGroup, RejectsInvalidSnapshotsWithoutReplacingExistingOutput)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("saved.motrack");
    const auto valid = group();
    motrack::writeArchiveGroup(path, valid);
    const auto before = readBytes(path);
    const std::vector<std::function<void(motrack::ArchiveGroup&)>> mutations{
        [](auto& value) { value.id = QUuid{}; },
        [](auto& value) { value.archive.reset(); },
        [](auto& value) { value.members.clear(); },
        [](auto& value) { value.members.push_back(value.members.front()); },
        [](auto& value) { value.members[1].id = value.members[0].id; },
        [](auto& value) { value.members[1].id = QUuid{}; },
        [](auto& value) { value.members[1].member.clear(); },
        [](auto& value)
        {
            value.members[1] = value.members[0];
            value.members[1].id = QUuid::createUuid();
        },
        [](auto& value) { value.members[1].document.archivePayload = sourceArchive(); },
        [](auto& value) { value.members[1].document.archiveReference = "outside.trz"; },
        [](auto& value) { value.members[1].document.archiveMember = "missing.trk"; },
        [](auto& value)
        {
            value.members[1].member = "missing.trk";
            value.members[1].document.archiveMember = "missing.trk";
        },
        [](auto& value) { value.members[1].document.trackerSource.reset(); },
        [](auto& value) { value.members[1].document.trackerSource = value.members[0].document.trackerSource; },
        [](auto& value) { value.members[1].document.project.pointMasses.front().id = 0; },
        [](auto& value)
        {
            value.members[1].document.project.clip.startTimeMs = std::numeric_limits<double>::quiet_NaN();
        }};
    for (std::size_t index = 0; index < mutations.size(); ++index)
    {
        SCOPED_TRACE(index);
        auto invalid = valid;
        mutations[index](invalid);
        EXPECT_THROW(motrack::writeArchiveGroup(path, invalid), motrack::ProjectError);
        EXPECT_EQ(readBytes(path), before);
    }
    const std::atomic_bool cancelled{true};
    EXPECT_THROW(motrack::writeArchiveGroup(path, valid, &cancelled), motrack::ProjectError);
    EXPECT_THROW((void)motrack::readArchiveGroup(path, &cancelled), motrack::ProjectError);
    EXPECT_EQ(readBytes(path), before);
    EXPECT_THROW(motrack::writeArchiveGroup(QDir(valid.archive->root()).filePath("unsafe.motrack"), valid), motrack::ProjectError);
}

TEST(ArchiveGroup, RejectsAlteredExtractedSourceEvenWhenMetadataMatchesIt)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("saved.motrack");
    auto snapshot = group();
    motrack::writeArchiveGroup(path, snapshot);
    const auto before = readBytes(path);
    auto& member = snapshot.members[1];
    const auto sourcePath = snapshot.archive->projectPath(QString::fromStdString(member.member));
    const auto originalXml = readBytes(sourcePath);
    const auto modifiedXml = originalXml + "\n<!-- changed extracted source -->\n";
    ASSERT_TRUE(writeBytes(sourcePath, modifiedXml));
    ASSERT_TRUE(member.document.trackerSource);
    member.document.trackerSource->xml = modifiedXml.toStdString();
    EXPECT_THROW(motrack::writeArchiveGroup(path, snapshot), motrack::ProjectError);
    EXPECT_EQ(readBytes(path), before);
}

TEST(ArchiveGroup, RejectsAmbiguousGroupMetadataAndMissingPayload)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("saved.motrack");
    const auto valid = group();
    motrack::writeArchiveGroup(path, valid);
    const auto storage = motrack::readWorkspaceStorage(path, nullptr);
    const auto metadata = QJsonDocument::fromJson(storage.json).object();
    const auto archiveBytes = readBytes(storage.archive->sourcePath());
    const std::vector<std::function<void(QJsonObject&)>> mutations{
        [](auto& value) { value.insert("format", "Foreign"); },
        [](auto& value) { value.insert("version", 7); },
        [](auto& value) { value.insert("kind", "document"); },
        [](auto& value) { value.remove("groupId"); },
        [](auto& value) { value.insert("groupId", QUuid{}.toString(QUuid::WithoutBraces)); },
        [](auto& value) { value.insert("groupId", "{" + value.value("groupId").toString() + "}"); },
        [](auto& value) { value.insert("sourceDigest", QString(64, '0')); },
        [](auto& value) { value.insert("members", QJsonArray{}); },
        [](auto& value) { value.insert("members", QJsonObject{}); },
        [](auto& value)
        {
            auto members = value.value("members").toArray();
            members.append(members.first());
            value.insert("members", members);
        }};
    for (std::size_t index = 0; index < mutations.size(); ++index)
    {
        SCOPED_TRACE(index);
        auto invalid = metadata;
        mutations[index](invalid);
        ASSERT_TRUE(
            motrack::test::writeArchive(path, {{"project.json", QJsonDocument(invalid).toJson()}, {"tracker/source.trz", archiveBytes}}));
        EXPECT_THROW((void)motrack::readArchiveGroup(path), motrack::ProjectError);
    }
    for (int mutation = 0; mutation < 10; ++mutation)
    {
        SCOPED_TRACE(mutation);
        auto invalid = metadata;
        auto members = invalid.value("members").toArray();
        auto second = members.at(1).toObject();
        auto nested = second.value("document").toObject();
        switch (mutation)
        {
        case 0:
            second.insert("documentId", "ABCDEFAB-CDEF-4ABC-8DEF-ABCDEFABCDEF");
            break;
        case 1:
            second.insert("documentId", members.at(0).toObject().value("documentId"));
            break;
        case 2:
            second.insert("member", 5);
            break;
        case 3:
            second.insert("document", 5);
            break;
        case 4:
            nested.insert("version", 4);
            second.insert("document", nested);
            break;
        case 5:
            nested.insert("kind", "archive-group");
            second.insert("document", nested);
            break;
        case 6:
            nested.insert("embeddedArchive", false);
            second.insert("document", nested);
            break;
        case 7:
            nested.insert("archiveMember", QJsonValue::Null);
            second.insert("document", nested);
            break;
        case 8:
            second = members.at(0).toObject();
            second.insert("documentId", QUuid::createUuid().toString(QUuid::WithoutBraces));
            break;
        default:
            break;
        }
        members.replace(1, mutation == 9 ? QJsonValue(5) : QJsonValue(second));
        invalid.insert("members", members);
        ASSERT_TRUE(
            motrack::test::writeArchive(path, {{"project.json", QJsonDocument(invalid).toJson()}, {"tracker/source.trz", archiveBytes}}));
        EXPECT_THROW((void)motrack::readArchiveGroup(path), motrack::ProjectError);
    }
    ASSERT_TRUE(writeBytes(path, storage.json));
    EXPECT_THROW((void)motrack::readArchiveGroup(path), motrack::ProjectError);
}

} // namespace
