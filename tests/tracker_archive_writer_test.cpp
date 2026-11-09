#include "tracker_archive_writer.hpp"

#include "archive_fixture.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"
#include "tracker_writer.hpp"
#include "workspace_document.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtEndian>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <functional>

namespace
{
QByteArray originalXml()
{
    return "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>"
           "<property name='description' type='string'>Original experiment</property></object>";
}
constexpr const char* EDITED_XML = "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>"
                                   "<property name='description' type='string'>Edited experiment with a longer note</property></object>";

QByteArray readBytes(const QString& path)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
        throw std::runtime_error("Could not read archive test input.");
    return input.readAll();
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile output(path);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size();
}

struct ZipRecord
{
    QByteArray name;
    QByteArray central;
    QByteArray local;
    quint32 offset;
};

// Independently inspect small test ZIPs, including compressed bytes and data descriptors.
std::vector<ZipRecord> zipRecords(const QByteArray& bytes)
{
    const auto end = bytes.lastIndexOf(QByteArray::fromHex("504b0506"));
    if (end < 0 || bytes.size() - end < 22)
        throw std::runtime_error("Missing test ZIP directory.");
    const auto directory = qFromLittleEndian<quint32>(bytes.constData() + end + 16);
    const auto count = qFromLittleEndian<quint16>(bytes.constData() + end + 10);
    qsizetype position = directory;
    std::vector<ZipRecord> records;
    for (unsigned i = 0; i < count; ++i)
    {
        if (position + 46 > end)
            throw std::runtime_error("Truncated test ZIP directory.");
        const auto nameSize = qFromLittleEndian<quint16>(bytes.constData() + position + 28);
        const auto size = qsizetype{46} + nameSize + qFromLittleEndian<quint16>(bytes.constData() + position + 30) +
                          qFromLittleEndian<quint16>(bytes.constData() + position + 32);
        const auto offset = qFromLittleEndian<quint32>(bytes.constData() + position + 42);
        if (position + size > end || offset >= directory)
            throw std::runtime_error("Invalid test ZIP offsets.");
        records.push_back(
            {.name = bytes.mid(position + 46, nameSize), .central = bytes.mid(position, size), .local = {}, .offset = offset});
        position += size;
    }
    for (auto& record : records)
    {
        auto next = directory;
        for (const auto& other : records)
            if (other.offset > record.offset)
                next = std::min(next, other.offset);
        record.local = bytes.mid(record.offset, next - record.offset);
    }
    return records;
}

void expectRetainedRecords(const QByteArray& original, const QByteArray& output, const QStringList& changed)
{
    const auto before = zipRecords(original);
    const auto after = zipRecords(output);
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i)
    {
        SCOPED_TRACE(before[i].name.toStdString());
        EXPECT_EQ(after[i].name, before[i].name);
        if (changed.contains(QString::fromUtf8(before[i].name)))
            continue;
        EXPECT_EQ(after[i].local, before[i].local);
        auto central = after[i].central;
        qToLittleEndian<quint32>(before[i].offset, central.data() + 42);
        EXPECT_EQ(central, before[i].central);
    }
}
TEST(TrackerArchiveWriter, ReplacesMultipleMembersWithIndependentPhysicalAndDirectoryOrder)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    const auto destination = temporary.filePath("saved.trz");
    ASSERT_TRUE(motrack::test::writeArchive(original,
                                            {{.name = "first.trk", .bytes = originalXml()},
                                             {.name = "unchanged.trk", .bytes = originalXml()},
                                             {.name = "nested/second.trk", .bytes = originalXml()},
                                             {.name = "support/data.bin", .bytes = QByteArray::fromHex("00ff017e")}}));
    auto bytes = readBytes(original);
    const auto records = zipRecords(bytes);
    const auto directory = qFromLittleEndian<quint32>(bytes.constData() + bytes.size() - 6);
    QByteArray reversed;
    for (auto entry = records.rbegin(); entry != records.rend(); ++entry)
        reversed += entry->central;
    bytes.replace(directory, reversed.size(), reversed);
    const auto comment = QByteArray::fromHex("4f706171756520636f6d6d656e7400ff");
    qToLittleEndian<quint16>(static_cast<quint16>(comment.size()), bytes.data() + bytes.size() - 2);
    bytes += comment;
    ASSERT_TRUE(writeBytes(original, bytes));
    const auto source = motrack::TrackerArchive::open(original);
    const auto longer = originalXml().toStdString() + "<!--" + std::string(32000, 'x') + "-->";
    const std::string shorter = "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'/>";
    std::array replacements{motrack::TrackerArchiveReplacement{.member = "nested/second.trk", .xml = shorter},
                            motrack::TrackerArchiveReplacement{.member = "first.trk", .xml = longer}};
    motrack::writeTrackerArchive(destination, source, replacements);
    const auto first = readBytes(destination);
    EXPECT_TRUE(first.endsWith(comment));
    expectRetainedRecords(bytes, first, {"first.trk", "nested/second.trk"});
    const auto reopened = motrack::TrackerArchive::open(destination);
    EXPECT_EQ(readBytes(reopened.projectPath("first.trk")).toStdString(), longer);
    EXPECT_EQ(readBytes(reopened.projectPath("nested/second.trk")).toStdString(), shorter);
    EXPECT_EQ(readBytes(reopened.projectPath("unchanged.trk")), originalXml());
    EXPECT_EQ(readBytes(source.sourcePath()), bytes);
    std::ranges::reverse(replacements);
    motrack::writeTrackerArchive(destination, source, replacements);
    EXPECT_EQ(readBytes(destination), first);
}

TEST(TrackerArchiveWriter, MultiMemberExportRetainsUntouchedJavaCompressedRecordsExactly)
{
    const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/multitab-images.trz";
    const auto source = motrack::TrackerArchive::open(fixture);
    const auto original = readBytes(source.sourcePath());
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("saved.trz");
    const auto first = originalXml().toStdString() + "<!--" + std::string(32000, 'y') + "-->";
    const std::array replacements{motrack::TrackerArchiveReplacement{.member = "second.trk", .xml = EDITED_XML},
                                  motrack::TrackerArchiveReplacement{.member = "experiment.trk", .xml = first}};
    motrack::writeTrackerArchive(destination, source, replacements);
    const auto output = readBytes(destination);
    expectRetainedRecords(original, output, {"experiment.trk", "second.trk"});
    const auto end = original.lastIndexOf(QByteArray::fromHex("504b0506"));
    EXPECT_TRUE(output.endsWith(original.mid(end + 22)));
    const auto reopened = motrack::TrackerArchive::open(destination);
    EXPECT_EQ(readBytes(reopened.projectPath("experiment.trk")).toStdString(), first);
    EXPECT_EQ(readBytes(reopened.projectPath("second.trk")).toStdString(), EDITED_XML);
}

TEST(TrackerArchiveWriter, InvalidSecondReplacementCannotPartiallyReplaceDestination)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    const auto destination = temporary.filePath("saved.trz");
    ASSERT_TRUE(motrack::test::writeArchive(
        original, {{.name = "first.trk", .bytes = originalXml()}, {.name = "second.trk", .bytes = originalXml()}}));
    const auto source = motrack::TrackerArchive::open(original);
    ASSERT_TRUE(writeBytes(destination, "Existing archive"));
    const std::vector<std::vector<motrack::TrackerArchiveReplacement>> invalid{
        {},
        {{.member = "first.trk", .xml = EDITED_XML}, {.member = "first.trk", .xml = EDITED_XML}},
        {{.member = "first.trk", .xml = EDITED_XML}, {.member = "missing.trk", .xml = EDITED_XML}},
        {{.member = "first.trk", .xml = EDITED_XML}, {.member = "second.trk", .xml = "invalid XML"}},
        {{.member = "first.trk", .xml = EDITED_XML}, {.member = "second.trk", .xml = ""}},
        {{.member = "first.trk", .xml = EDITED_XML},
         {.member = "second.trk", .xml = EDITED_XML},
         {.member = "third.trk", .xml = EDITED_XML}}};
    for (const auto& replacements : invalid)
    {
        EXPECT_THROW(motrack::writeTrackerArchive(destination, source, replacements), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), "Existing archive");
    }
    const std::array valid{motrack::TrackerArchiveReplacement{.member = "first.trk", .xml = EDITED_XML},
                           motrack::TrackerArchiveReplacement{.member = "second.trk", .xml = EDITED_XML}};
    const std::atomic_bool cancelled{true};
    EXPECT_THROW(motrack::writeTrackerArchive(destination, source, valid, &cancelled), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), "Existing archive");
}

TEST(TrackerArchiveWriter, SavesBothExperimentsAfterSeparateNativeReopenAndOriginalRemoval)
{
    const QDir fixtures(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures");
    for (const bool movie : {false, true})
    {
        SCOPED_TRACE(movie);
        QTemporaryDir temporary;
        const auto source = temporary.filePath("original.trz");
        const auto destination = temporary.filePath(QString::fromUtf8("résultat.trz"));
        const auto fixture = fixtures.filePath(movie ? "linked-mov/archived-movie.trz" : "linked-images/multitab-images.trz");
        ASSERT_TRUE(QFile::copy(fixture, source));
        auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(source));
        const auto original = readBytes(source);
        const QStringList members{"experiment.trk", "second.trk"};
        std::vector<motrack::WorkspaceDocument> documents;
        documents.reserve(static_cast<std::size_t>(members.size()));
        for (int i = 0; i < members.size(); ++i)
        {
            auto document = motrack::importTrackerDocument(payload->projectPath(members[i]));
            payload->resolveMedia(document.project, members[i]);
            document.archivePayload = payload;
            document.archiveMember = members[i].toStdString();
            if (movie && i == 0)
            {
                document.movie = true;
                document.alignMovieToClip = true;
                document.movieFrameTimes = {0.0, 0.125, 0.25, 0.375};
                document.project.clip.sourceFrameCount = 4;
            }
            motrack::TrackEdits edits;
            ASSERT_TRUE(edits.setPointMassValue(document.project, 0, i == 0 ? 5.0 : 7.0));
            ASSERT_TRUE(edits.markPointMass(document.project, 0, {.frame = i + 1, .x = 8.0 + i, .y = 12.0 - i}));
            ASSERT_TRUE(edits.undo(document.project));
            ASSERT_TRUE(edits.redo(document.project));
            motrack::writeWorkspaceDocument(temporary.filePath(QString("saved-%1.motrack").arg(i)), document);
        }
        payload.reset();
        ASSERT_TRUE(QFile::remove(source));
        for (int i = 0; i < members.size(); ++i)
            documents.push_back(motrack::readWorkspaceDocument(temporary.filePath(QString("saved-%1.motrack").arg(i))));
        ASSERT_NE(documents[0].archivePayload, documents[1].archivePayload);
        ASSERT_EQ(documents[0].archivePayload->sourceDigest(), documents[1].archivePayload->sourceDigest());
        motrack::writeTrackerArchiveDocuments(destination, documents);
        const auto first = readBytes(destination);
        expectRetainedRecords(original, first, members);
        const auto reopened = motrack::TrackerArchive::open(destination);
        for (std::size_t i = 0; i < documents.size(); ++i)
        {
            const auto loaded = motrack::importTrackerDocument(reopened.projectPath(members[static_cast<qsizetype>(i)]));
            ASSERT_EQ(loaded.project.pointMasses.size(), 1);
            EXPECT_DOUBLE_EQ(loaded.project.pointMasses.front().mass, i == 0 ? 5.0 : 7.0);
            EXPECT_EQ(loaded.project.pointMasses.front().marks, documents[i].project.pointMasses.front().marks);
            EXPECT_EQ(readBytes(documents[i].archivePayload->sourcePath()), original);
        }
        std::ranges::reverse(documents);
        motrack::writeTrackerArchiveDocuments(destination, documents);
        EXPECT_EQ(readBytes(destination), first);
    }
}

TEST(TrackerArchiveWriter, RefusesAnInvalidSecondDocumentWithoutSavingTheFirst)
{
    QTemporaryDir temporary;
    const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/multitab-images.trz";
    const auto destination = temporary.filePath("existing.trz");
    std::vector<motrack::WorkspaceDocument> documents;
    for (const auto* member : {"experiment.trk", "second.trk"})
    {
        auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(fixture));
        auto document = motrack::importTrackerDocument(payload->projectPath(member));
        payload->resolveMedia(document.project, member);
        document.archivePayload = payload;
        document.archiveMember = member;
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 6.0));
        documents.push_back(std::move(document));
    }
    motrack::writeTrackerArchiveDocuments(destination, documents);
    const auto originalOutput = readBytes(destination);
    const auto foreignPath = temporary.filePath("foreign.trz");
    ASSERT_TRUE(motrack::test::writeArchive(
        foreignPath, {{.name = "experiment.trk", .bytes = originalXml()}, {.name = "second.trk", .bytes = originalXml()}}));
    const auto foreign = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(foreignPath));
    const std::vector<std::function<void(motrack::WorkspaceDocument&)>> mutations{
        [](auto& document) { document.archivePayload.reset(); },
        [](auto& document) { document.archiveMember.reset(); },
        [](auto& document) { document.trackerSource.reset(); },
        [](auto& document) { document.archiveMember = "experiment.trk"; },
        [](auto& document) { document.archiveMember = "missing.trk"; },
        [](auto& document)
        {
            if (!document.trackerSource)
                throw std::runtime_error("Missing test source.");
            document.trackerSource->xml += " ";
        },
        [](auto& document) { document.archiveReference = "stale.trz"; },
        [](auto& document) { document.project.clip.mediaReferences.front() = "outside.png"; },
        [foreign](auto& document)
        {
            document.archivePayload = foreign;
        }};
    for (std::size_t i = 0; i < mutations.size(); ++i)
    {
        SCOPED_TRACE(i);
        auto invalid = documents;
        mutations[i](invalid[1]);
        EXPECT_THROW(motrack::writeTrackerArchiveDocuments(destination, invalid), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), originalOutput);
    }
    EXPECT_THROW(motrack::writeTrackerArchiveDocuments(destination, {}), motrack::ProjectError);
    const std::atomic_bool cancelled{true};
    EXPECT_THROW(motrack::writeTrackerArchiveDocuments(destination, documents, &cancelled), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), originalOutput);
    for (const auto& document : documents)
    {
        const auto owned = document.archivePayload->sourcePath();
        const auto original = readBytes(owned);
        EXPECT_THROW(motrack::writeTrackerArchiveDocuments(owned, documents), motrack::ProjectError);
        EXPECT_EQ(readBytes(owned), original);
        EXPECT_THROW(motrack::writeTrackerArchiveDocuments(QDir(document.archivePayload->root()).filePath("saved.trz"), documents),
                     motrack::ProjectError);
    }
    const auto& second = documents[1];
    const auto media = QDir(second.archivePayload->root()).filePath(QString::fromStdString(second.project.clip.mediaReferences.front()));
    ASSERT_TRUE(writeBytes(media, "changed media"));
    EXPECT_THROW(motrack::writeTrackerArchiveDocuments(destination, documents), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), originalOutput);
}

TEST(TrackerArchiveWriter, ReplacesOnlyTheChosenMemberAndKeepsTheOwnedSource)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    const auto destination = temporary.filePath(QString::fromUtf8("edited élève.trz"));
    const QByteArray opaque(150000, '\xA5');
    for (const auto* selected : {"first.trk", "nested/second.trk", "last.trk"})
    {
        SCOPED_TRACE(selected);
        ASSERT_TRUE(motrack::test::writeArchive(original,
                                                {{.name = "first.trk", .bytes = originalXml()},
                                                 {.name = "nested/second.trk", .bytes = originalXml()},
                                                 {.name = "media/opaque.bin", .bytes = opaque},
                                                 {.name = "last.trk", .bytes = originalXml()}}));
        auto bytes = readBytes(original);
        // Archive comments are opaque metadata. libarchive's ordinary writer does not expose
        // them, so independently append one to its otherwise unchanged classic ZIP output.
        const auto comment = QByteArray::fromHex("54656163686572206e6f74657300ff");
        ASSERT_EQ(bytes.right(22).left(4), QByteArray::fromHex("504b0506"));
        qToLittleEndian<quint16>(static_cast<quint16>(comment.size()), bytes.data() + bytes.size() - 2);
        bytes += comment;
        ASSERT_TRUE(writeBytes(original, bytes));
        const auto source = motrack::TrackerArchive::open(original);
        ASSERT_TRUE(QFile::remove(original));
        motrack::writeTrackerArchive(destination, source, selected, EDITED_XML);
        const auto firstOutput = readBytes(destination);
        EXPECT_TRUE(firstOutput.endsWith(comment));
        EXPECT_EQ(readBytes(source.sourcePath()), bytes);
        EXPECT_EQ(readBytes(source.projectPath(selected)), originalXml());
        const auto reopened = motrack::TrackerArchive::open(destination);
        EXPECT_EQ(reopened.projects(), source.projects());
        for (const auto& member : source.projects())
            EXPECT_EQ(readBytes(reopened.projectPath(member)), member == selected ? QByteArray::fromStdString(EDITED_XML) : originalXml());
        EXPECT_EQ(readBytes(QDir(reopened.root()).filePath("media/opaque.bin")), opaque);
        motrack::writeTrackerArchive(destination, source, selected, EDITED_XML);
        EXPECT_EQ(readBytes(destination), firstOutput);
        motrack::writeTrackerArchive(destination, reopened, selected, originalXml().toStdString());
        const auto restored = motrack::TrackerArchive::open(destination);
        EXPECT_EQ(readBytes(restored.projectPath(selected)), originalXml());
    }
}

TEST(TrackerArchiveWriter, RefusesAValidReplacementOfTheOwnedSourceInBothFormats)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    const auto destination = temporary.filePath("saved.trz");
    const auto native = temporary.filePath("saved.motrack");
    const QDir fixtures(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images");
    const auto sourceXml = readBytes(fixtures.filePath("linked-images.trk"));
    std::vector<motrack::test::ArchiveEntry> members{
        {.name = "selected.trk", .bytes = sourceXml},
        {.name = "images/frame0.png", .bytes = readBytes(fixtures.filePath("images/frame0.png"))},
        {.name = "images/frame1.png", .bytes = readBytes(fixtures.filePath("images/frame1.png"))},
        {.name = "images/frame2.png", .bytes = readBytes(fixtures.filePath("images/frame2.png"))},
        {.name = "images/frame3.png", .bytes = readBytes(fixtures.filePath("images/frame3.png"))}};
    ASSERT_TRUE(motrack::test::writeArchive(original, members));
    auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(original));
    auto document = motrack::importTrackerDocument(payload->projectPath("selected.trk"));
    payload->resolveMedia(document.project, "selected.trk");
    document.archivePayload = payload;
    document.archiveMember = "selected.trk";
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5));
    motrack::writeTrackerArchiveDocument(destination, document);
    motrack::writeWorkspaceDocument(native, document);
    const auto standardBytes = readBytes(destination);
    const auto nativeBytes = readBytes(native);
    const auto originalBytes = readBytes(payload->sourcePath());
    const auto originalDigest = payload->sourceDigest();
    members[1].bytes = members[2].bytes;
    ASSERT_TRUE(motrack::test::writeArchive(payload->sourcePath(), members));
    const auto replacement = motrack::TrackerArchive::open(payload->sourcePath());
    ASSERT_EQ(replacement.projects(), payload->projects());
    ASSERT_EQ(readBytes(replacement.projectPath("selected.trk")), sourceXml);
    ASSERT_NE(replacement.sourceDigest(), originalDigest);
    ASSERT_TRUE(payload->memberMatchesSource("images/frame0.png"));
    EXPECT_THROW(motrack::writeTrackerArchiveDocument(destination, document), motrack::ProjectError);
    EXPECT_THROW(motrack::writeWorkspaceDocument(native, document), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), standardBytes);
    EXPECT_EQ(readBytes(native), nativeBytes);
    EXPECT_EQ(payload->sourceDigest(), originalDigest);
    ASSERT_TRUE(writeBytes(payload->sourcePath(), originalBytes));
    EXPECT_NO_THROW(motrack::writeTrackerArchiveDocument(destination, document));
    EXPECT_NO_THROW(motrack::writeWorkspaceDocument(native, document));
}

TEST(TrackerArchiveWriter, RefusesArchivedAviThatPinnedTrackerCannotExtractAfterNativeReopen)
{
    QTemporaryDir temporary;
    const auto source = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-movie/archived-movie.trz";
    const auto destination = temporary.filePath("existing.trz");
    const auto native = temporary.filePath("saved.motrack");
    auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(source));
    auto document = motrack::importTrackerDocument(payload->projectPath("experiment.trk"));
    payload->resolveMedia(document.project, "experiment.trk");
    document.archivePayload = payload;
    document.archiveMember = "experiment.trk";
    document.movie = true;
    document.alignMovieToClip = true;
    document.movieFrameTimes = {0.0, 0.125, 0.25, 0.375};
    document.project.clip.sourceFrameCount = 4;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5));
    for (const bool reopen : {false, true})
    {
        SCOPED_TRACE(reopen);
        if (reopen)
        {
            motrack::writeWorkspaceDocument(native, document);
            document = motrack::readWorkspaceDocument(native);
        }
        ASSERT_TRUE(writeBytes(destination, "existing archive"));
        EXPECT_THROW(motrack::writeTrackerArchiveDocument(destination, document), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), "existing archive");
    }
}

TEST(TrackerArchiveWriter, SavesIndexedMovieEditsAfterNativeReopenWithoutChangingOtherMembers)
{
    const QDir fixtures(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-mov");
    const auto movie = readBytes(fixtures.filePath("constant-rate.mov"));
    const auto originalXml = readBytes(fixtures.filePath("linked-movie.trk"));
    for (const auto* selected : {"selected.trk", "nested/selected.trk"})
    {
        SCOPED_TRACE(selected);
        QTemporaryDir temporary;
        const auto source = temporary.filePath("movie.trz");
        const auto destination = temporary.filePath("saved.trz");
        const auto native = temporary.filePath("movie.motrack");
        auto selectedXml = originalXml;
        if (QString::fromUtf8(selected).startsWith("nested/"))
            selectedXml.replace(">constant-rate.mov<", ">../constant-rate.mov<");
        ASSERT_TRUE(motrack::test::writeArchive(source,
                                                {{.name = selected, .bytes = selectedXml},
                                                 {.name = "other.trk", .bytes = originalXml},
                                                 {.name = "constant-rate.mov", .bytes = movie},
                                                 {.name = "support/opaque.bin", .bytes = QByteArray::fromHex("00ff0180")}}));
        auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(source));
        auto document = motrack::importTrackerDocument(payload->projectPath(selected));
        payload->resolveMedia(document.project, selected);
        document.archivePayload = payload;
        document.archiveMember = selected;
        document.movie = true;
        document.alignMovieToClip = true;
        document.movieFrameTimes = {0.0, 0.125, 0.25, 0.375};
        document.project.clip.sourceFrameCount = 4;
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5));
        ASSERT_TRUE(edits.markPointMass(document.project, 0, {.frame = 1, .x = 8.0, .y = 12.0}));
        ASSERT_TRUE(edits.setClipRange(document.project, 1, 3, 2));
        ASSERT_TRUE(edits.undo(document.project));
        ASSERT_TRUE(edits.redo(document.project));
        const auto original = readBytes(payload->sourcePath());
        for (const bool reopen : {false, true})
        {
            SCOPED_TRACE(reopen);
            if (reopen)
            {
                motrack::writeWorkspaceDocument(native, document);
                ASSERT_TRUE(QFile::remove(source));
                document = motrack::readWorkspaceDocument(native);
            }
            ASSERT_TRUE(document.movie);
            ASSERT_EQ(document.movieFrameTimes.size(), 4);
            motrack::writeTrackerArchiveDocument(destination, document);
            const auto output = motrack::TrackerArchive::open(destination);
            const auto exported = motrack::importTrackerDocument(output.projectPath(selected));
            EXPECT_EQ(exported.project.pointMasses, document.project.pointMasses);
            EXPECT_EQ(exported.project.clip.firstFrame, 1);
            EXPECT_EQ(exported.project.clip.stepSize, 2);
            EXPECT_EQ(exported.project.clip.stepCount, 2);
            EXPECT_EQ(readBytes(output.projectPath("other.trk")), originalXml);
            EXPECT_EQ(readBytes(QDir(output.root()).filePath("constant-rate.mov")), movie);
            EXPECT_EQ(readBytes(QDir(output.root()).filePath("support/opaque.bin")), QByteArray::fromHex("00ff0180"));
            ASSERT_TRUE(document.archivePayload);
            EXPECT_EQ(readBytes(document.archivePayload->sourcePath()), original);
        }
        const auto savedBytes = readBytes(destination);
        document.movieFrameTimes.pop_back();
        EXPECT_THROW(motrack::writeTrackerArchiveDocument(destination, document), motrack::ProjectError);
        document.movieFrameTimes.push_back(0.375);
        document.alignMovieToClip = false;
        EXPECT_THROW(motrack::writeTrackerArchiveDocument(destination, document), motrack::ProjectError);
        document.alignMovieToClip = true;
        const std::atomic_bool cancelled{true};
        EXPECT_THROW(motrack::writeTrackerArchiveDocument(destination, document, &cancelled), motrack::ProjectError);
        ASSERT_TRUE(document.archivePayload);
        const auto extractedMovie = QDir(document.archivePayload->root()).filePath("constant-rate.mov");
        ASSERT_TRUE(writeBytes(extractedMovie, "replacement movie"));
        EXPECT_THROW(motrack::writeTrackerArchiveDocument(destination, document), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), savedBytes);
        ASSERT_TRUE(writeBytes(extractedMovie, movie));
        EXPECT_NO_THROW(motrack::writeTrackerArchiveDocument(destination, document));
        EXPECT_EQ(readBytes(destination), savedBytes);
    }
}

TEST(TrackerArchiveWriter, PreservesExistingDestinationOnInvalidInputAndCancellation)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    const auto destination = temporary.filePath("existing.trz");
    ASSERT_TRUE(motrack::test::writeArchive(original, {{.name = "selected.trk", .bytes = originalXml()}}));
    const auto source = motrack::TrackerArchive::open(original);
    ASSERT_TRUE(writeBytes(destination, "existing output"));
    for (const auto* member : {"", "missing.trk", "../selected.trk"})
        EXPECT_THROW(motrack::writeTrackerArchive(destination, source, member, EDITED_XML), motrack::ProjectError);
    for (const auto* xml : {"", "not XML", "<object class='unknown'/>"})
        EXPECT_THROW(motrack::writeTrackerArchive(destination, source, "selected.trk", xml), motrack::ProjectError);
    const std::atomic_bool cancelled{true};
    EXPECT_THROW(motrack::writeTrackerArchive(destination, source, "selected.trk", EDITED_XML, &cancelled), motrack::ProjectError);
    EXPECT_THROW(motrack::writeTrackerArchive(temporary.filePath("wrong.zip"), source, "selected.trk", EDITED_XML), motrack::ProjectError);
    EXPECT_THROW(motrack::writeTrackerArchive(temporary.filePath("missing/parent.trz"), source, "selected.trk", EDITED_XML),
                 motrack::ProjectError);
    EXPECT_THROW(motrack::writeTrackerArchive(source.sourcePath(), source, "selected.trk", EDITED_XML), motrack::ProjectError);
    EXPECT_THROW(motrack::writeTrackerArchive(QDir(source.root()).filePath("invalid.trz"), source, "selected.trk", EDITED_XML),
                 motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), "existing output");
    EXPECT_EQ(readBytes(source.projectPath("selected.trk")), originalXml());
}

TEST(TrackerArchiveWriter, RefusesReaderAcceptedButWriterUnsupportedZipLayouts)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    const auto destination = temporary.filePath("existing.trz");
    ASSERT_TRUE(motrack::test::writeArchive(original, {{.name = "selected.trk", .bytes = originalXml()}}));
    const auto valid = readBytes(original);
    const auto central = qFromLittleEndian<quint32>(valid.constData() + valid.size() - 6);
    ASSERT_TRUE(writeBytes(destination, "existing output"));
    for (const int kind : {0, 1, 2, 3})
    {
        SCOPED_TRACE(kind);
        auto unsupported = valid;
        if (kind == 0)
        {
            qToLittleEndian<quint16>(45, unsupported.data() + 4);
            qToLittleEndian<quint16>(45, unsupported.data() + central + 6);
        }
        else if (kind == 1)
        {
            const auto extra = central + 46 + qFromLittleEndian<quint16>(unsupported.constData() + central + 28);
            const auto extraSize = qFromLittleEndian<quint16>(unsupported.constData() + central + 30);
            qToLittleEndian<quint16>(extraSize + 4, unsupported.data() + central + 30);
            unsupported.insert(extra, QByteArray::fromHex("34120000"));
            const auto directorySize = qFromLittleEndian<quint32>(unsupported.constData() + unsupported.size() - 10);
            qToLittleEndian<quint32>(directorySize + 4, unsupported.data() + unsupported.size() - 10);
        }
        else if (kind == 2)
        {
            const auto extra = 30 + qFromLittleEndian<quint16>(unsupported.constData() + 26);
            const auto extraSize = qFromLittleEndian<quint16>(unsupported.constData() + 28);
            qToLittleEndian<quint16>(extraSize + 4, unsupported.data() + 28);
            unsupported.insert(extra, QByteArray::fromHex("34120000"));
            qToLittleEndian<quint32>(central + 4, unsupported.data() + unsupported.size() - 6);
        }
        else
        {
            // A harmless self-extracting-style prefix is readable, but outside the writer's
            // contiguous ZIP-record contract. Rebase both offsets before the immutable import.
            constexpr quint32 PREFIX_SIZE = 4;
            qToLittleEndian<quint32>(PREFIX_SIZE, unsupported.data() + central + 42);
            qToLittleEndian<quint32>(central + PREFIX_SIZE, unsupported.data() + unsupported.size() - 6);
            unsupported.prepend("stub");
        }
        ASSERT_TRUE(writeBytes(original, unsupported));
        const auto source = motrack::TrackerArchive::open(original);
        ASSERT_EQ(readBytes(source.projectPath("selected.trk")), originalXml());
        ASSERT_EQ(readBytes(source.sourcePath()), unsupported);
        EXPECT_THROW(motrack::writeTrackerArchive(destination, source, "selected.trk", EDITED_XML), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), "existing output");
        EXPECT_EQ(readBytes(source.sourcePath()), unsupported);
    }
}

TEST(TrackerArchiveWriter, RefusesChangedSourceBytesBeforeReplacement)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    const auto destination = temporary.filePath("existing.trz");
    ASSERT_TRUE(motrack::test::writeArchive(original, {{.name = "selected.trk", .bytes = originalXml()}}));
    const auto source = motrack::TrackerArchive::open(original);
    const auto valid = readBytes(source.sourcePath());
    const auto central = qFromLittleEndian<quint32>(valid.constData() + valid.size() - 6);
    ASSERT_TRUE(writeBytes(destination, "existing output"));
    const std::vector<std::function<void(QByteArray&)>> mutations{
        [](auto& bytes) { bytes.truncate(10); },
        [](auto& bytes) { bytes[0] = '!'; },
        [](auto& bytes) { qToLittleEndian<quint16>(1, bytes.data() + bytes.size() - 18); },
        [](auto& bytes) { qToLittleEndian<quint16>(65535, bytes.data() + bytes.size() - 12); },
        [central](auto& bytes) { qToLittleEndian<quint16>(45, bytes.data() + central + 6); },
        [central](auto& bytes) { qToLittleEndian<quint16>(1, bytes.data() + central + 8); },
        [central](auto& bytes) { qToLittleEndian<quint16>(99, bytes.data() + central + 10); },
        [central](auto& bytes) { qToLittleEndian<quint32>(0xffffffff, bytes.data() + central + 20); },
        [central](auto& bytes) { qToLittleEndian<quint32>(1, bytes.data() + central + 42); },
        [central](auto& bytes) { bytes[central + 46] = '!'; },
        [central](auto& bytes)
        {
            const auto extra = central + 46 + qFromLittleEndian<quint16>(bytes.constData() + central + 28);
            qToLittleEndian<quint16>(0x1234, bytes.data() + extra);
        }};
    for (std::size_t i = 0; i < mutations.size(); ++i)
    {
        SCOPED_TRACE(i);
        auto corrupted = valid;
        mutations[i](corrupted);
        ASSERT_TRUE(writeBytes(source.sourcePath(), corrupted));
        EXPECT_THROW(motrack::writeTrackerArchive(destination, source, "selected.trk", EDITED_XML), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), "existing output");
    }
}

TEST(TrackerArchiveWriter, ExportsAnEditedImageDocumentAfterNativeReopenAndOriginalRemoval)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    const auto native = temporary.filePath("saved.motrack");
    const auto destination = temporary.filePath("edited.trz");
    const QDir fixtures(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images");
    const auto sourceXml = readBytes(fixtures.filePath("linked-images.trk"));
    ASSERT_TRUE(
        motrack::test::writeArchive(original,
                                    {{.name = "nested/selected.trk", .bytes = sourceXml},
                                     {.name = "other.trk", .bytes = originalXml()},
                                     {.name = "nested/images/frame0.png", .bytes = readBytes(fixtures.filePath("images/frame0.png"))},
                                     {.name = "nested/images/frame1.png", .bytes = readBytes(fixtures.filePath("images/frame1.png"))},
                                     {.name = "nested/images/frame2.png", .bytes = readBytes(fixtures.filePath("images/frame2.png"))},
                                     {.name = "nested/images/frame3.png", .bytes = readBytes(fixtures.filePath("images/frame3.png"))},
                                     {.name = "notes.txt", .bytes = "Untouched supporting document"}}));
    const auto originalBytes = readBytes(original);
    auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(original));
    auto document = motrack::importTrackerDocument(payload->projectPath("nested/selected.trk"));
    payload->resolveMedia(document.project, "nested/selected.trk");
    document.archivePayload = payload;
    document.archiveMember = "nested/selected.trk";
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5));
    ASSERT_TRUE(edits.markPointMass(document.project, 0, {.frame = 1, .x = 8, .y = 12}));
    ASSERT_TRUE(edits.setClipRange(document.project, 1, 3, 2));
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    motrack::writeWorkspaceDocument(native, document);
    document = {};
    payload.reset();
    ASSERT_TRUE(QFile::remove(original));
    document = motrack::readWorkspaceDocument(native);
    ASSERT_TRUE(document.archivePayload);
    motrack::writeTrackerArchiveDocument(destination, document);
    const auto firstOutput = readBytes(destination);
    const auto reopened = motrack::TrackerArchive::open(destination);
    const auto edited = motrack::importTrackerDocument(reopened.projectPath("nested/selected.trk"));
    ASSERT_EQ(edited.project.pointMasses.size(), 1);
    EXPECT_DOUBLE_EQ(edited.project.pointMasses.front().mass, 5);
    EXPECT_EQ(edited.project.clip.firstFrame, 1);
    EXPECT_EQ(edited.project.clip.stepSize, 2);
    EXPECT_EQ(edited.project.clip.stepCount, 2);
    EXPECT_EQ(edited.project.pointMasses.front().marks, document.project.pointMasses.front().marks);
    EXPECT_EQ(edited.project.clip.mediaReferences,
              (std::vector<std::string>{"images/frame0.png", "images/frame1.png", "images/frame2.png", "images/frame3.png"}));
    EXPECT_EQ(readBytes(reopened.projectPath("other.trk")), originalXml());
    EXPECT_EQ(readBytes(QDir(reopened.root()).filePath("notes.txt")), "Untouched supporting document");
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(readBytes(QDir(reopened.root()).filePath(QString("nested/images/frame%1.png").arg(i))),
                  readBytes(fixtures.filePath(QString("images/frame%1.png").arg(i))));
    EXPECT_EQ(readBytes(document.archivePayload->sourcePath()), originalBytes);
    if (!document.trackerSource)
        FAIL() << "Archive export removed the retained source envelope.";
    EXPECT_EQ(document.trackerSource->xml, sourceXml.toStdString());
    motrack::writeTrackerArchiveDocument(destination, document);
    EXPECT_EQ(readBytes(destination), firstOutput);

    const std::vector<std::function<void(motrack::WorkspaceDocument&)>> mutations{
        [](auto& value) { value.archivePayload.reset(); },
        [](auto& value) { value.archiveMember.reset(); },
        [](auto& value) { value.trackerSource.reset(); },
        [](auto& value) { value.archiveMember = "other.trk"; },
        [](auto& value) { value.archiveReference = "unavailable.trz"; },
        [](auto& value) { value.project.clip.mediaReferences.front() = "outside.png"; },
        [](auto& value) { value.project.clip.mediaReferences.clear(); },
        [](auto& value)
        {
            value.movie = true;
        }};
    for (std::size_t i = 0; i < mutations.size(); ++i)
    {
        SCOPED_TRACE(i);
        auto invalid = document;
        mutations[i](invalid);
        EXPECT_THROW(motrack::writeTrackerArchiveDocument(destination, invalid), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), firstOutput);
    }
    // A replacement can still be a valid image at the same member path. Reject it rather than
    // copying the original compressed pixels while pretending to save the displayed replacement.
    const auto media = QDir(document.archivePayload->root()).filePath("nested/images/frame0.png");
    const auto originalImage = readBytes(media);
    ASSERT_TRUE(writeBytes(media, readBytes(fixtures.filePath("images/frame1.png"))));
    EXPECT_THROW(motrack::writeTrackerArchiveDocument(destination, document), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), firstOutput);
    EXPECT_EQ(readBytes(document.archivePayload->sourcePath()), originalBytes);
    ASSERT_TRUE(QFile::remove(media));
    EXPECT_THROW(motrack::writeTrackerArchiveDocument(destination, document), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), firstOutput);
    ASSERT_TRUE(writeBytes(media, originalImage));
    motrack::writeTrackerArchiveDocument(destination, document);
    EXPECT_EQ(readBytes(destination), firstOutput);

    const std::atomic_bool cancelled{true};
    EXPECT_THROW(motrack::writeTrackerArchiveDocument(destination, document, &cancelled), motrack::ProjectError);
    EXPECT_EQ(readBytes(destination), firstOutput);
}

#ifndef Q_OS_WIN
TEST(TrackerArchiveWriter, RejectsOwnedSourceFileAliasesBeforeStandardOrNativeReplacement)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    ASSERT_TRUE(motrack::test::writeArchive(original, {{.name = "selected.trk", .bytes = originalXml()}}));
    auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(original));
    auto document = motrack::importTrackerDocument(payload->projectPath("selected.trk"));
    document.archivePayload = payload;
    document.archiveMember = "selected.trk";
    const auto native = temporary.filePath("saved.motrack");
    motrack::writeWorkspaceDocument(native, document);
    const auto nativeBytes = readBytes(native);
    const auto originalBytes = readBytes(payload->sourcePath());
    const auto external = temporary.filePath("external.trz");
    ASSERT_TRUE(writeBytes(external, originalBytes));
    ASSERT_TRUE(QFile::remove(payload->sourcePath()));
    ASSERT_TRUE(QFile::link(external, payload->sourcePath()));
    ASSERT_TRUE(QFileInfo(payload->sourcePath()).isSymLink());
    EXPECT_THROW(motrack::writeTrackerArchive(external, *payload, "selected.trk", EDITED_XML), motrack::ProjectError);
    EXPECT_THROW(motrack::writeWorkspaceDocument(native, document), motrack::ProjectError);
    EXPECT_EQ(readBytes(external), originalBytes);
    EXPECT_EQ(readBytes(payload->sourcePath()), originalBytes);
    EXPECT_EQ(readBytes(native), nativeBytes);
    ASSERT_TRUE(QFile::remove(payload->sourcePath()));
    ASSERT_TRUE(writeBytes(payload->sourcePath(), originalBytes));
    motrack::writeTrackerArchive(external, *payload, "selected.trk", EDITED_XML);
    motrack::writeWorkspaceDocument(native, document);
    EXPECT_EQ(readBytes(payload->sourcePath()), originalBytes);
    EXPECT_EQ(readBytes(motrack::TrackerArchive::open(external).projectPath("selected.trk")), QByteArray(EDITED_XML));
}

TEST(TrackerArchiveWriter, ProtectsCanonicalOwnedPathsAndRejectsDanglingDestinationAliases)
{
    QTemporaryDir temporary;
    const auto physical = temporary.filePath("physical-temp");
    const auto alias = temporary.filePath("alias-temp");
    ASSERT_TRUE(QDir().mkpath(physical));
    ASSERT_TRUE(QFile::link(physical, alias));
    const auto original = temporary.filePath("input.trz");
    ASSERT_TRUE(motrack::test::writeArchive(original, {{.name = "selected.trk", .bytes = originalXml()}}));
    const bool hadTemp = qEnvironmentVariableIsSet("TMPDIR");
    const auto previousTemp = qgetenv("TMPDIR");
    const auto restoreTemp = qScopeGuard(
        [hadTemp, previousTemp]
        {
            if (hadTemp)
                qputenv("TMPDIR", previousTemp);
            else
                qunsetenv("TMPDIR");
        });
    ASSERT_TRUE(qputenv("TMPDIR", alias.toUtf8()));
    const auto source = motrack::TrackerArchive::open(original);
    const auto bytes = readBytes(source.sourcePath());
    // QTemporaryDir canonicalizes TMPDIR on supported Qt builds. Introduce an alias after
    // importing so the retained spelling and physical root deliberately differ everywhere.
    const auto retainedDirectory = QFileInfo(source.sourcePath()).absolutePath();
    const auto movedDirectory = temporary.filePath("relocated-owned-source");
    ASSERT_TRUE(QDir().rename(retainedDirectory, movedDirectory));
    const auto restoreDirectory = qScopeGuard(
        [&retainedDirectory, &movedDirectory]
        {
            if (QFileInfo(retainedDirectory).isSymLink())
                QFile::remove(retainedDirectory);
            QDir().rename(movedDirectory, retainedDirectory);
        });
    ASSERT_TRUE(QFile::link(movedDirectory, retainedDirectory));
    const auto canonical = QFileInfo(source.sourcePath()).canonicalFilePath();
    ASSERT_NE(source.sourcePath(), canonical);
    EXPECT_THROW(motrack::writeTrackerArchive(canonical, source, "selected.trk", EDITED_XML), motrack::ProjectError);
    EXPECT_EQ(readBytes(source.sourcePath()), bytes);
    const auto extracted = QDir(source.root()).filePath("new-output.trz");
    const auto dangling = temporary.filePath("dangling-output.trz");
    ASSERT_TRUE(QFile::link(extracted, dangling));
    EXPECT_THROW(motrack::writeTrackerArchive(dangling, source, "selected.trk", EDITED_XML), motrack::ProjectError);
    EXPECT_FALSE(QFileInfo::exists(extracted));
    EXPECT_TRUE(QFileInfo(dangling).isSymLink());
    EXPECT_EQ(readBytes(source.sourcePath()), bytes);
}

TEST(TrackerArchiveWriter, RejectsAliasesThatRedirectOriginalArchiveMembers)
{
    const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/archived-images.trz";
    for (const int kind : {0, 1, 2, 3})
    {
        SCOPED_TRACE(kind); // External image file/directory/project, then an in-root renamed image directory.
        QTemporaryDir temporary;
        auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(fixture));
        auto document = motrack::importTrackerDocument(payload->projectPath("experiment.trk"));
        payload->resolveMedia(document.project, "experiment.trk");
        document.archivePayload = payload;
        document.archiveMember = "experiment.trk";
        document.project.pointMasses.front().mass = 5;
        const auto destination = temporary.filePath("edited.trz");
        motrack::writeTrackerArchiveDocument(destination, document);
        const auto valid = readBytes(destination);
        const auto original = readBytes(payload->sourcePath());
        const auto member = kind == 2 ? QString("experiment.trk") : QString("images/frame0.png");
        const auto extracted = QDir(payload->root()).filePath(member);
        ASSERT_TRUE(payload->memberMatchesSource(member));
        if (kind == 1)
        {
            const auto outside = temporary.filePath("external-images");
            ASSERT_TRUE(QDir().mkpath(outside));
            for (int i = 0; i < 4; ++i)
            {
                const auto name = QString("frame%1.png").arg(i);
                ASSERT_TRUE(QFile::copy(QDir(payload->root()).filePath("images/" + name), outside + '/' + name));
            }
            QDir root(payload->root());
            ASSERT_TRUE(root.rename("images", "original-images"));
            ASSERT_TRUE(QFile::link(outside, root.filePath("images")));
        }
        else if (kind == 3)
        {
            QDir root(payload->root());
            ASSERT_TRUE(root.rename("images", "original-images"));
            ASSERT_TRUE(QFile::link(root.filePath("original-images"), root.filePath("images")));
        }
        else
        {
            const auto outside = temporary.filePath(kind == 2 ? "external.trk" : "external.png");
            ASSERT_TRUE(QFile::copy(extracted, outside));
            ASSERT_TRUE(QFile::remove(extracted));
            ASSERT_TRUE(QFile::link(outside, extracted));
        }
        EXPECT_EQ(payload->memberMatchesSource(member), kind == 3);
        EXPECT_THROW(motrack::writeTrackerArchiveDocument(destination, document), motrack::ProjectError);
        EXPECT_EQ(readBytes(destination), valid);
        EXPECT_EQ(readBytes(payload->sourcePath()), original);
    }
}
#endif

TEST(TrackerArchiveWriter, PreservesJavaDeflatedMembersUnicodeNamesAndComments)
{
    const auto fixture = QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/archived-images.trz";
    auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(fixture));
    auto document = motrack::importTrackerDocument(payload->projectPath("experiment.trk"));
    payload->resolveMedia(document.project, "experiment.trk");
    document.archivePayload = payload;
    document.archiveMember = "experiment.trk";
    const auto input = readBytes(payload->sourcePath());
    document.project.pointMasses.front().mass = 5;
    QTemporaryDir temporary;
    const auto destination = temporary.filePath("java-deflated.trz");
    motrack::writeTrackerArchiveDocument(destination, document);
    const auto output = readBytes(destination);
    EXPECT_TRUE(output.endsWith("MoTrack archive-preservation reference; comments must survive."));
    const auto reopened = motrack::TrackerArchive::open(destination);
    for (const auto& name : {QString("images/frame0.png"),
                             QString("images/frame1.png"),
                             QString("images/frame2.png"),
                             QString("images/frame3.png"),
                             QString::fromUtf8("support/notes é.txt"),
                             QString("support/opaque.bin")})
    {
        EXPECT_EQ(readBytes(QDir(reopened.root()).filePath(name)), readBytes(QDir(payload->root()).filePath(name)));
        EXPECT_TRUE(output.contains("Original metadata: " + name.toUtf8()));
    }
    EXPECT_TRUE(output.contains("Original metadata: experiment.trk"));
    EXPECT_EQ(readBytes(payload->sourcePath()), input);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(reopened.projectPath("experiment.trk")).project.pointMasses.front().mass, 5);
}
} // namespace
