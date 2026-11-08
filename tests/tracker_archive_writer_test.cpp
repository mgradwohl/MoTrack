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
