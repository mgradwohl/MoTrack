#include "tracker_archive.hpp"

#include "archive_fixture.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>
#include <gtest/gtest.h>

#include <array>
#include <clocale>
#include <initializer_list>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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

QStringList extractedFiles(const QString& root)
{
    QStringList files;
    QDirIterator iterator(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (iterator.hasNext())
        files.push_back(QDir(root).relativeFilePath(iterator.next()));
    files.sort();
    return files;
}

TEST(TrackerArchive, SkipsUnsafeNonProjectMembersWithoutWritingOutsideTheExtraction)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // A real absolute path in this test's directory: drive-letter on Windows, rooted elsewhere.
    const auto outside = QDir(temporary.path()).filePath("outside");
    const auto absolute = (outside + "/absolute.mp4").toUtf8();
    const auto traversal = QString("../%1.mp4").arg(token).toUtf8();
    const auto nestedTraversal = QString("videos/../../%1-nested.mp4").arg(token).toUtf8();
    std::vector<motrack::test::ArchiveEntry> members{{.name = "experiment.trk", .bytes = "<object/>"},
                                                     {.name = "videos/a.mp4", .bytes = motrack::test::duplicateMovieBytes()},
                                                     {.name = "support|notes.txt", .bytes = "backslash-only names use '/'"}};
    for (auto& unsafe : motrack::test::unsafeTrackerExportMembers())
        members.push_back(std::move(unsafe));
    // The same file name with different bytes is not a redundant copy, so saving keeps it.
    for (const auto* name : {"D:/horHarmonic/videos/a.mp4",
                             "C:|Users|x|b.mp4",
                             "//server/share/a.mp4",
                             "||server|share|b.mp4",
                             "/rooted/a.mp4",
                             "videos/mixed|c.mp4",
                             absolute.constData(),
                             traversal.constData(),
                             nestedTraversal.constData(),
                             "videos/tab\t.mp4",
                             "aux.mp4"})
        members.push_back({.name = name, .bytes = "unsafe"});
    const auto path = temporary.filePath("unsafe.trz");
    ASSERT_TRUE(motrack::test::writeArchiveWithBackslashes(path, members));
    QStringList expectedSkipped;
    for (std::size_t i = 3; i < members.size(); ++i)
        expectedSkipped.push_back(QString::fromUtf8(members[i].name));
    // libarchive reads a name that has only '\' separators with '/' separators; mixed names keep '\'.
    expectedSkipped.replace(expectedSkipped.indexOf("C:|Users|x|b.mp4"), "C:/Users/x/b.mp4");
    expectedSkipped.replace(expectedSkipped.indexOf("||server|share|b.mp4"), "//server/share/b.mp4");
    expectedSkipped.replace(expectedSkipped.indexOf("videos/mixed|c.mp4"), "videos/mixed\\c.mp4");

    const auto archive = motrack::TrackerArchive::open(path);
    EXPECT_EQ(archive.projects(), QStringList{"experiment.trk"});
    EXPECT_EQ(archive.skippedMembers(), expectedSkipped);
    EXPECT_EQ(archive.omittedOnSave(), motrack::test::omittedTrackerExportMembers());
    EXPECT_EQ(extractedFiles(archive.root()), (QStringList{"experiment.trk", "support/notes.txt", "videos/a.mp4"}));
    EXPECT_EQ(archive.memberDigests().size(), 3);
    EXPECT_TRUE(archive.memberMatchesSource("videos/a.mp4"));
    EXPECT_FALSE(archive.memberMatchesSource("C:/Users/x/videos/a.mp4"));
    EXPECT_THROW(std::ignore = archive.projectPath("C:/Users/x/videos/a.mp4"), motrack::ProjectError);
    // Nothing escapes the private extraction directory.
    EXPECT_FALSE(QFileInfo::exists(outside));
    EXPECT_FALSE(QFileInfo::exists(QDir(archive.root()).filePath(QString::fromUtf8(traversal))));
    EXPECT_FALSE(QFileInfo::exists(QDir(archive.root()).filePath(QString("../%1-nested.mp4").arg(token))));
    EXPECT_EQ(QDir(temporary.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList{"unsafe.trz"});
}

TEST(TrackerArchive, RefusesUnsafeProjectNamesAndUnsafeSpellingsOfExtractedMembers)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("refused.trz");
    for (const auto* project :
         {"D:/a/b.trk", "C:/Users/x/Experiment.TRK", "//server/share/c.trk", "nested/mixed|d.trk", "..|e.trk", "html/Icon\r.trk"})
    {
        SCOPED_TRACE(project);
        ASSERT_TRUE(motrack::test::writeArchiveWithBackslashes(
            path, {{.name = "experiment.trk", .bytes = "<object/>"}, {.name = project, .bytes = "<object/>"}}));
        EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(path), motrack::ProjectError);
    }
    ASSERT_TRUE(motrack::test::writeArchiveWithBackslashes(path, {{.name = "nested|d.trk", .bytes = "<object/>"}}));
    EXPECT_EQ(motrack::TrackerArchive::open(path).projects(), QStringList{"nested/d.trk"});
    // Another spelling of an extracted member's name, skipped or not, leaves the archive ambiguous.
    for (const auto* alias : {"videos|a.mp4", "./videos/a.mp4", "videos/x/../A.MP4"})
    {
        SCOPED_TRACE(alias);
        ASSERT_TRUE(motrack::test::writeArchiveWithBackslashes(
            path,
            {{.name = "experiment.trk", .bytes = "<object/>"}, {.name = "videos/a.mp4", .bytes = "a"}, {.name = alias, .bytes = "b"}}));
        EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(path), motrack::ProjectError);
    }
    // Entry type checks still apply before any name is skipped.
    ASSERT_TRUE(motrack::test::writeArchive(
        path, {{.name = "experiment.trk", .bytes = "<object/>"}, {.name = "C:/Users/x/link", .bytes = {}, .symlink = true}}));
    EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(path), motrack::ProjectError);
    ASSERT_TRUE(motrack::test::writeArchive(path, motrack::test::unsafeTrackerExportMembers()));
    EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(path), motrack::ProjectError);
}

TEST(TrackerArchive, RefusesMediaReferencesToSkippedMembersAndNeverResolvesLocalFiles)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    // This member's name is also a real file on disk; neither may satisfy a project's reference.
    const auto local = QDir(temporary.path()).filePath("local.mp4");
    QFile file(local);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly) && file.write("local") == 5);
    file.close();
    const auto localName = local.toUtf8();
    std::vector<motrack::test::ArchiveEntry> members{{.name = "experiment.trk", .bytes = "<object/>"},
                                                     {.name = "nested/inner.trk", .bytes = "<object/>"},
                                                     {.name = "videos/a.mp4", .bytes = "movie"},
                                                     {.name = "../escape.mp4", .bytes = "unsafe"},
                                                     {.name = localName.constData(), .bytes = "copy"}};
    for (auto& unsafe : motrack::test::unsafeTrackerExportMembers())
        members.push_back(std::move(unsafe));
    const auto path = temporary.filePath("references.trz");
    ASSERT_TRUE(motrack::test::writeArchive(path, members));
    const auto archive = motrack::TrackerArchive::open(path);

    motrack::LegacyProject project;
    project.clip.mediaReferences = {"videos/a.mp4", "./videos/a.mp4", "E:/elsewhere/a.mp4", "videos/missing.mp4"};
    archive.resolveMedia(project, "experiment.trk");
    EXPECT_EQ(project.clip.mediaReferences, (std::vector<std::string>{"videos/a.mp4", "videos/a.mp4", "", ""}));
    const auto otherLocal = QDir(temporary.path()).filePath("references.trz").toStdString();
    project.clip.mediaReferences = {otherLocal};
    archive.resolveMedia(project, "experiment.trk");
    EXPECT_EQ(project.clip.mediaReferences, std::vector<std::string>{""});

    for (const auto& [reference, member] :
         std::initializer_list<std::pair<std::string, const char*>>{{"C:/Users/x/videos/a.mp4", "experiment.trk"},
                                                                    {R"(C:\Users\x\videos\a.mp4)", "experiment.trk"},
                                                                    {"archive.trz!/C:/Users/x/videos/a.mp4", "experiment.trk"},
                                                                    {local.toStdString(), "experiment.trk"},
                                                                    {"html/Icon\r", "experiment.trk"},
                                                                    {"videos\\..\\html\\Icon\r", "experiment.trk"},
                                                                    {"../html/Icon\r", "nested/inner.trk"},
                                                                    {"../escape.mp4", "experiment.trk"}})
    {
        SCOPED_TRACE(reference);
        project.clip.mediaReferences = {"videos/a.mp4", reference};
        EXPECT_THROW(archive.resolveMedia(project, member), motrack::ProjectError);
    }
}

TEST(TrackerArchive, KeepsMacMetadataAsMembersButNeverAsProjects)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("finder.trz");
    const auto appleDouble = motrack::test::appleDoubleBytes();
    // Every platform reads these entries; macOS libarchive would otherwise hide "__MACOSX/" members.
    ASSERT_TRUE(motrack::test::writeArchive(path,
                                            {{.name = "Experiment/experiment.trk", .bytes = "<object/>"},
                                             {.name = "__MACOSX/Experiment/._experiment.trk", .bytes = appleDouble},
                                             {.name = "__MACOSX/Experiment/._empty.trk", .bytes = {}},
                                             {.name = "__MACOSX/Experiment/kept.trk", .bytes = "<object/>"},
                                             {.name = "Experiment/._copied.trk", .bytes = appleDouble},
                                             {.name = "__MACOSX/Experiment/html/._Icon\r", .bytes = appleDouble}}));
    const auto archive = motrack::TrackerArchive::open(path);
    EXPECT_EQ(archive.projects(), (QStringList{"Experiment/experiment.trk", "__MACOSX/Experiment/kept.trk"}));
    EXPECT_EQ(archive.skippedMembers(), QStringList{"__MACOSX/Experiment/html/._Icon\r"});
    EXPECT_EQ(archive.omittedOnSave(), QStringList{"__MACOSX/Experiment/html/._Icon\r"});
    for (const auto* member : {"__MACOSX/Experiment/._experiment.trk", "__MACOSX/Experiment/._empty.trk", "Experiment/._copied.trk"})
    {
        EXPECT_TRUE(archive.memberDigests().contains(member)) << member;
        EXPECT_TRUE(archive.memberMatchesSource(member)) << member;
    }
    ASSERT_TRUE(motrack::test::writeArchive(path, {{.name = "__MACOSX/._experiment.trk", .bytes = appleDouble}}));
    EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(path), motrack::ProjectError);
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
