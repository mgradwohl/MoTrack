#include "tracker_archive.hpp"

#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <clocale>
#include <initializer_list>
#include <memory>
#include <string>
#include <tuple>

#include <archive.h>
#include <archive_entry.h>

#ifdef __APPLE__
#include <xlocale.h>
#endif

namespace
{

QString fixturePath(const QString& name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + '/' + name;
}

void writeArchive(const QString& path, std::initializer_list<const char*> names)
{
    const std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), &archive_write_free);
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(archive_write_set_format_zip(writer.get()), ARCHIVE_OK);
    std::array<char, 8192> buffer{};
    std::size_t used = 0;
    ASSERT_EQ(archive_write_open_memory(writer.get(), buffer.data(), buffer.size(), &used), ARCHIVE_OK);
    for (const auto* name : names)
    {
        const std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), &archive_entry_free);
        ASSERT_NE(entry, nullptr);
        archive_entry_set_pathname_utf8(entry.get(), name);
        archive_entry_set_filetype(entry.get(), AE_IFREG);
        archive_entry_set_perm(entry.get(), 0644);
        archive_entry_set_size(entry.get(), 1);
        ASSERT_EQ(archive_write_header(writer.get(), entry.get()), ARCHIVE_OK);
        ASSERT_EQ(archive_write_data(writer.get(), "a", 1), 1);
    }
    ASSERT_EQ(archive_write_close(writer.get()), ARCHIVE_OK);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(buffer.data(), static_cast<qint64>(used)), static_cast<qint64>(used));
}

TEST(TrackerArchive, ResolvesMediaOnlyFromExtractedMembers)
{
    QString extracted;
    {
        const auto archive = motrack::TrackerArchive::open(fixturePath("archive.trz"));
        EXPECT_EQ(archive.projects(), QStringList{"image-sequence.trk"});
        extracted = archive.root();
        EXPECT_TRUE(QFileInfo(archive.projectPath("image-sequence.trk")).isFile());
        EXPECT_THROW(std::ignore = archive.projectPath("missing.trk"), motrack::ProjectError);
        motrack::LegacyProject project;
        project.clip.mediaReferences = {"../frames/frame00.ppm",
                                        "archive.trz!/frames/frame02.ppm",
                                        "../../frames/frame00.ppm",
                                        "/frames/frame00.ppm",
                                        "https://example.test/frame.ppm"};
        archive.resolveMedia(project, "nested/experiment.trk");
        EXPECT_EQ(project.clip.mediaReferences, (std::vector<std::string>{"frames/frame00.ppm", "frames/frame02.ppm", "", "", ""}));
    }
    EXPECT_FALSE(QFileInfo::exists(extracted));
}

TEST(TrackerArchive, DetectsReplacedAndMissingExtractedMembers)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("source.trz");
    writeArchive(path, {"experiment.trk", "media.bin"});
    const auto archive = motrack::TrackerArchive::open(path);
    EXPECT_TRUE(archive.memberMatchesSource("media.bin"));
    EXPECT_FALSE(archive.memberMatchesSource("missing.bin"));
    EXPECT_FALSE(archive.memberMatchesSource("../media.bin"));
    QFile media(archive.root() + "/media.bin");
    ASSERT_TRUE(media.open(QIODevice::WriteOnly));
    ASSERT_EQ(media.write("b"), 1); // Equal size, different contents: checking paths or sizes is insufficient.
    media.close();
    EXPECT_FALSE(archive.memberMatchesSource("media.bin"));
    ASSERT_TRUE(media.remove());
    EXPECT_FALSE(archive.memberMatchesSource("media.bin"));
    ASSERT_TRUE(media.open(QIODevice::WriteOnly));
    ASSERT_EQ(media.write("a"), 1);
    media.close();
    EXPECT_TRUE(archive.memberMatchesSource("media.bin"));
    const std::atomic_bool cancelled{true};
    EXPECT_THROW(std::ignore = archive.memberMatchesSource("media.bin", &cancelled), motrack::ProjectError);
}

TEST(TrackerArchive, RejectsDangerousAndAmbiguousNames)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("bad.trz");
    writeArchive(path, {"experiment.trk", "../outside.trk"});
    EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(path), motrack::ProjectError);
    writeArchive(path, {"experiment.trk", "Experiment.TRK"});
    EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(path), motrack::ProjectError);
    writeArchive(path, {"experiment.trk", "NUL.trk"});
    EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(path), motrack::ProjectError);
    writeArchive(path, {"notes.txt"});
    EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(path), motrack::ProjectError);
    EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(fixturePath("point-mass.trk")), motrack::ProjectError);
}

TEST(TrackerArchive, OpensAnArchiveWithAUnicodeFilename)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath(QString::fromUtf8("élève.trz"));
    ASSERT_TRUE(QFile::copy(fixturePath("archive.trz"), path));
    const auto archive = motrack::TrackerArchive::open(path);
    EXPECT_EQ(archive.projects(), QStringList{"image-sequence.trk"});
}

/// An archive holding experiment.trk and a member named by the raw bytes @p name with the ZIP UTF-8 flag (bit 11)
/// clear, as Info-ZIP and older archivers write UTF-8 or code-page names. libarchive writes an ASCII placeholder of
/// the same length, which is then replaced in the local and central headers; ZIP checksums do not cover names.
void writeUnflaggedArchive(const QString& path, const QByteArray& name)
{
    auto placeholder = name;
    std::ranges::replace_if(placeholder, [](char byte) { return static_cast<unsigned char>(byte) >= 0x80; }, '#');
    const auto ascii = placeholder.toStdString();
    const QTemporaryDir staging;
    ASSERT_TRUE(staging.isValid());
    const auto staged = staging.filePath("staged.trz");
    writeArchive(staged, {"experiment.trk", ascii.c_str()});
    QFile input(staged);
    ASSERT_TRUE(input.open(QIODevice::ReadOnly));
    auto bytes = input.readAll();
    ASSERT_EQ(bytes.count(placeholder), 2);
    bytes.replace(placeholder, name);
    QFile output(path);
    ASSERT_TRUE(output.open(QIODevice::WriteOnly));
    ASSERT_EQ(output.write(bytes), bytes.size());
}

TEST(TrackerArchive, ReadsUnflaggedUtf8MemberNamesAsUtf8LikeTracker)
{
    // Java's ZipInputStream and ZipFile, which Tracker reads archives with, decode every name as UTF-8 whatever
    // the flag says. MoTrack must not decode it through the process's C locale or code page instead (#275).
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("legacy.trz");
    ASSERT_NO_FATAL_FAILURE(writeUnflaggedArchive(path, "vid\xC3\xA9o/\xE5\xB8\xA7 \xF0\x9F\x98\x80.png"));
    const auto archive = motrack::TrackerArchive::open(path);
    const auto member = QString::fromUtf8("vidéo/帧 😀.png");
    EXPECT_TRUE(QFileInfo(archive.root() + '/' + member).isFile());
    EXPECT_TRUE(archive.memberMatchesSource(member));
}

TEST(TrackerArchive, RefusesUnflaggedMemberNamesThatAreNotUtf8LikeTracker)
{
    // 0x82 is "é" in IBM code page 437. Java refuses the whole archive ("invalid CEN header (bad entry name)");
    // so does MoTrack, rather than guessing a legacy code page.
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("cp437.trz");
    ASSERT_NO_FATAL_FAILURE(writeUnflaggedArchive(path, "vid\x82o.png"));
    EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(path), motrack::ProjectError);
}

TEST(TrackerArchive, CancelsExtraction)
{
    const std::atomic_bool cancelled{true};
    EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(fixturePath("archive.trz"), &cancelled), motrack::ProjectError);
}

#ifndef _WIN32
TEST(TrackerArchive, ReadsUnicodeMembersInTheCLocaleAndRestoresCallerState)
{
    const auto callerLocale = newlocale(LC_ALL_MASK, "C", nullptr);
    ASSERT_NE(callerLocale, nullptr);
    const auto freeCallerLocale = qScopeGuard([callerLocale] { freelocale(callerLocale); });
    const auto previousLocale = uselocale(callerLocale);
    ASSERT_NE(previousLocale, nullptr);
    const auto restoreCallerLocale = qScopeGuard([previousLocale] { uselocale(previousLocale); });
    const std::string globalLocale = std::setlocale(LC_CTYPE, nullptr);
    const auto archive = motrack::TrackerArchive::open(fixturePath("../corpus/fixtures/linked-images/archived-images.trz"));
    EXPECT_EQ(archive.projects(), QStringList{"experiment.trk"});
    QFile notes(archive.root() + QString::fromUtf8("/support/notes é.txt"));
    ASSERT_TRUE(notes.open(QIODevice::ReadOnly));
    EXPECT_FALSE(notes.readAll().isEmpty());
    EXPECT_EQ(uselocale(nullptr), callerLocale);
    EXPECT_EQ(globalLocale, std::setlocale(LC_CTYPE, nullptr));
    EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(fixturePath("point-mass.trk")), motrack::ProjectError);
    EXPECT_EQ(uselocale(nullptr), callerLocale);
    EXPECT_EQ(globalLocale, std::setlocale(LC_CTYPE, nullptr));
}
#endif

TEST(TrackerArchive, OwnsTheOriginalSnapshotUntilTheLastOwnerIsDestroyed)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    ASSERT_TRUE(QFile::copy(fixturePath("archive.trz"), original));
    QFile input(original);
    ASSERT_TRUE(input.open(QIODevice::ReadOnly));
    const auto bytes = input.readAll();
    input.close();
    QString source;
    QString extracted;
    {
        auto archive = motrack::TrackerArchive::open(original);
        source = archive.sourcePath();
        extracted = archive.root();
        ASSERT_TRUE(QFile::remove(original));
        const auto moved = std::move(archive);
        EXPECT_EQ(moved.sourcePath(), source);
        EXPECT_EQ(moved.root(), extracted);
        QFile snapshot(source);
        ASSERT_TRUE(snapshot.open(QIODevice::ReadOnly));
        EXPECT_EQ(snapshot.readAll(), bytes);
        EXPECT_TRUE(QFileInfo(moved.projectPath("image-sequence.trk")).isFile());
    }
    EXPECT_FALSE(QFileInfo::exists(source));
    EXPECT_FALSE(QFileInfo::exists(extracted));
}

} // namespace
