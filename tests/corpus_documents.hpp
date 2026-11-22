#pragma once

#include "utf8_path.hpp"
#include "utf8_thread_locale.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

/// The committed Tracker documents that corpus-wide tests read: every .trk and .xml under tests/fixtures and
/// tests/corpus/fixtures, and the .trk and .xml members of their .trz archives. Requires MOTRACK_FIXTURES and
/// MOTRACK_CORPUS_FIXTURES.
namespace motrack::test
{

struct CorpusDocument
{
    std::string label;
    std::string bytes;
    bool quirk = false; ///< Synthetic reader fixture (tests/fixtures/osp) rather than a Tracker experiment.
};

inline std::string readCorpusFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

inline bool corpusXmlName(std::string_view name)
{
    return name.ends_with(".trk") || name.ends_with(".xml");
}

/// XML members of a Tracker archive, read in memory with TrackerArchive's name rules: UTF-8 names whatever the
/// C locale (#275), and a header warning, such as a name that is not UTF-8, fails instead of ending the list.
inline std::vector<CorpusDocument> corpusArchiveMembers(const std::string& label, const std::string& bytes)
{
    constexpr std::size_t MAX_MEMBER_BYTES = std::size_t{32} * 1024 * 1024;
    const motrack::ScopedUtf8ThreadLocale utf8Names;
    const std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), &archive_read_free);
    if (!reader || archive_read_support_format_zip(reader.get()) != ARCHIVE_OK ||
        archive_read_set_format_option(reader.get(), "zip", "hdrcharset", "UTF-8") != ARCHIVE_OK ||
        archive_read_open_memory(reader.get(), bytes.data(), bytes.size()) != ARCHIVE_OK)
    {
        throw std::runtime_error("Could not open archive " + label);
    }
    std::vector<CorpusDocument> members;
    archive_entry* entry = nullptr;
    for (auto status = archive_read_next_header(reader.get(), &entry); status != ARCHIVE_EOF;
         status = archive_read_next_header(reader.get(), &entry))
    {
        const char* path = status == ARCHIVE_OK ? archive_entry_pathname_utf8(entry) : nullptr;
        if (path == nullptr)
        {
            const char* error = archive_error_string(reader.get());
            throw std::runtime_error("Could not read a member header of archive " + label + ": " + (error != nullptr ? error : "no name"));
        }
        const std::string name = path;
        const auto size = archive_entry_size(entry);
        if (!corpusXmlName(name) || size < 0 || std::cmp_greater(size, MAX_MEMBER_BYTES))
        {
            continue;
        }
        std::string data(static_cast<std::size_t>(size), '\0');
        if (std::cmp_not_equal(archive_read_data(reader.get(), data.data(), data.size()), data.size()))
        {
            throw std::runtime_error("Could not read an archive member of " + label);
        }
        auto memberLabel = label;
        memberLabel += '!';
        memberLabel += name;
        members.push_back({.label = std::move(memberLabel), .bytes = std::move(data)});
    }
    return members;
}

/// Every distinct document, in a stable order. Archive members that repeat a linked experiment byte for byte are
/// listed once.
inline const std::vector<CorpusDocument>& corpusDocuments()
{
    static const std::vector<CorpusDocument> documents = []
    {
        std::vector<CorpusDocument> result;
        std::set<std::string> seen;
        const auto add = [&](CorpusDocument document)
        {
            if (seen.insert(document.bytes).second)
            {
                result.push_back(std::move(document));
            }
        };
        for (const auto* rootText : {MOTRACK_FIXTURES, MOTRACK_CORPUS_FIXTURES})
        {
            const auto root = motrack::utf8Path(rootText);
            std::vector<std::filesystem::path> paths;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
            {
                // library/ holds ComPADRE web-service responses for the library census (A2), which are plain XML
                // rather than OSP XMLControl documents.
                const auto relative = motrack::genericUtf8String(std::filesystem::relative(entry.path(), root));
                if (entry.is_regular_file() && !relative.starts_with("library/"))
                {
                    paths.push_back(entry.path());
                }
            }
            std::ranges::sort(paths);
            for (const auto& path : paths)
            {
                const auto label = motrack::genericUtf8String(std::filesystem::relative(path, root));
                const auto name = motrack::utf8String(path.filename());
                if (corpusXmlName(name))
                {
                    add({.label = label, .bytes = readCorpusFile(path), .quirk = path.parent_path().filename() == "osp"});
                }
                else if (name.ends_with(".trz"))
                {
                    for (auto& member : corpusArchiveMembers(label, readCorpusFile(path)))
                    {
                        add(std::move(member));
                    }
                }
            }
        }
        return result;
    }();
    return documents;
}

} // namespace motrack::test
