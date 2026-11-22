#pragma once

#include "library/collection.hpp"
#include "library/java_string.hpp"
#include "library/resource.hpp"
#include "osp/expected.hpp"
#include "osp/source.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// `org.opensourcephysics.tools.Library`: the Library Browser's "my library" file (my_library.xml) and the OSP
/// library lists (tracker_library.xml, shared_library.xml) with their collection menu, sub-libraries, recently
/// opened tabs and search exclusions.
namespace motrack::library
{

/// Library.maxRecentTabCount.
inline constexpr std::size_t MAX_RECENT_TABS = 6;

struct LibraryCollectionEntry
{
    std::string path;
    std::string name;

    friend bool operator==(const LibraryCollectionEntry&, const LibraryCollectionEntry&) = default;
};

/// The fields Library.Loader reads, as declared in the file. OSP also opens each sub-library and imported
/// library while loading and keeps only those that read as a Library; MoTrack keeps the declared paths and
/// leaves opening them to the browser.
struct Library
{
    /// Null when the file has none: OSP then names the library after the user's home folder ("<name> Library").
    std::optional<std::string> name{};
    /// `pathList` in order (Library.Loader keeps duplicate paths) with each path's name.
    std::vector<LibraryCollectionEntry> collections{};
    std::vector<std::optional<std::string>> subLibraryPaths{};
    std::vector<std::optional<std::string>> importedLibraryPaths{};
    /// `recentTabs` after Library.addRecent: no duplicates, at most MAX_RECENT_TABS.
    std::vector<std::string> recentlyOpened{};
    /// `allPathsToNameMap`: collection names, then recently opened names (which may be null).
    PropertyMap pathNames{};
    /// `noSearchSet` (TreeSet): sorted, unique.
    std::vector<std::string> noSearchPaths{};
    std::optional<std::vector<std::optional<std::string>>> openTabs{};
    std::optional<std::string> chooserDirectory{};
    /// The OSP cache directory recorded by the browser that saved the file.
    std::optional<std::string> cache{};
    std::optional<SourceOrigin> origin{};
};

/// Equality of the saved content (ignores `origin`).
[[nodiscard]] bool sameContent(const Library& left, const Library& right);

/// Library.addCollection: trims both, ignores a path already present.
void addCollection(Library& library, std::string_view path, std::string_view name);
/// Library.renameCollection: only paths already added.
void renameCollection(Library& library, std::string_view path, std::string_view name);
/// Library.addRecent: moves the path to the front (or end), then drops entries beyond MAX_RECENT_TABS.
void addRecent(Library& library, std::string path, bool atEnd);
/// Library.removeRecent.
void removeRecent(Library& library, std::string_view path);
/// The name Library.Loader.saveObject writes for a recently opened path: its `pathNames` entry, else its file name.
[[nodiscard]] std::string recentName(const Library& library, std::string_view path);

struct LibraryDocument
{
    std::shared_ptr<const osp::Source> source{};
    Library library{};
};

/// Loads with Library.Loader semantics; `Malformed` where Java would throw (for example fewer
/// collection_names than collection_paths).
[[nodiscard]] osp::Expected<LibraryDocument, LoadError> loadLibrary(std::shared_ptr<const osp::Source> source);
[[nodiscard]] osp::Expected<LibraryDocument, LoadError> loadLibrary(std::string bytes, const osp::ParseLimits& limits = {});
/// Saves with the original bytes for every unchanged property; changed lists are rewritten whole.
[[nodiscard]] osp::Expected<std::string, SaveError> saveLibrary(const LibraryDocument& document);
/// Library.Loader.saveObject's layout. A null name is omitted (OSP would write the derived default).
[[nodiscard]] osp::ObjectValue libraryObjectValue(const Library& library);
[[nodiscard]] osp::Expected<std::string, SaveError> writeLibrary(const Library& library, osp::LineEnding lineEnding = osp::LineEnding::Lf);

} // namespace motrack::library
