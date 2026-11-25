#include "tracker_archive.hpp"

#include "archive_fixture.hpp"
#include "utf8_thread_locale.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>
#include <gtest/gtest.h>

#include <algorithm>
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
    const motrack::ScopedUtf8ThreadLocale utf8Names; // as the production writers do
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
    // "//server" and "/server" stay distinct names on every platform.
    for (const auto* name : {"D:/horHarmonic/videos/a.mp4",
                             "C:|Users|x|b.mp4",
                             "//server/share/a.mp4",
                             "/server/share/a.mp4",
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
                                                                    {"../escape.mp4", "experiment.trk"},
                                                                    {"videos/./../../escape.mp4", "experiment.trk"}})
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
    EXPECT_TRUE(archive.omittedOnSave().isEmpty()); // The folder-icon resource fork is real metadata.
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
    try
    {
        std::ignore = motrack::TrackerArchive::open(path);
        ADD_FAILURE() << "A CP437 member name was accepted.";
    }
    catch (const motrack::ProjectError& error)
    {
        // The user learns why, not libarchive's "cannot be converted ... to current locale".
        EXPECT_NE(std::string(error.what()).find("not valid UTF-8"), std::string::npos) << error.what();
    }
}

TEST(TrackerArchive, RefusesNamesJavaCannotDecodeWhateverTheFlagSays)
{
    // java.util.zip decodes every name with a strict UTF-8 decoder, flagged (general-purpose bit 11) or not, and
    // throws on the first malformed sequence, so Tracker opens none of these archives (#275). The records are written
    // directly: libarchive's writers cannot store such names.
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("malformed.trz");
    struct Case
    {
        const char* what;
        QByteArray name;
    };
    for (const auto& malformed : {Case{.what = "IBM 437 e-acute", .name = "vid\x82o.png"},
                                  Case{.what = "Windows-1252 e-acute", .name = "vid\xE9o.png"},
                                  Case{.what = "truncated sequence", .name = "video\xC3"},
                                  Case{.what = "stray continuation byte", .name = "vid\xA9o.png"},
                                  Case{.what = "overlong slash",
                                       .name = "videos\xC0\xAF"
                                               "a.png"},
                                  Case{.what = "lone surrogate", .name = "vid\xED\xA0\x80o.png"},
                                  Case{.what = "beyond U+10FFFF", .name = "vid\xF4\x90\x80\x80o.png"}})
    {
        for (const bool flagged : {false, true})
        {
            SCOPED_TRACE(std::string(malformed.what) + (flagged ? ", flagged" : ", unflagged"));
            ASSERT_TRUE(motrack::test::writeStoredArchive(
                path, {{.name = "experiment.trk", .bytes = "<object/>"}, {.name = malformed.name.constData(), .bytes = "a"}}, flagged));
            try
            {
                std::ignore = motrack::TrackerArchive::open(path);
                ADD_FAILURE() << "The archive opened.";
            }
            catch (const motrack::ProjectError& error)
            {
                EXPECT_NE(std::string(error.what()).find("not valid UTF-8"), std::string::npos) << error.what();
            }
            EXPECT_EQ(motrack::trackerArchiveProjectNames(path), std::nullopt);
        }
    }
}

TEST(TrackerArchive, TreatsAReplacementCharacterInAValidNameAsUnsafeNotAsMalformed)
{
    // U+FFFD encoded as EF BF BD is valid UTF-8: java.util.zip reads the name and Tracker opens the archive. Such
    // names come from tools that already replaced bytes they could not decode. MoTrack must not report the archive
    // as "not valid UTF-8"; the name is unsafe instead (it can hide what it stood for), so the member is skipped and
    // the rest of the archive opens, and a project with such a name refuses the archive as other unsafe ones do.
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("replaced.trz");
    const QByteArray replaced("notes \xEF\xBF\xBD.txt");
    for (const bool flagged : {false, true})
    {
        SCOPED_TRACE(flagged ? "flagged" : "unflagged");
        ASSERT_TRUE(motrack::test::writeStoredArchive(
            path, {{.name = "experiment.trk", .bytes = "<object/>"}, {.name = replaced.constData(), .bytes = "kept"}}, flagged));
        const auto archive = motrack::TrackerArchive::open(path);
        EXPECT_EQ(archive.projects(), QStringList{"experiment.trk"});
        EXPECT_EQ(archive.skippedMembers(), QStringList{QString::fromUtf8(replaced)});
        EXPECT_TRUE(archive.omittedOnSave().isEmpty());
        EXPECT_EQ(extractedFiles(archive.root()), QStringList{"experiment.trk"});
        EXPECT_EQ(motrack::trackerArchiveProjectNames(path), QSet<QString>{"experiment.trk"});

        ASSERT_TRUE(motrack::test::writeStoredArchive(path, {{.name = "exp\xEF\xBF\xBD.trk", .bytes = "<object/>"}}, flagged));
        try
        {
            std::ignore = motrack::TrackerArchive::open(path);
            ADD_FAILURE() << "A project with a replacement character in its name was extracted.";
        }
        catch (const motrack::ProjectError& error)
        {
            EXPECT_NE(std::string(error.what()).find("unsafe member name"), std::string::npos) << error.what();
        }
    }
}

TEST(TrackerArchive, NamesOnlyNameConversionWarningsAsNonUtf8Names)
{
    // libarchive's ZIP reader texts for a name hdrcharset=UTF-8 cannot convert; anything else keeps the generic message.
    EXPECT_TRUE(motrack::detail::isNameConversionWarning("Pathname cannot be converted from UTF-8 to current locale."));
    EXPECT_TRUE(motrack::detail::isNameConversionWarning("Symlink cannot be converted from UTF-8 to current locale."));
    EXPECT_FALSE(motrack::detail::isNameConversionWarning("ZIP bad CRC: 0x1234 should be 0x5678"));
    EXPECT_FALSE(motrack::detail::isNameConversionWarning("Truncated ZIP file header"));
    EXPECT_FALSE(motrack::detail::isNameConversionWarning("Inconsistent uncompressed size: 4 in central directory, 5 in local header"));
    EXPECT_FALSE(motrack::detail::isNameConversionWarning(""));
}

TEST(TrackerArchive, MemberKeysAreNfc)
{
    const auto nfc = QString::fromUtf8("expérience/帧.trk").toStdString();
    const auto nfd = QString::fromStdString(nfc).normalized(QString::NormalizationForm_D).toStdString();
    ASSERT_NE(nfc, nfd);
    EXPECT_EQ(motrack::archiveMemberKey(nfd), nfc);
    EXPECT_EQ(motrack::archiveMemberKey(nfc), nfc);
    EXPECT_EQ(motrack::archiveMemberKey("experiment.trk"), "experiment.trk");
    EXPECT_EQ(motrack::archiveMemberKey(QString::fromStdString(nfd)), QString::fromStdString(nfc));
}

TEST(TrackerArchive, IdentifiesMembersByNfcWhicheverFormIsStored)
{
    // libarchive returns the names it reads in NFD on macOS and in NFC elsewhere, whatever the archive stores, so
    // an archive made on macOS (decomposed names) and its Windows twin must list, extract and resolve alike on
    // every platform (#308). The records are written directly: libarchive's writers normalize names too.
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto project = QString::fromUtf8("expérience/élève.trk");
    const auto image = QString::fromUtf8("expérience/vidéos/îmage ç.png");
    const auto reference = QString::fromUtf8("vidéos/îmage ç.png");
    ASSERT_NE(project, project.normalized(QString::NormalizationForm_D));
    for (const auto form : {QString::NormalizationForm_C, QString::NormalizationForm_D})
    {
        SCOPED_TRACE(form == QString::NormalizationForm_C ? "stored NFC" : "stored NFD");
        const auto storedProject = project.normalized(form).toUtf8();
        const auto storedImage = image.normalized(form).toUtf8();
        const auto path = temporary.filePath(QStringLiteral("stored-%1.trz").arg(static_cast<int>(form)));
        ASSERT_TRUE(motrack::test::writeStoredArchive(
            path, {{.name = storedProject.constData(), .bytes = "<object/>"}, {.name = storedImage.constData(), .bytes = "pixels"}}));
        const auto archive = motrack::TrackerArchive::open(path);
        EXPECT_EQ(archive.projects(), QStringList{project});
        EXPECT_EQ(archive.memberDigests().size(), 2);
        EXPECT_TRUE(archive.memberDigests().contains(project));
        EXPECT_TRUE(archive.memberDigests().contains(image));
        EXPECT_TRUE(archive.skippedMembers().isEmpty());
        auto extracted = extractedFiles(archive.root());
        for (auto& file : extracted)
            file = file.normalized(QString::NormalizationForm_C); // a directory listing has the file system's form
        extracted.sort();
        EXPECT_EQ(extracted, (QStringList{image, project}));
        for (const auto lookup : {QString::NormalizationForm_C, QString::NormalizationForm_D})
        {
            SCOPED_TRACE(lookup == QString::NormalizationForm_C ? "lookup NFC" : "lookup NFD");
            EXPECT_EQ(archive.projectPath(project.normalized(lookup)), QDir(archive.root()).filePath(project));
            EXPECT_TRUE(QFileInfo(archive.projectPath(project.normalized(lookup))).isFile());
            EXPECT_TRUE(archive.memberMatchesSource(image.normalized(lookup)));
            motrack::LegacyProject experiment;
            experiment.clip.mediaReferences = {reference.normalized(lookup).toStdString(),
                                               ("./" + reference.normalized(lookup)).toStdString(),
                                               ("archive.trz!/" + image.normalized(lookup)).toStdString(),
                                               QString::fromUtf8("vidéos/image c.png").toStdString()};
            archive.resolveMedia(experiment, project.normalized(lookup));
            EXPECT_EQ(experiment.clip.mediaReferences,
                      (std::vector<std::string>{image.toStdString(), image.toStdString(), image.toStdString(), ""}));
        }
    }
    // Tracker reads the two spellings as two members. MoTrack cannot tell them apart by identity, nor could a
    // normalization-insensitive file system hold both, so the archive is refused as ambiguous.
    const auto composed = QString::fromUtf8("vidéo.png").toUtf8();
    const auto decomposed = QString::fromUtf8("vidéo.png").normalized(QString::NormalizationForm_D).toUtf8();
    const auto ambiguous = temporary.filePath("ambiguous.trz");
    ASSERT_TRUE(motrack::test::writeStoredArchive(ambiguous,
                                                  {{.name = "experiment.trk", .bytes = "<object/>"},
                                                   {.name = composed.constData(), .bytes = "a"},
                                                   {.name = decomposed.constData(), .bytes = "b"}}));
    EXPECT_THROW(std::ignore = motrack::TrackerArchive::open(ambiguous), motrack::ProjectError);
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
