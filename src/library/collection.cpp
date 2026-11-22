#include "library/collection.hpp"

#include "library/osp_values.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace motrack::library
{
namespace
{

using detail::Failure;
using detail::JavaValue;
using detail::Result;

/// The order in which LibraryResource.Loader and LibraryCollection.Loader write properties.
constexpr std::array<std::string_view, 9> RESOURCE_ORDER = {
    "name", "description", "html_path", "base_path", "target", "type", "metadata", "properties", "resources"};

constexpr std::string_view COLLECTION_ARRAY_CLASS = "[Lorg.opensourcephysics.tools.LibraryCollection;";

/// Whether a saved HTML path inside the target archive is written. OSP's writer omits it (hasExternalHTML) and
/// derives it again when the archive is opened; MoTrack's save keeps an existing one so that saving loses nothing.
enum class HtmlRule : std::uint8_t
{
    Osp,
    Keep,
};

std::string_view classOf(ResourceKind kind) noexcept
{
    return kind == ResourceKind::Collection ? COLLECTION_CLASS : RESOURCE_CLASS;
}

bool isResourceClass(std::string_view className) noexcept
{
    return className == RESOURCE_CLASS || className == COLLECTION_CLASS;
}

bool htmlWritten(const Resource& resource, HtmlRule rule)
{
    return !resource.htmlPath.empty() && (rule == HtmlRule::Keep || hasExternalHtml(resource));
}

/// A resource with an HTML page and no target loads as HTML unless its type is written. OSP's writer omits
/// Unknown, so MoTrack's save writes it explicitly to keep such a resource Unknown.
bool needsExplicitUnknown(const Resource& resource) noexcept
{
    return !resource.isCollection() && resource.type == ResourceType::Unknown && !resource.htmlPath.empty() && resource.target.empty();
}

bool typeWritten(const Resource& resource, HtmlRule rule) noexcept
{
    return resource.type != ResourceType::Unknown || (rule == HtmlRule::Keep && needsExplicitUnknown(resource));
}

osp::PropertyValue metadataProperty(const Resource& resource)
{
    detail::StringRows rows;
    rows.reserve(resource.metadata.size());
    for (const auto& item : resource.metadata)
    {
        rows.emplace_back(detail::StringList{item.key, item.value});
    }
    return detail::stringMatrixProperty("metadata", rows);
}

osp::PropertyValue propertiesProperty(const Resource& resource)
{
    detail::StringRows rows;
    rows.reserve(resource.properties.size());
    for (const auto& [key, value] : resource.properties)
    {
        rows.emplace_back(detail::StringList{key, value});
    }
    return detail::stringMatrixProperty("properties", rows);
}

/// Produces the object written for a child resource.
using ChildWriter = std::function<osp::ObjectValue(const Resource&)>;

osp::ObjectValue objectValueOf(const Resource& resource, HtmlRule rule, const ChildWriter& childValue)
{
    osp::ObjectValue object{.className = std::string(classOf(resource.kind))};
    auto& properties = object.properties;
    properties.push_back(osp::stringProperty("name", resource.name));
    if (!resource.description.empty())
    {
        properties.push_back(osp::stringProperty("description", resource.description));
    }
    if (htmlWritten(resource, rule))
    {
        properties.push_back(osp::stringProperty("html_path", resource.htmlPath));
    }
    if (!resource.basePath.empty())
    {
        properties.push_back(osp::stringProperty("base_path", resource.basePath));
    }
    if (!resource.target.empty())
    {
        properties.push_back(osp::stringProperty("target", resource.target));
    }
    if (typeWritten(resource, rule))
    {
        properties.push_back(osp::stringProperty("type", resourceTypeName(resource.type)));
    }
    if (!resource.metadata.empty())
    {
        properties.push_back(metadataProperty(resource));
    }
    if (!resource.properties.empty())
    {
        properties.push_back(propertiesProperty(resource));
    }
    if (resource.isCollection() && !resource.children.empty())
    {
        std::vector<std::optional<osp::PropertyValue>> elements;
        elements.reserve(resource.children.size());
        for (const auto& child : resource.children)
        {
            elements.emplace_back(osp::objectProperty("", childValue(child)));
        }
        properties.push_back(osp::arrayProperty("resources", std::string(RESOURCE_ARRAY_CLASS), elements));
    }
    return object;
}

/// OSP's writer: every resource in the layout LibraryResource.Loader/LibraryCollection.Loader write.
osp::ObjectValue writtenValue(const Resource& resource, HtmlRule rule)
{
    return objectValueOf(resource, rule, [rule](const Resource& child) { return writtenValue(child, rule); });
}

/// Reads LibraryResource/LibraryCollection objects with OSP's loader semantics.
class Loader
{
  public:
    Loader(std::shared_ptr<const osp::Source> source, std::vector<Diagnostic>* diagnostics)
        : m_Source(std::move(source)), m_Diagnostics(diagnostics)
    {}

    Result<Resource> load(const osp::ObjectView& object, const std::string& location)
    {
        std::vector<Failure> ignored;
        const detail::ValueReader reader([this](const osp::ObjectView& child, const std::string& childLocation)
                                         { return loadChild(child, childLocation); },
                                         m_Diagnostics != nullptr ? &ignored : nullptr);
        auto resource = loadFields(reader, object, location);
        if (resource && m_Diagnostics != nullptr)
        {
            for (auto& note : ignored)
            {
                m_Diagnostics->push_back(Diagnostic{.location = std::move(note.location), .message = std::move(note.message)});
            }
        }
        return resource;
    }

  private:
    Result<Resource> loadChild(const osp::ObjectView& object, const std::string& location)
    {
        return load(object, location);
    }

    Result<Resource> loadFields(const detail::ValueReader& reader, const osp::ObjectView& object, const std::string& location)
    {
        // createObject: new LibraryResource(control.getString("name")).
        auto name = reader.getString(object, "name", location);
        if (!name)
        {
            return osp::makeUnexpected(name.error());
        }
        auto resource = object.className() == COLLECTION_CLASS ? makeCollection(name->value_or("")) : makeResource(name->value_or(""));
        auto description = reader.getString(object, "description", location);
        if (!description)
        {
            return osp::makeUnexpected(description.error());
        }
        setDescription(resource, description->value_or(""));
        auto basePath = reader.getString(object, "base_path", location);
        if (!basePath)
        {
            return osp::makeUnexpected(basePath.error());
        }
        setBasePath(resource, basePath->value_or(""));
        auto target = reader.getString(object, "target", location);
        if (!target)
        {
            return osp::makeUnexpected(target.error());
        }
        if (*target)
        {
            // The loader assigns the field directly: no trimming and no type from the extension.
            resource.target = **target;
        }
        auto htmlPath = reader.getString(object, "html_path", location);
        if (!htmlPath)
        {
            return osp::makeUnexpected(htmlPath.error());
        }
        setHtmlPath(resource, htmlPath->value_or(""));
        auto type = reader.getString(object, "type", location);
        if (!type)
        {
            return osp::makeUnexpected(type.error());
        }
        if (*type)
        {
            if (const auto known = resourceTypeFromName(**type))
            {
                setType(resource, *known);
            }
        }
        if (auto loaded = loadMetadata(reader, object, location, resource); !loaded)
        {
            return osp::makeUnexpected(loaded.error());
        }
        if (auto loaded = loadProperties(reader, object, location, resource); !loaded)
        {
            return osp::makeUnexpected(loaded.error());
        }
        if (resource.isCollection())
        {
            if (auto loaded = loadResources(reader, object, location, resource); !loaded)
            {
                return osp::makeUnexpected(loaded.error());
            }
        }
        resource.origin = SourceOrigin{.source = m_Source, .object = object.id()};
        return resource;
    }

    static Result<bool>
    loadMetadata(const detail::ValueReader& reader, const osp::ObjectView& object, const std::string& location, Resource& resource)
    {
        const auto here = location + ".metadata";
        auto value = reader.getObject(object, "metadata", location);
        if (!value)
        {
            return osp::makeUnexpected(value.error());
        }
        auto rows = detail::castStringMatrix(*value, here);
        if (!rows)
        {
            return osp::makeUnexpected(rows.error());
        }
        if (!*rows)
        {
            return true;
        }
        for (const auto& row : **rows)
        {
            if (!row)
            {
                return detail::fail(here, "null metadata entry");
            }
            if (row->size() < 2)
            {
                return detail::fail(here, "metadata entry without a value");
            }
            addMetadata(resource, Metadata{.key = (*row)[0].value_or(""), .value = (*row)[1].value_or("")});
        }
        return true;
    }

    static Result<bool>
    loadProperties(const detail::ValueReader& reader, const osp::ObjectView& object, const std::string& location, Resource& resource)
    {
        const auto here = location + ".properties";
        auto value = reader.getObject(object, "properties", location);
        if (!value)
        {
            return osp::makeUnexpected(value.error());
        }
        auto rows = detail::castStringMatrix(*value, here);
        if (!rows)
        {
            return osp::makeUnexpected(rows.error());
        }
        if (!*rows)
        {
            return true;
        }
        for (const auto& row : **rows)
        {
            if (!row)
            {
                return detail::fail(here, "null property entry");
            }
            if (row->size() < 2)
            {
                return detail::fail(here, "property entry without a value");
            }
            const auto& key = (*row)[0];
            if (!key)
            {
                return detail::fail(here, "property without a name");
            }
            setProperty(resource, *key, (*row)[1]);
        }
        return true;
    }

    Result<bool>
    loadResources(const detail::ValueReader& reader, const osp::ObjectView& object, const std::string& location, Resource& collection)
    {
        const auto here = location + ".resources";
        auto value = reader.getObject(object, "resources", location);
        if (!value)
        {
            return osp::makeUnexpected(value.error());
        }
        switch (value->kind)
        {
        case JavaValue::Kind::Null:
        {
            // Without resources the collection target is set again, which trims it.
            auto target = reader.getString(object, "target", location);
            if (!target)
            {
                return osp::makeUnexpected(target.error());
            }
            setTarget(collection, target->value_or(""), {});
            return true;
        }
        case JavaValue::Kind::Array:
            if (value->arrayClass != RESOURCE_ARRAY_CLASS && value->arrayClass != COLLECTION_ARRAY_CLASS)
            {
                return detail::fail(here, "not a LibraryResource[] array");
            }
            break;
        case JavaValue::Kind::Collection:
            // OSP's swingJS tip reads an ArrayList; Tracker 6.3.5 throws ClassCastException.
            if (value->arrayClass != "java.util.ArrayList")
            {
                return detail::fail(here, "not a LibraryResource[] array");
            }
            if (m_Diagnostics != nullptr)
            {
                m_Diagnostics->push_back(Diagnostic{.location = here,
                                                    .message = "resources are stored as a collection, which Tracker 6.3.5 cannot read; "
                                                               "saving changes to this list writes an array"});
            }
            break;
        default:
            return detail::fail(here, "not a LibraryResource[] array");
        }
        for (auto& element : value->elements)
        {
            if (element.kind == JavaValue::Kind::Resource)
            {
                collection.children.push_back(std::move(element.resource));
            }
            else if (element.kind != JavaValue::Kind::Null)
            {
                return detail::fail(here, "collection element is not a LibraryResource");
            }
        }
        return true;
    }

    std::shared_ptr<const osp::Source> m_Source;
    std::vector<Diagnostic>* m_Diagnostics;
};

/// The resource an object loads as. Saving compares against it; the object loaded before, so this succeeds
/// unless the origin is stale.
Result<Resource> baselineLoad(const SourceOrigin& origin)
{
    Loader loader(origin.source, nullptr);
    return loader.load(osp::ObjectView(*origin.source, origin.object), "root");
}

/// Whether saving keeps a childless collection's original `resources` property, and OSP reads it as an array or
/// list (even an empty one). The loader then leaves the collection's target untrimmed.
bool keepsDeclaredResources(const Resource& collection)
{
    if (!collection.origin)
    {
        return false;
    }
    const auto baseline = baselineLoad(*collection.origin);
    if (!baseline || !baseline->children.empty() || baseline->kind != ResourceKind::Collection)
    {
        return false;
    }
    const detail::ValueReader reader([](const osp::ObjectView&, const std::string&) -> Result<Resource> { return Resource{}; });
    const auto value = reader.getObject(osp::ObjectView(*collection.origin->source, collection.origin->object), "resources", "root");
    return value && value->kind != JavaValue::Kind::Null;
}

Resource reloadedForm(const Resource& resource, HtmlRule rule)
{
    Resource result;
    result.kind = resource.kind;
    setName(result, resource.name);
    setDescription(result, resource.description);
    setBasePath(result, resource.basePath);
    result.target = resource.target;
    const auto htmlPath = htmlWritten(resource, rule) ? std::string_view(resource.htmlPath) : std::string_view{};
    if (rule == HtmlRule::Keep)
    {
        // MoTrack's save writes whatever type the loader would not infer.
        result.htmlPath = javaTrimmed(htmlPath);
        result.type = resource.isCollection() ? ResourceType::Unknown : resource.type;
    }
    else
    {
        setHtmlPath(result, htmlPath);
        if (resource.type != ResourceType::Unknown)
        {
            setType(result, resource.type);
        }
    }
    for (const auto& item : resource.metadata)
    {
        addMetadata(result, item);
    }
    result.properties = resource.properties;
    if (resource.isCollection())
    {
        if (resource.children.empty() && !(rule == HtmlRule::Keep && keepsDeclaredResources(resource)))
        {
            setTarget(result, resource.target, {});
        }
        for (const auto& child : resource.children)
        {
            result.children.push_back(reloadedForm(child, rule));
        }
    }
    return result;
}

/// Elements of a `resources` array that OSP ignores (objects of other classes); kept when the array is rewritten.
std::vector<osp::PropertyValue> opaqueElements(const osp::Source& source, osp::NodeId object)
{
    std::vector<osp::PropertyValue> opaque;
    const auto property = osp::ObjectView(source, object).property("resources");
    if (!property || property->type() != osp::PropertyType::Array)
    {
        return opaque;
    }
    for (const auto& element : property->elements())
    {
        if (const auto child = element.object(); child && !isResourceClass(child->className()))
        {
            opaque.push_back(osp::propertyValue(source, element.id()));
        }
    }
    return opaque;
}

class Saver
{
  public:
    explicit Saver(const CollectionDocument& document) : m_Document(&document)
    {}

    osp::Expected<std::string, SaveError> save()
    {
        const auto& document = *m_Document;
        const auto& root = document.root;
        osp::Expected<std::string, osp::RenderError> output = std::string{};
        const bool bound =
            document.source && root.origin && root.origin->source == document.source && root.origin->object == document.source->root();
        if (bound && diffInPlace(root, document.source->root()))
        {
            output = osp::render(*document.source, m_Patch);
        }
        else
        {
            const auto ending = document.source ? document.source->lineEnding() : osp::LineEnding::Lf;
            output = osp::write(merged(root), osp::WriteOptions{.lineEnding = ending});
        }
        if (!output)
        {
            return osp::makeUnexpected(SaveError{.code = SaveError::Code::Render, .message = output.error().message});
        }
        auto reloaded = loadCollection(*output);
        if (!reloaded || !sameContent(reloaded->root, reloadedForm(root, HtmlRule::Keep)))
        {
            return osp::makeUnexpected(SaveError{.code = SaveError::Code::VerificationFailed,
                                                 .message = reloaded
                                                                ? "the saved collection reloads with different content"
                                                                : "the saved collection cannot be reloaded: " + reloaded.error().message});
        }
        return *std::move(output);
    }

  private:
    [[nodiscard]] const osp::Source& source() const
    {
        return *m_Document->source;
    }

    /// The baseline of an object of the document's source, from one load of the whole document.
    [[nodiscard]] const Resource* baselineOf(osp::NodeId object)
    {
        if (!m_Indexed)
        {
            m_Indexed = true;
            auto loaded = baselineLoad(SourceOrigin{.source = m_Document->source, .object = m_Document->source->root()});
            if (loaded)
            {
                m_Baseline = *std::move(loaded);
                index(m_Baseline);
            }
        }
        const auto found = m_Baselines.find(object);
        return found == m_Baselines.end() ? nullptr : found->second;
    }

    void index(const Resource& resource)
    {
        if (resource.origin)
        {
            m_Baselines.emplace(resource.origin->object, &resource);
        }
        for (const auto& child : resource.children)
        {
            index(child);
        }
    }

    /// Records the edits for `current` against the object it was loaded from; false when the object's class
    /// changed, so the caller must replace it.
    bool diffInPlace(const Resource& current, osp::NodeId object)
    {
        const auto* baseline = baselineOf(object);
        if (baseline == nullptr || baseline->kind != current.kind)
        {
            return false;
        }
        detail::PatchEditor editor(source(), object, m_Patch, RESOURCE_ORDER);
        applyFields(editor, *baseline, current);
        if (current.isCollection())
        {
            diffChildren(editor, *baseline, current, object);
        }
        return true;
    }

    static void applyFields(detail::PropertyEditor& editor, const Resource& baseline, const Resource& current)
    {
        if (current.name != baseline.name)
        {
            editor.setString("name", current.name);
        }
        const auto optional = [&editor](std::string_view name, const std::string& before, const std::string& after)
        {
            if (after == before)
            {
                return;
            }
            if (after.empty())
            {
                editor.removeAll(name);
            }
            else
            {
                editor.setString(name, after);
            }
        };
        optional("description", baseline.description, current.description);
        optional("html_path", baseline.htmlPath, current.htmlPath);
        optional("base_path", baseline.basePath, current.basePath);
        optional("target", baseline.target, current.target);
        // The loaded type also depends on the HTML path and target, so all three decide the type property.
        if (!current.isCollection() &&
            (current.type != baseline.type || current.htmlPath != baseline.htmlPath || current.target != baseline.target))
        {
            if (typeWritten(current, HtmlRule::Keep))
            {
                editor.setString("type", std::string(resourceTypeName(current.type)));
            }
            else
            {
                editor.removeAll("type");
            }
        }
        if (current.metadata != baseline.metadata)
        {
            if (current.metadata.empty())
            {
                editor.removeAll("metadata");
            }
            else
            {
                editor.setProperty(metadataProperty(current));
            }
        }
        if (current.properties != baseline.properties)
        {
            if (current.properties.empty())
            {
                editor.removeAll("properties");
            }
            else
            {
                editor.setProperty(propertiesProperty(current));
            }
        }
    }

    void diffChildren(detail::PropertyEditor& editor, const Resource& baseline, const Resource& current, osp::NodeId object)
    {
        if (current.children.empty())
        {
            if (!baseline.children.empty())
            {
                editor.removeAll("resources");
            }
            return;
        }
        if (keepsChildren(baseline, current))
        {
            for (const auto& child : current.children)
            {
                editChild(child);
            }
            return;
        }
        if (appendInPlace(baseline, current, object))
        {
            return;
        }
        editor.setProperty(resourcesProperty(current, m_Document->source.get(), object));
    }

    /// The same children, from the same objects, in the same order: each is edited where it is, whatever form the
    /// list has (array or collection).
    [[nodiscard]] bool keepsChildren(const Resource& baseline, const Resource& current) const
    {
        return std::ranges::equal(baseline.children,
                                  current.children,
                                  [this](const Resource& before, const Resource& after) {
                                      return after.origin && after.origin->source == m_Document->source &&
                                             after.origin->object == before.origin->object;
                                  });
    }

    /// Edits a child in place, or replaces its element when its class changed.
    void editChild(const Resource& child)
    {
        // Bound children were loaded from an element of this source.
        if (!child.origin || diffInPlace(child, child.origin->object))
        {
            return;
        }
        const auto& element = source().node(child.origin->object).parent;
        if (element)
        {
            m_Patch.replace(*element, osp::objectProperty(source().node(*element).name.value_or(""), merged(child)));
        }
    }

    /// Edits the existing `resources` array when the bound children keep their order and new children follow
    /// them. Returns false when the array must be rewritten.
    bool appendInPlace(const Resource& baseline, const Resource& current, osp::NodeId object)
    {
        const auto property = osp::ObjectView(source(), object).property("resources");
        if (!property || property->type() != osp::PropertyType::Array)
        {
            return false;
        }
        const auto elements = property->elements();
        if (!elements.empty() && elements.front().name() == "array")
        {
            return false;
        }
        std::map<osp::NodeId, std::size_t> positions;
        for (std::size_t index = 0; index < baseline.children.size(); ++index)
        {
            if (const auto& origin = baseline.children[index].origin)
            {
                positions.emplace(origin->object, index);
            }
        }
        std::vector<std::optional<std::size_t>> bound(current.children.size());
        std::set<osp::NodeId> claimed;
        std::optional<std::size_t> previous;
        bool appending = false;
        for (std::size_t index = 0; index < current.children.size(); ++index)
        {
            const auto& child = current.children[index];
            if (child.origin && child.origin->source == m_Document->source)
            {
                const auto found = positions.find(child.origin->object);
                if (found != positions.end() && claimed.insert(found->first).second)
                {
                    if (appending || (previous && *previous >= found->second))
                    {
                        return false;
                    }
                    previous = found->second;
                    bound[index] = found->second;
                    continue;
                }
            }
            appending = true;
        }
        // Indices of the slots of removed children; every element with such an index goes (shadowed duplicates too).
        std::set<std::int32_t> removed;
        for (const auto& child : baseline.children)
        {
            const auto& origin = child.origin;
            if (!origin || claimed.contains(origin->object))
            {
                continue;
            }
            const auto& element = source().node(origin->object).parent;
            if (!element)
            {
                return false;
            }
            const auto index = osp::PropertyView(source(), *element).elementIndex();
            if (!index)
            {
                return false;
            }
            removed.insert(*index);
        }
        std::int32_t maxIndex = -1;
        std::optional<std::int32_t> lastRemaining;
        std::int32_t maxRemaining = -1;
        for (const auto& element : elements)
        {
            const auto index = element.elementIndex();
            if (!index)
            {
                return false;
            }
            maxIndex = std::max(maxIndex, *index);
            if (!removed.contains(*index))
            {
                lastRemaining = *index;
                maxRemaining = std::max(maxRemaining, *index);
            }
        }
        const bool appends = std::ranges::any_of(bound, [](const auto& slot) { return !slot; });
        // OSP sizes the array from the last element in the file, so it must hold the largest remaining index.
        if (!appends && lastRemaining && *lastRemaining < maxRemaining)
        {
            return false;
        }
        for (const auto& element : elements)
        {
            if (removed.contains(element.elementIndex().value()))
            {
                m_Patch.remove(element.id());
            }
        }
        auto next = maxIndex + 1;
        for (std::size_t index = 0; index < current.children.size(); ++index)
        {
            const auto& child = current.children[index];
            if (bound[index])
            {
                editChild(child);
                continue;
            }
            m_Patch.append(property->id(), osp::objectProperty("[" + std::to_string(next++) + "]", merged(child)));
        }
        return true;
    }

    /// A rewritten `resources` array: every child's logical content, then the elements OSP ignores.
    osp::PropertyValue resourcesProperty(const Resource& collection, const osp::Source* originSource, osp::NodeId object)
    {
        std::vector<std::optional<osp::PropertyValue>> elements;
        elements.reserve(collection.children.size());
        for (const auto& child : collection.children)
        {
            elements.emplace_back(osp::objectProperty("", merged(child)));
        }
        if (originSource != nullptr)
        {
            for (auto& opaque : opaqueElements(*originSource, object))
            {
                elements.emplace_back(std::move(opaque));
            }
        }
        return osp::arrayProperty("resources", std::string(RESOURCE_ARRAY_CLASS), elements);
    }

    /// The logical content to write for a resource: its original object with the changed fields applied, or a
    /// new object in OSP's layout.
    osp::ObjectValue merged(const Resource& resource)
    {
        const auto childValue = [this](const Resource& child)
        {
            return merged(child);
        };
        if (!resource.origin)
        {
            return objectValueOf(resource, HtmlRule::Keep, childValue);
        }
        const auto& origin = *resource.origin;
        auto baseline = baselineLoad(origin);
        if (!baseline)
        {
            return objectValueOf(resource, HtmlRule::Keep, childValue);
        }
        auto value = osp::objectValue(*origin.source, origin.object);
        value.className = std::string(classOf(resource.kind));
        detail::ValueEditor editor(value, RESOURCE_ORDER);
        if (baseline->kind != resource.kind)
        {
            // A collection that became a resource loses its children; a new collection starts without a type.
            editor.removeAll("resources");
            if (resource.isCollection())
            {
                editor.removeAll("type");
                baseline->type = ResourceType::Unknown;
            }
            baseline->children.clear();
        }
        applyFields(editor, *baseline, resource);
        if (resource.isCollection())
        {
            if (resource.children.empty())
            {
                if (!baseline->children.empty())
                {
                    editor.removeAll("resources");
                }
            }
            else
            {
                editor.setProperty(resourcesProperty(resource, origin.source.get(), origin.object));
            }
        }
        return value;
    }

    const CollectionDocument* m_Document;
    osp::Patch m_Patch;
    bool m_Indexed = false;
    Resource m_Baseline{};
    std::map<osp::NodeId, const Resource*> m_Baselines{};
};

} // namespace

DocumentKind documentKind(const osp::Source& source)
{
    const auto className = source.node(source.root()).className;
    if (className == RESOURCE_CLASS)
    {
        return DocumentKind::Resource;
    }
    if (className == COLLECTION_CLASS)
    {
        return DocumentKind::Collection;
    }
    if (className == LIBRARY_CLASS)
    {
        return DocumentKind::Library;
    }
    return DocumentKind::Other;
}

osp::Expected<CollectionDocument, LoadError> loadCollection(std::shared_ptr<const osp::Source> source)
{
    if (!source)
    {
        return osp::makeUnexpected(LoadError{.code = LoadError::Code::Parse, .message = "no document"});
    }
    const auto kind = documentKind(*source);
    if (kind != DocumentKind::Resource && kind != DocumentKind::Collection)
    {
        return osp::makeUnexpected(LoadError{.code = LoadError::Code::NotLibraryDocument,
                                             .location = "root",
                                             .message = kind == DocumentKind::Library
                                                            ? "the document is a Library, not a collection"
                                                            : "the root is not a LibraryResource or LibraryCollection"});
    }
    CollectionDocument document{.source = std::move(source)};
    Loader loader(document.source, &document.diagnostics);
    auto root = loader.load(osp::ObjectView(*document.source, document.source->root()), "root");
    if (!root)
    {
        return osp::makeUnexpected(
            LoadError{.code = LoadError::Code::Malformed, .location = root.error().location, .message = root.error().message});
    }
    document.root = *std::move(root);
    return document;
}

osp::Expected<CollectionDocument, LoadError> loadCollection(std::string bytes, const osp::ParseLimits& limits)
{
    auto source = osp::parse(std::move(bytes), limits);
    if (!source)
    {
        return osp::makeUnexpected(LoadError{.code = LoadError::Code::Parse, .message = source.error().message, .parse = source.error()});
    }
    return loadCollection(*std::move(source));
}

osp::Expected<std::string, SaveError> saveCollection(const CollectionDocument& document)
{
    return Saver(document).save();
}

osp::ObjectValue collectionObjectValue(const Resource& resource)
{
    return writtenValue(resource, HtmlRule::Osp);
}

osp::Expected<std::string, SaveError> writeCollection(const Resource& resource, osp::LineEnding lineEnding)
{
    auto output = osp::write(collectionObjectValue(resource), osp::WriteOptions{.lineEnding = lineEnding});
    if (!output)
    {
        return osp::makeUnexpected(SaveError{.code = SaveError::Code::Render, .message = output.error().message});
    }
    auto reloaded = loadCollection(*output);
    if (!reloaded || !sameContent(reloaded->root, reloadedForm(resource, HtmlRule::Osp)))
    {
        return osp::makeUnexpected(
            SaveError{.code = SaveError::Code::VerificationFailed, .message = "the written collection reloads with different content"});
    }
    return *std::move(output);
}

Resource reloadedForm(const Resource& resource)
{
    return reloadedForm(resource, HtmlRule::Osp);
}

} // namespace motrack::library
