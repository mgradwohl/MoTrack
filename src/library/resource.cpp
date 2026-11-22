#include "library/resource.hpp"

#include "library/paths.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace motrack::library
{
namespace
{

constexpr std::array<std::string_view, 10> TYPE_NAMES = {
    "Unknown", "Collection", "Tracker", "EJS", "Video", "Image", "HTML", "PDF", "Data", "URL"};

/// Keys LibraryResource.addMetadata drops because web page builders add them.
constexpr std::array<std::string_view, 9> EXCLUDED_METADATA = {
    "generator", "progid", "originator", "referrer", "theme-color", "color-scheme", "viewport", "googlebot", "robots"};

/// LibraryResource.META_TYPES.
constexpr std::array<std::string_view, 3> STANDARD_METADATA = {"Author", "Contact", "Keywords"};

/// String.length(): the number of UTF-16 code units of valid UTF-8 text.
std::size_t utf16Length(std::string_view text) noexcept
{
    std::size_t length = 0;
    for (const auto byte : text)
    {
        const auto value = static_cast<unsigned char>(byte);
        if ((value & 0xC0U) != 0x80U)
        {
            length += value >= 0xF0U ? 2 : 1;
        }
    }
    return length;
}

} // namespace

void PropertyMap::set(std::string key, std::optional<std::string> value)
{
    const auto position = std::ranges::lower_bound(
        m_Entries, key, [](std::string_view left, std::string_view right) { return javaCompare(left, right) < 0; }, &Entry::first);
    if (position != m_Entries.end() && position->first == key)
    {
        position->second = std::move(value);
        return;
    }
    m_Entries.emplace(position, std::move(key), std::move(value));
}

const std::optional<std::string>* PropertyMap::find(std::string_view key) const noexcept
{
    const auto position = std::ranges::lower_bound(
        m_Entries, key, [](std::string_view left, std::string_view right) { return javaCompare(left, right) < 0; }, &Entry::first);
    return position != m_Entries.end() && position->first == key ? &position->second : nullptr;
}

const std::optional<std::string>& PropertyMap::at(std::string_view key) const
{
    const auto* value = find(key);
    if (value == nullptr)
    {
        throw std::out_of_range("no property named \"" + std::string(key) + "\"");
    }
    return *value;
}
std::string_view resourceTypeName(ResourceType type) noexcept
{
    return TYPE_NAMES.at(static_cast<std::size_t>(type));
}

std::optional<ResourceType> resourceTypeFromName(std::string_view name) noexcept
{
    for (std::size_t index = 0; index < TYPE_NAMES.size(); ++index)
    {
        if (TYPE_NAMES.at(index) == name)
        {
            return static_cast<ResourceType>(index);
        }
    }
    return std::nullopt;
}

bool metadataLess(const Metadata& left, const Metadata& right) noexcept
{
    const auto keys = javaCompare(left.key, right.key);
    return keys != 0 ? keys < 0 : javaCompare(left.value, right.value) < 0;
}

ResourceType effectiveType(const Resource& resource) noexcept
{
    return resource.isCollection() ? ResourceType::Collection : resource.type;
}

std::optional<std::string_view> targetOf(const Resource& resource) noexcept
{
    if (resource.target.empty())
    {
        return std::nullopt;
    }
    return std::string_view(resource.target);
}

bool sameContent(const Resource& left, const Resource& right)
{
    if (left.kind != right.kind || left.name != right.name || left.description != right.description || left.basePath != right.basePath ||
        left.htmlPath != right.htmlPath || left.target != right.target || left.type != right.type || left.metadata != right.metadata ||
        left.properties != right.properties || left.children.size() != right.children.size())
    {
        return false;
    }
    return std::ranges::equal(left.children, right.children, [](const Resource& a, const Resource& b) { return sameContent(a, b); });
}

Resource makeResource(std::string_view name)
{
    Resource resource;
    setName(resource, name);
    return resource;
}

Resource makeCollection(std::string_view name)
{
    Resource collection;
    collection.kind = ResourceKind::Collection;
    setName(collection, name);
    return collection;
}

bool setName(Resource& resource, std::string_view name)
{
    auto trimmed = javaTrimmed(name);
    if (trimmed == resource.name)
    {
        return false;
    }
    resource.name = std::move(trimmed);
    return true;
}

bool setDescription(Resource& resource, std::string_view description)
{
    auto trimmed = javaTrimmed(description);
    if (trimmed == resource.description)
    {
        return false;
    }
    resource.description = std::move(trimmed);
    return true;
}

bool setBasePath(Resource& resource, std::string_view path)
{
    auto trimmed = javaTrimmed(path);
    if (trimmed == resource.basePath)
    {
        return false;
    }
    resource.basePath = std::move(trimmed);
    return true;
}

ResourceType typeFromPath(std::string_view path, const MediaTypeLookup& media)
{
    std::string ext;
    if (utf16Length(path) < 4)
    {
        // OSP passes getHTMLPath(), which is never null, so short paths take the "html" branch.
        ext = "html";
    }
    else if (const auto found = extension(path))
    {
        ext = asciiLower(*found);
    }
    if (ext.find("&trackerset") != std::string::npos)
    {
        return ResourceType::Tracker;
    }
    if (ext == "html")
    {
        return ResourceType::Html;
    }
    if (ext == "trk" || ext == "trz")
    {
        return ResourceType::Tracker;
    }
    if (ext == "pdf")
    {
        return ResourceType::Pdf;
    }
    if (ext == "ejs")
    {
        return ResourceType::Ejs;
    }
    if (ext.empty() || ext == "zip" || !media)
    {
        return ResourceType::Unknown;
    }
    const auto kind = media(ext);
    if (kind == ResourceType::Image || kind == ResourceType::Video)
    {
        return *kind;
    }
    return ResourceType::Unknown;
}

bool setTarget(Resource& resource, std::string_view path, const MediaTypeLookup& media)
{
    auto trimmed = javaTrimmed(path);
    if (trimmed == resource.target)
    {
        return false;
    }
    if (resource.isCollection())
    {
        resource.target = std::move(trimmed);
        return true;
    }
    resource.thumbnail.reset();
    resource.target = std::move(trimmed);
    if (const auto type = typeFromPath(resource.target, media); type != ResourceType::Unknown)
    {
        setType(resource, type);
    }
    return true;
}

bool setHtmlPath(Resource& resource, std::string_view path)
{
    auto trimmed = javaTrimmed(path);
    if (trimmed == resource.htmlPath)
    {
        return false;
    }
    resource.htmlPath = std::move(trimmed);
    if (!resource.isCollection() && resource.target.empty())
    {
        setType(resource, resource.htmlPath.empty() ? ResourceType::Unknown : ResourceType::Html);
    }
    return true;
}

bool setType(Resource& resource, ResourceType type)
{
    if (resource.isCollection() || resource.type == type)
    {
        return false;
    }
    resource.type = type;
    return true;
}

bool addMetadata(Resource& resource, Metadata data)
{
    const auto key = asciiLower(data.key);
    if (std::ranges::find(EXCLUDED_METADATA, key) != EXCLUDED_METADATA.end())
    {
        return false;
    }
    for (const auto standard : STANDARD_METADATA)
    {
        // OSP compares type.toLowerCase() with the key exactly, so only the all-lower-case spelling is renamed.
        if (asciiLower(standard) == data.key)
        {
            data.key = std::string(standard);
        }
    }
    const auto position = std::ranges::lower_bound(resource.metadata, data, metadataLess);
    if (position != resource.metadata.end() && *position == data)
    {
        return false;
    }
    resource.metadata.insert(position, std::move(data));
    return true;
}

bool removeMetadata(Resource& resource, const Metadata& data)
{
    const auto position = std::ranges::find(resource.metadata, data);
    if (position == resource.metadata.end())
    {
        return false;
    }
    resource.metadata.erase(position);
    return true;
}

void setProperty(Resource& resource, std::string key, std::optional<std::string> value)
{
    resource.properties.set(std::move(key), std::move(value));
}

bool hasExternalHtml(const Resource& resource)
{
    if (resource.target.empty())
    {
        return true;
    }
    return !resource.htmlPath.starts_with(asciiLower(resource.target) + "!");
}

std::string inheritedBasePath(std::span<const Resource* const> lineage)
{
    for (const auto* resource : std::views::reverse(lineage))
    {
        if (!resource->basePath.empty())
        {
            return resource->basePath;
        }
    }
    return {};
}

std::string absoluteTarget(std::span<const Resource* const> lineage)
{
    const auto& resource = *lineage.back();
    return resource.target.empty() ? std::string{} : resolvedPath(resource.target, inheritedBasePath(lineage));
}

std::string absoluteHtmlPath(std::span<const Resource* const> lineage)
{
    const auto& resource = *lineage.back();
    return resource.htmlPath.empty() ? std::string{} : resolvedPath(resource.htmlPath, inheritedBasePath(lineage));
}

std::string browserBasePath(std::span<const Resource* const> lineage, std::string_view documentPath)
{
    const auto inherited = inheritedBasePath(lineage);
    return inherited.empty() ? directoryPath(documentPath) : inherited;
}

std::optional<std::string> browserAbsoluteTarget(std::span<const Resource* const> lineage, std::string_view documentPath)
{
    const auto& resource = *lineage.back();
    if (resource.target.empty())
    {
        return std::nullopt;
    }
    const auto base = browserBasePath(lineage, documentPath);
    // LibraryBrowser.isPopulatedCollection: a collection node with child nodes.
    if (resource.isCollection() && !resource.children.empty())
    {
        return base + resource.target;
    }
    return resolvedPath(resource.target, base);
}

std::optional<std::string> browserHtmlPath(std::span<const Resource* const> lineage, std::string_view documentPath)
{
    const auto& resource = *lineage.back();
    if (javaTrimmed(resource.htmlPath).empty())
    {
        return std::nullopt;
    }
    return resolvedPath(resource.htmlPath, browserBasePath(lineage, documentPath));
}

} // namespace motrack::library
