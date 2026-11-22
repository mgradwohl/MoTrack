#pragma once

#include "library/java_string.hpp"
#include "osp/source.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// Typed model of Open Source Physics Digital Library resources and collections (`LibraryResource`,
/// `LibraryCollection`) as OSP d56207b (Tracker 6.3.5) holds them after loading. The model has no Qt, network or
/// file-system dependency; the Library Browser (slice I6) supplies I/O. docs/library-collections.md describes
/// the semantics and the deliberate differences.
namespace motrack::library
{

/// OSP class names written in collection files.
inline constexpr std::string_view RESOURCE_CLASS = "org.opensourcephysics.tools.LibraryResource";
inline constexpr std::string_view COLLECTION_CLASS = "org.opensourcephysics.tools.LibraryCollection";
inline constexpr std::string_view LIBRARY_CLASS = "org.opensourcephysics.tools.Library";
/// The array class OSP writes for `resources`, whatever the elements' classes are.
inline constexpr std::string_view RESOURCE_ARRAY_CLASS = "[Lorg.opensourcephysics.tools.LibraryResource;";

/// LibraryResource type constants (line 75). `Unknown` is the default and is never written.
enum class ResourceType : std::uint8_t
{
    Unknown,
    Collection,
    Tracker,
    Ejs,
    Video,
    Image,
    Html,
    Pdf,
    Data,
    Url,
};

/// The type name as OSP writes it: "Unknown", "Collection", "Tracker", "EJS", "Video", "Image", "HTML", "PDF",
/// "Data" or "URL".
[[nodiscard]] std::string_view resourceTypeName(ResourceType type) noexcept;
/// LibraryResource.setType accepts only these exact (case-sensitive) names.
[[nodiscard]] std::optional<ResourceType> resourceTypeFromName(std::string_view name) noexcept;

enum class ResourceKind : std::uint8_t
{
    Resource,   ///< org.opensourcephysics.tools.LibraryResource
    Collection, ///< org.opensourcephysics.tools.LibraryCollection
};

/// A LibraryResource.Metadata key/value pair. Resources keep metadata in a `TreeSet` ordered by key, then value,
/// with Java string order, so equal pairs collapse.
struct Metadata
{
    std::string key;
    std::string value;

    friend bool operator==(const Metadata&, const Metadata&) = default;
};

/// TreeSet<Metadata> order (Metadata.compareTo).
[[nodiscard]] bool metadataLess(const Metadata& left, const Metadata& right) noexcept;

/// The parsed object a resource was loaded from. Saving uses it to keep the original bytes and every property
/// MoTrack does not model. A resource built in memory has no origin.
struct SourceOrigin
{
    std::shared_ptr<const osp::Source> source;
    osp::NodeId object{};

    friend bool operator==(const SourceOrigin&, const SourceOrigin&) = default;
};

/// `TreeMap<String, String>` with Java string order, stored as a sorted vector so that moving it never allocates.
/// Values can be null.
class PropertyMap
{
  public:
    using Entry = std::pair<std::string, std::optional<std::string>>;

    /// TreeMap.put: replaces the value of an existing key.
    void set(std::string key, std::optional<std::string> value);
    /// The entry's value, or nullptr when the key is absent.
    [[nodiscard]] const std::optional<std::string>* find(std::string_view key) const noexcept;
    /// The value of a present key; throws std::out_of_range otherwise.
    [[nodiscard]] const std::optional<std::string>& at(std::string_view key) const;
    [[nodiscard]] bool contains(std::string_view key) const noexcept
    {
        return find(key) != nullptr;
    }
    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Entries.size();
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return m_Entries.empty();
    }
    void clear() noexcept
    {
        m_Entries.clear();
    }
    [[nodiscard]] std::vector<Entry>::const_iterator begin() const noexcept
    {
        return m_Entries.begin();
    }
    [[nodiscard]] std::vector<Entry>::const_iterator end() const noexcept
    {
        return m_Entries.end();
    }

    friend bool operator==(const PropertyMap&, const PropertyMap&) = default;

  private:
    std::vector<Entry> m_Entries;
};

/// Well-known keys of `Resource::properties`.
inline constexpr std::string_view DOWNLOAD_FILENAME_PROPERTY = "download_filename"; ///< Set by ComPADRE records.
inline constexpr std::string_view RELOAD_URL_PROPERTY = "reload_url";               ///< ComPADRE query that listed it.

/// A LibraryResource or LibraryCollection with the field values OSP's loader leaves in the Java object.
///
/// Invariants maintained by the functions below (and by loading): `name`, `description`, `basePath` and
/// `htmlPath` are trimmed; `metadata` is sorted by `metadataLess` without duplicates; a collection's `type` is
/// `Unknown` (LibraryCollection.setType never changes it; `effectiveType` reports `Collection`). `target` is
/// kept exactly as loaded: LibraryResource's loader assigns it without trimming.
struct Resource
{
    ResourceKind kind = ResourceKind::Resource;
    std::string name{};
    std::string description{}; ///< HTML shown when there is no HTML page.
    std::string basePath{};    ///< Base for relative targets and HTML paths of this resource and its descendants.
    std::string htmlPath{};    ///< Relative or absolute path of the page that describes the resource.
    std::string target{};      ///< Relative or absolute path or URL of the file, or a ComPADRE query.
    ResourceType type = ResourceType::Unknown;
    /// Runtime only: OSP neither saves nor loads it. ComPADRE records set it to their thumbnail URL.
    std::optional<std::string> thumbnail{};
    std::vector<Metadata> metadata{};
    /// Arbitrary string properties (TreeMap); a value can be null.
    PropertyMap properties{};
    std::vector<Resource> children{}; ///< Collections only, in order.
    std::optional<SourceOrigin> origin{};

    [[nodiscard]] bool isCollection() const noexcept
    {
        return kind == ResourceKind::Collection;
    }
};

/// LibraryResource.getType: `Collection` for collections, otherwise `type`.
[[nodiscard]] ResourceType effectiveType(const Resource& resource) noexcept;
/// LibraryResource.getTarget: the target, or nullopt when it is empty.
[[nodiscard]] std::optional<std::string_view> targetOf(const Resource& resource) noexcept;

/// Equality of everything OSP saves or loads, recursively: ignores `origin` and `thumbnail`.
[[nodiscard]] bool sameContent(const Resource& left, const Resource& right);

/// Builders following LibraryResource's/LibraryCollection's constructors (the name is trimmed).
[[nodiscard]] Resource makeResource(std::string_view name);
[[nodiscard]] Resource makeCollection(std::string_view name);

// Setters with OSP's side effects. Each returns true when the value changed.

/// LibraryResource.setName: trims.
bool setName(Resource& resource, std::string_view name);
/// LibraryResource.setDescription: trims.
bool setDescription(Resource& resource, std::string_view description);
/// LibraryResource.setBasePath: trims.
bool setBasePath(Resource& resource, std::string_view path);

/// Classifies a target extension the way VideoIO.getVideoTypesForPath does for the registered video types;
/// return Image, Video or nullopt (unknown). MoTrack's media layer supplies it, so a library target is
/// classified by what MoTrack can actually open.
using MediaTypeLookup = std::function<std::optional<ResourceType>(std::string_view lowerCaseExtension)>;

/// LibraryResource.getTypeFromPath (line 250). Paths shorter than four characters are classified as HTML (OSP
/// passes the never-null HTML path). Otherwise the lower-cased extension decides: "html" HTML; "trk"/"trz" and
/// any extension containing "&trackerset" Tracker; "pdf" PDF; "ejs" EJS; "zip", none or "" Unknown; anything
/// else asks `media`.
[[nodiscard]] ResourceType typeFromPath(std::string_view path, const MediaTypeLookup& media);

/// LibraryResource.setTarget: trims, clears the thumbnail and adopts `typeFromPath` unless it is Unknown.
/// LibraryCollection.setTarget only trims.
bool setTarget(Resource& resource, std::string_view path, const MediaTypeLookup& media);
/// LibraryResource.setHTMLPath: trims; a non-collection without a target becomes HTML, or Unknown when the
/// path is cleared.
bool setHtmlPath(Resource& resource, std::string_view path);
/// LibraryResource.setType: only the exact names of `resourceTypeName` are accepted; collections ignore it.
bool setType(Resource& resource, ResourceType type);

/// LibraryResource.addMetadata: drops keys that web page builders add (generator, progid, originator,
/// referrer, theme-color, color-scheme, viewport, googlebot, robots; ASCII case-insensitive), renames the exact
/// keys "author", "contact" and "keywords" to "Author", "Contact" and "Keywords", and inserts into the ordered
/// set. Returns true when added.
bool addMetadata(Resource& resource, Metadata data);
/// LibraryResource.removeMetadata.
bool removeMetadata(Resource& resource, const Metadata& data);
/// LibraryResource.setProperty.
void setProperty(Resource& resource, std::string key, std::optional<std::string> value);

/// LibraryResource.hasExternalHTML: false when the HTML path starts with the lower-cased target followed by
/// '!', meaning a page inside the target archive, which OSP derives again when it opens the archive and so
/// does not save. Note OSP lower-cases only the target, so "Ball.trz!/..." with target "Ball.trz" is external.
[[nodiscard]] bool hasExternalHtml(const Resource& resource);

/// Path resolution through a resource's ancestry: `lineage` runs from the outermost collection to the resource
/// itself (the resource is `lineage.back()`).

/// LibraryResource.getInheritedBasePath: the first non-empty base path from the resource up to the root.
[[nodiscard]] std::string inheritedBasePath(std::span<const Resource* const> lineage);
/// LibraryResource.getAbsoluteTarget: "" without a target, else `resolvedPath(target, inheritedBasePath)`.
[[nodiscard]] std::string absoluteTarget(std::span<const Resource* const> lineage);
/// LibraryResource.getAbsoluteHTMLPath: "" without an HTML path, else resolved like the target.
[[nodiscard]] std::string absoluteHtmlPath(std::span<const Resource* const> lineage);

/// What the Library Browser shows (LibraryTreeNode): the base path falls back from the resource's ancestors to
/// the directory of the collection file or URL (`documentPath`), and a collection that has children
/// concatenates base path and target without a separator (ComPADRE query parameters).
[[nodiscard]] std::string browserBasePath(std::span<const Resource* const> lineage, std::string_view documentPath);
[[nodiscard]] std::optional<std::string> browserAbsoluteTarget(std::span<const Resource* const> lineage, std::string_view documentPath);
[[nodiscard]] std::optional<std::string> browserHtmlPath(std::span<const Resource* const> lineage, std::string_view documentPath);

} // namespace motrack::library
