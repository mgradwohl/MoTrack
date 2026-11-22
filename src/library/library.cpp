#include "library/library.hpp"

#include "library/osp_values.hpp"
#include "library/paths.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <utility>

namespace motrack::library
{
namespace
{

using detail::Result;
using detail::StringList;

/// The order in which Library.Loader.saveObject writes properties.
constexpr std::array<std::string_view, 11> LIBRARY_ORDER = {"name",
                                                            "collection_paths",
                                                            "collection_names",
                                                            "sublibrary_paths",
                                                            "imported_library_paths",
                                                            "open_tabs",
                                                            "chooser_directory",
                                                            "recently_opened",
                                                            "recently_opened_names",
                                                            "no_search_paths",
                                                            "cache"};

std::vector<std::optional<std::string>> recentNames(const Library& library)
{
    std::vector<std::optional<std::string>> names;
    names.reserve(library.recentlyOpened.size());
    for (const auto& path : library.recentlyOpened)
    {
        names.emplace_back(recentName(library, path));
    }
    return names;
}

StringList toList(const std::vector<std::string>& values)
{
    return {values.begin(), values.end()};
}

StringList collectionPaths(const Library& library)
{
    StringList paths;
    for (const auto& entry : library.collections)
    {
        paths.emplace_back(entry.path);
    }
    return paths;
}

StringList collectionNames(const Library& library)
{
    StringList names;
    for (const auto& entry : library.collections)
    {
        names.emplace_back(entry.name);
    }
    return names;
}

Result<std::optional<StringList>> stringArray(const detail::ValueReader& reader, const osp::ObjectView& root, std::string_view name)
{
    auto value = reader.getObject(root, name, "root");
    if (!value)
    {
        return osp::makeUnexpected(value.error());
    }
    return detail::castStringArray(*value, "root." + std::string(name));
}

Result<Library> loadFields(const osp::Source& source)
{
    const detail::ValueReader reader([](const osp::ObjectView&, const std::string&) -> Result<Resource> { return Resource{}; });
    const osp::ObjectView root(source, source.root());
    Library library;
    auto name = reader.getString(root, "name", "root");
    if (!name)
    {
        return osp::makeUnexpected(name.error());
    }
    library.name = *name;
    auto paths = stringArray(reader, root, "collection_paths");
    if (!paths)
    {
        return osp::makeUnexpected(paths.error());
    }
    if (*paths)
    {
        auto names = stringArray(reader, root, "collection_names");
        if (!names)
        {
            return osp::makeUnexpected(names.error());
        }
        std::map<std::string, std::string, JavaLess> pathToName;
        std::vector<std::string> pathList;
        for (std::size_t index = 0; index < (*paths)->size(); ++index)
        {
            const auto& path = (**paths)[index];
            if (!path)
            {
                continue;
            }
            if (!*names)
            {
                return detail::fail("root.collection_names", "missing collection names");
            }
            if (index >= (*names)->size())
            {
                return detail::fail("root.collection_names", "fewer collection names than paths");
            }
            const auto& collectionName = (**names)[index];
            if (!collectionName)
            {
                continue;
            }
            pathList.push_back(*path);
            pathToName.insert_or_assign(*path, *collectionName);
            library.pathNames.set(*path, *collectionName);
        }
        for (auto& path : pathList)
        {
            auto entryName = pathToName.at(path);
            library.collections.push_back(LibraryCollectionEntry{.path = std::move(path), .name = std::move(entryName)});
        }
    }
    auto subLibraries = stringArray(reader, root, "sublibrary_paths");
    if (!subLibraries)
    {
        return osp::makeUnexpected(subLibraries.error());
    }
    library.subLibraryPaths = subLibraries->value_or(StringList{});
    auto imported = stringArray(reader, root, "imported_library_paths");
    if (!imported)
    {
        return osp::makeUnexpected(imported.error());
    }
    library.importedLibraryPaths = imported->value_or(StringList{});
    auto recent = stringArray(reader, root, "recently_opened");
    if (!recent)
    {
        return osp::makeUnexpected(recent.error());
    }
    auto recentNamesList = stringArray(reader, root, "recently_opened_names");
    if (!recentNamesList)
    {
        return osp::makeUnexpected(recentNamesList.error());
    }
    if (*recent)
    {
        for (const auto& path : **recent)
        {
            if (path)
            {
                addRecent(library, *path, true);
            }
        }
        if (*recentNamesList)
        {
            if ((*recentNamesList)->size() > (*recent)->size())
            {
                return detail::fail("root.recently_opened_names", "more names than recently opened paths");
            }
            for (std::size_t index = 0; index < (*recentNamesList)->size(); ++index)
            {
                const auto& path = (**recent)[index];
                if (!path)
                {
                    // HashMap.put(null, name): MoTrack has no null key, and the browser never reads one.
                    continue;
                }
                library.pathNames.set(*path, (**recentNamesList)[index]);
            }
        }
    }
    auto noSearch = stringArray(reader, root, "no_search_paths");
    if (!noSearch)
    {
        return osp::makeUnexpected(noSearch.error());
    }
    if (*noSearch)
    {
        for (const auto& path : **noSearch)
        {
            if (!path)
            {
                return detail::fail("root.no_search_paths", "null path");
            }
            const auto position = std::ranges::lower_bound(library.noSearchPaths, *path, JavaLess{});
            if (position == library.noSearchPaths.end() || *position != *path)
            {
                library.noSearchPaths.insert(position, *path);
            }
        }
    }
    auto openTabs = stringArray(reader, root, "open_tabs");
    if (!openTabs)
    {
        return osp::makeUnexpected(openTabs.error());
    }
    library.openTabs = *openTabs;
    auto chooser = reader.getString(root, "chooser_directory", "root");
    if (!chooser)
    {
        return osp::makeUnexpected(chooser.error());
    }
    library.chooserDirectory = *chooser;
    auto cache = reader.getString(root, "cache", "root");
    if (!cache)
    {
        return osp::makeUnexpected(cache.error());
    }
    library.cache = *cache;
    return library;
}

/// The library as Library.Loader reads it back after saving: names of recently opened paths are what the
/// writer derives.
Library reloadedForm(const Library& library)
{
    Library result = library;
    result.origin.reset();
    result.pathNames.clear();
    for (const auto& entry : library.collections)
    {
        result.pathNames.set(entry.path, entry.name);
    }
    for (const auto& path : library.recentlyOpened)
    {
        result.pathNames.set(path, recentName(library, path));
    }
    return result;
}

bool sameSavedContent(const Library& left, const Library& right)
{
    return left.name == right.name && left.collections == right.collections && left.subLibraryPaths == right.subLibraryPaths &&
           left.importedLibraryPaths == right.importedLibraryPaths && left.recentlyOpened == right.recentlyOpened &&
           left.noSearchPaths == right.noSearchPaths && left.openTabs == right.openTabs &&
           left.chooserDirectory == right.chooserDirectory && left.cache == right.cache && recentNames(left) == recentNames(right);
}

void applyChanges(detail::PropertyEditor& editor, const Library& baseline, const Library& current)
{
    const auto text = [&editor](std::string_view name, const std::optional<std::string>& before, const std::optional<std::string>& after)
    {
        if (after == before)
        {
            return;
        }
        if (after)
        {
            editor.setString(name, *after);
        }
        else
        {
            editor.removeAll(name);
        }
    };
    // Writes a changed list, or removes it where Library.Loader.saveObject would not write it.
    const auto list = [&editor](std::string_view name, const StringList& values, bool written)
    {
        if (written)
        {
            editor.setProperty(detail::stringArrayProperty(std::string(name), values));
        }
        else
        {
            editor.removeAll(name);
        }
    };
    text("name", baseline.name, current.name);
    if (baseline.collections != current.collections)
    {
        list("collection_paths", collectionPaths(current), !current.collections.empty());
        list("collection_names", collectionNames(current), !current.collections.empty());
    }
    if (baseline.subLibraryPaths != current.subLibraryPaths)
    {
        list("sublibrary_paths", current.subLibraryPaths, !current.subLibraryPaths.empty());
    }
    if (baseline.importedLibraryPaths != current.importedLibraryPaths)
    {
        list("imported_library_paths", current.importedLibraryPaths, !current.importedLibraryPaths.empty());
    }
    if (baseline.openTabs != current.openTabs)
    {
        list("open_tabs", current.openTabs.value_or(StringList{}), current.openTabs.has_value());
    }
    text("chooser_directory", baseline.chooserDirectory, current.chooserDirectory);
    if (baseline.recentlyOpened != current.recentlyOpened || recentNames(baseline) != recentNames(current))
    {
        list("recently_opened", toList(current.recentlyOpened), !current.recentlyOpened.empty());
        list("recently_opened_names", recentNames(current), !current.recentlyOpened.empty());
    }
    if (baseline.noSearchPaths != current.noSearchPaths)
    {
        list("no_search_paths", toList(current.noSearchPaths), !current.noSearchPaths.empty());
    }
    text("cache", baseline.cache, current.cache);
}

osp::Expected<std::string, SaveError> verified(osp::Expected<std::string, osp::RenderError> output, const Library& library)
{
    if (!output)
    {
        return osp::makeUnexpected(SaveError{.code = SaveError::Code::Render, .message = output.error().message});
    }
    auto reloaded = loadLibrary(*output);
    if (!reloaded || !sameSavedContent(reloaded->library, reloadedForm(library)))
    {
        return osp::makeUnexpected(
            SaveError{.code = SaveError::Code::VerificationFailed, .message = "the saved library reloads with different content"});
    }
    return *std::move(output);
}

} // namespace

bool sameContent(const Library& left, const Library& right)
{
    return sameSavedContent(left, right) && left.pathNames == right.pathNames;
}

void addCollection(Library& library, std::string_view path, std::string_view name)
{
    auto trimmedPath = javaTrimmed(path);
    if (std::ranges::any_of(library.collections, [&trimmedPath](const auto& entry) { return entry.path == trimmedPath; }))
    {
        return;
    }
    auto trimmedName = javaTrimmed(name);
    library.pathNames.set(trimmedPath, trimmedName);
    library.collections.push_back(LibraryCollectionEntry{.path = std::move(trimmedPath), .name = std::move(trimmedName)});
}

void renameCollection(Library& library, std::string_view path, std::string_view name)
{
    const auto trimmedPath = javaTrimmed(path);
    const auto trimmedName = javaTrimmed(name);
    bool found = false;
    for (auto& entry : library.collections)
    {
        if (entry.path == trimmedPath)
        {
            entry.name = trimmedName;
            found = true;
        }
    }
    if (found)
    {
        library.pathNames.set(trimmedPath, trimmedName);
    }
}

void addRecent(Library& library, std::string path, bool atEnd)
{
    auto& recent = library.recentlyOpened;
    std::erase(recent, path);
    if (atEnd)
    {
        recent.push_back(std::move(path));
    }
    else
    {
        recent.insert(recent.begin(), std::move(path));
    }
    if (recent.size() > MAX_RECENT_TABS)
    {
        recent.resize(MAX_RECENT_TABS);
    }
}

void removeRecent(Library& library, std::string_view path)
{
    std::erase(library.recentlyOpened, path);
}

std::string recentName(const Library& library, std::string_view path)
{
    if (const auto* name = library.pathNames.find(path); name != nullptr && name->has_value())
    {
        return name->value();
    }
    return std::string(fileName(path));
}

osp::Expected<LibraryDocument, LoadError> loadLibrary(std::shared_ptr<const osp::Source> source)
{
    if (!source)
    {
        return osp::makeUnexpected(LoadError{.code = LoadError::Code::Parse, .message = "no document"});
    }
    if (documentKind(*source) != DocumentKind::Library)
    {
        return osp::makeUnexpected(
            LoadError{.code = LoadError::Code::NotLibraryDocument, .location = "root", .message = "the root is not a Library"});
    }
    auto library = loadFields(*source);
    if (!library)
    {
        return osp::makeUnexpected(
            LoadError{.code = LoadError::Code::Malformed, .location = library.error().location, .message = library.error().message});
    }
    library->origin = SourceOrigin{.source = source, .object = source->root()};
    return LibraryDocument{.source = std::move(source), .library = *std::move(library)};
}

osp::Expected<LibraryDocument, LoadError> loadLibrary(std::string bytes, const osp::ParseLimits& limits)
{
    auto source = osp::parse(std::move(bytes), limits);
    if (!source)
    {
        return osp::makeUnexpected(LoadError{.code = LoadError::Code::Parse, .message = source.error().message, .parse = source.error()});
    }
    return loadLibrary(*std::move(source));
}

osp::ObjectValue libraryObjectValue(const Library& library)
{
    osp::ObjectValue object{.className = std::string(LIBRARY_CLASS)};
    auto& properties = object.properties;
    if (library.name)
    {
        properties.push_back(osp::stringProperty("name", *library.name));
    }
    if (!library.collections.empty())
    {
        properties.push_back(detail::stringArrayProperty("collection_paths", collectionPaths(library)));
        properties.push_back(detail::stringArrayProperty("collection_names", collectionNames(library)));
    }
    if (!library.subLibraryPaths.empty())
    {
        properties.push_back(detail::stringArrayProperty("sublibrary_paths", library.subLibraryPaths));
    }
    if (!library.importedLibraryPaths.empty())
    {
        properties.push_back(detail::stringArrayProperty("imported_library_paths", library.importedLibraryPaths));
    }
    if (library.openTabs)
    {
        properties.push_back(detail::stringArrayProperty("open_tabs", *library.openTabs));
    }
    if (library.chooserDirectory)
    {
        properties.push_back(osp::stringProperty("chooser_directory", *library.chooserDirectory));
    }
    if (!library.recentlyOpened.empty())
    {
        properties.push_back(detail::stringArrayProperty("recently_opened", toList(library.recentlyOpened)));
        properties.push_back(detail::stringArrayProperty("recently_opened_names", recentNames(library)));
    }
    if (!library.noSearchPaths.empty())
    {
        properties.push_back(detail::stringArrayProperty("no_search_paths", toList(library.noSearchPaths)));
    }
    if (library.cache)
    {
        properties.push_back(osp::stringProperty("cache", *library.cache));
    }
    return object;
}

osp::Expected<std::string, SaveError> saveLibrary(const LibraryDocument& document)
{
    const auto& library = document.library;
    const bool bound = document.source && library.origin && library.origin->source == document.source;
    if (!bound)
    {
        const auto ending = document.source ? document.source->lineEnding() : osp::LineEnding::Lf;
        return verified(osp::write(libraryObjectValue(library), osp::WriteOptions{.lineEnding = ending}), library);
    }
    auto baseline = loadFields(*document.source);
    if (!baseline)
    {
        return osp::makeUnexpected(SaveError{.code = SaveError::Code::Render, .message = baseline.error().message});
    }
    osp::Patch patch;
    detail::PatchEditor editor(*document.source, document.source->root(), patch, LIBRARY_ORDER);
    applyChanges(editor, *baseline, library);
    return verified(osp::render(*document.source, patch), library);
}

osp::Expected<std::string, SaveError> writeLibrary(const Library& library, osp::LineEnding lineEnding)
{
    return verified(osp::write(libraryObjectValue(library), osp::WriteOptions{.lineEnding = lineEnding}), library);
}

} // namespace motrack::library
