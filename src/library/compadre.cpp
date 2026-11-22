#include "library/compadre.hpp"

#include "library/java_string.hpp"
#include "library/paths.hpp"
#include "library/xml_dom.hpp"
#include "osp/java_text.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace motrack::library
{
namespace
{

using xml::Node;

/// A Java exception escaping LibraryComPADRE.loadNode, which ends Tracker's record list (Tracker635 only).
struct Thrown
{};

template <typename T> using Outcome = osp::Expected<T, Thrown>;

constexpr std::string_view EJS = "EJS";
constexpr std::string_view TRACKER = "Tracker";

class RecordMapper
{
  public:
    explicit RecordMapper(const ComPadreOptions& options) : m_Options(&options)
    {}

    [[nodiscard]] bool strict() const noexcept
    {
        return m_Options->mode == ComPadreMode::Tracker635;
    }

    /// LibraryComPADRE.isDesiredOSPType. A null desired type (empty) accepts any osp-type element.
    [[nodiscard]] Outcome<bool> isDesired(const Node& record) const
    {
        for (const auto* type : record.childrenNamed("osp-type"))
        {
            if (m_Options->desiredOspType.empty())
            {
                return true;
            }
            const auto text = type->textValue();
            if (!text && strict())
            {
                return osp::makeUnexpected(Thrown{});
            }
            const auto value = text.value_or("");
            if (value.find(m_Options->desiredOspType) != std::string_view::npos || value.find(TRACKER) != std::string_view::npos)
            {
                return true;
            }
        }
        return false;
    }

    /// LibraryComPADRE.isTrackerType.
    [[nodiscard]] Outcome<bool> isTrackerType(const Node& record) const
    {
        for (const auto* type : record.childrenNamed("osp-type"))
        {
            const auto text = type->textValue();
            if (!text && strict())
            {
                return osp::makeUnexpected(Thrown{});
            }
            if (text.value_or("").find(TRACKER) != std::string_view::npos)
            {
                return true;
            }
        }
        return false;
    }

    /// LibraryComPADRE.getAttachment: the first downloadable attached document of these types, replaced by a
    /// later one whose file identifier matches the record's.
    [[nodiscard]] Outcome<std::optional<ComPadreAttachment>> attachmentByType(const Node& record,
                                                                              std::span<const std::string_view> types) const
    {
        const auto id = xml::childValue(&record, "file-identifier");
        std::optional<ComPadreAttachment> attachment;
        for (const auto* document : record.childrenNamed("attached-document"))
        {
            const auto documentId = xml::childValue(document, "file-identifier");
            if (!id && strict())
            {
                return osp::makeUnexpected(Thrown{});
            }
            const bool matchesId = id && documentId && *id == *documentId;
            const auto* fileType = document->firstChild("file-type");
            for (const auto type : types)
            {
                if (fileType == nullptr || fileType->textValue() != type)
                {
                    continue;
                }
                const auto* url = document->firstChild("download-url");
                if (url == nullptr || (attachment && !matchesId))
                {
                    continue;
                }
                auto found = describe(*document, type);
                if (!found)
                {
                    return osp::makeUnexpected(found.error());
                }
                if (!found->url && !strict())
                {
                    continue;
                }
                attachment = *std::move(found);
            }
        }
        return attachment;
    }

    /// The attachment a record opens.
    [[nodiscard]] Outcome<std::optional<ComPadreAttachment>> attachment(const Node& record) const
    {
        if (!strict())
        {
            // #213: the record's own file, matched by its file identifier.
            if (const auto id = xml::childValue(&record, "file-identifier"))
            {
                for (const auto* document : record.childrenNamed("attached-document"))
                {
                    if (xml::childValue(document, "file-identifier") == *id && document->firstChild("download-url") != nullptr)
                    {
                        auto found = describe(*document, xml::childValue(document, "file-type").value_or(""));
                        if (found && found->url)
                        {
                            return std::optional<ComPadreAttachment>(*std::move(found));
                        }
                    }
                }
            }
        }
        if (m_Options->desiredOspType == EJS)
        {
            // Java evaluates isTrackerType only for EJS.
            const auto tracker = isTrackerType(record);
            if (!tracker)
            {
                return osp::makeUnexpected(tracker.error());
            }
            if (!*tracker)
            {
                constexpr std::array<std::string_view, 1> SOURCE = {"Source Code"};
                return attachmentByType(record, SOURCE);
            }
        }
        constexpr std::array<std::string_view, 2> MAIN = {"Main", "Primary"};
        auto found = attachmentByType(record, MAIN);
        if (!found || *found)
        {
            return found;
        }
        constexpr std::array<std::string_view, 1> SUPPLEMENTAL = {"Supplemental"};
        return attachmentByType(record, SUPPLEMENTAL);
    }

    /// LibraryComPADRE.setRecord. Returns false where Java's catch block returns false, keeping the fields set so
    /// far (Tracker635); MoTrack fills every field it can.
    [[nodiscard]] bool setRecord(Resource& record, const Node& node, const ComPadreAttachment& attachment) const
    {
        if (!attachment.url)
        {
            return false;
        }
        setTarget(record, processUrl(*attachment.url), {});
        const auto title = xml::childValue(&node, "title");
        setName(record, title.value_or(""));
        setProperty(record,
                    std::string(DOWNLOAD_FILENAME_PROPERTY),
                    attachment.fileName ? std::optional<std::string>(*attachment.fileName) : std::nullopt);
        const auto ospType = xml::childValue(&node, "osp-type");
        std::optional<std::string> typeText = ospType ? std::optional<std::string>(*ospType) : std::nullopt;
        bool ejsModel = false;
        if (m_Options->desiredOspType == EJS)
        {
            const auto tracker = isTrackerType(node);
            if (!tracker)
            {
                return false;
            }
            ejsModel = !*tracker;
        }
        if (ejsModel)
        {
            typeText = std::string(EJS);
            setType(record, ResourceType::Ejs);
        }
        else if (m_Options->desiredOspType == TRACKER)
        {
            typeText = std::string(TRACKER);
            setType(record, ResourceType::Tracker);
        }
        else
        {
            if (!typeText && strict())
            {
                return false;
            }
            const auto lower = asciiLower(typeText.value_or(""));
            if (lower.find("track") != std::string::npos)
            {
                typeText = std::string(TRACKER);
                setType(record, ResourceType::Tracker);
            }
            else if (lower.find("ej") != std::string::npos)
            {
                typeText = std::string(EJS);
                setType(record, ResourceType::Ejs);
            }
            else
            {
                setType(record, ResourceType::Unknown);
            }
        }
        const auto description = xml::childValue(&node, "description");
        const auto information = xml::childValue(&node, "information-url");
        const auto thumbnail = xml::childValue(&node, "thumbnail-url");
        std::string authors;
        const auto* contributors = node.firstChild("contributors");
        if (contributors == nullptr && strict())
        {
            return false;
        }
        if (contributors != nullptr)
        {
            for (const auto* contributor : contributors->childrenNamed("contributor"))
            {
                if (contributor->attribute("role") != "Author")
                {
                    continue;
                }
                const auto name = contributor->textValue();
                if (!name && !strict())
                {
                    continue;
                }
                authors += std::string(name.value_or("null")) + ", ";
            }
        }
        if (authors.ends_with(", "))
        {
            authors.resize(authors.size() - 2);
        }
        // Desktop OSP shows its cached copy of the thumbnail; the browser (I6) maps the URL to its cache.
        record.thumbnail = thumbnail ? std::optional<std::string>(*thumbnail) : std::nullopt;
        setDescription(record,
                       comPadreHtmlBody(title.value_or(""),
                                        typeText.value_or(""),
                                        thumbnail,
                                        description,
                                        authors,
                                        information,
                                        attachment,
                                        m_Options->labels,
                                        m_Options->mode));
        record.metadata.clear();
        std::vector<std::string> words;
        for (const auto* subject : node.childrenNamed("osp-subject"))
        {
            const auto text = subject->textValue();
            if (!text)
            {
                if (strict())
                {
                    return false;
                }
                continue;
            }
            for (auto& word : javaSplit(*text, " / "))
            {
                if (word != "General" && std::ranges::find(words, word) == words.end())
                {
                    words.push_back(std::move(word));
                }
            }
        }
        if (!words.empty())
        {
            std::string keywords;
            for (const auto& word : words)
            {
                keywords += word + ", ";
            }
            keywords.resize(keywords.size() - 2);
            addMetadata(record, Metadata{.key = "keywords", .value = std::move(keywords)});
        }
        if (!authors.empty())
        {
            addMetadata(record, Metadata{.key = "author", .value = authors});
        }
        return true;
    }

    /// LibraryComPADRE.loadNode. Returns how many resources it added; Thrown ends the list in Tracker635.
    Outcome<std::size_t> loadNode(Resource& collection, const Node& node, std::string_view reloadUrl) const
    {
        const auto desired = isDesired(node);
        if (!desired)
        {
            return osp::makeUnexpected(desired.error());
        }
        if (!*desired)
        {
            return std::size_t{0};
        }
        auto selected = attachment(node);
        if (!selected)
        {
            return osp::makeUnexpected(selected.error());
        }
        const auto& chosen = *selected;
        if (!chosen)
        {
            return std::size_t{0};
        }
        collection.children.push_back(makeResource(xml::childValue(&node, "title").value_or("")));
        auto& record = collection.children.back();
        if (setRecord(record, node, *chosen))
        {
            setProperty(record, std::string(RELOAD_URL_PROPERTY), std::string(reloadUrl));
        }
        return std::size_t{1};
    }

  private:
    [[nodiscard]] Outcome<ComPadreAttachment> describe(const Node& document, std::string_view type) const
    {
        ComPadreAttachment found{.type = std::string(type)};
        if (const auto url = xml::childValue(&document, "download-url"))
        {
            found.url = std::string(*url);
        }
        if (const auto* file = document.firstChild("file-name"))
        {
            if (const auto name = file->textValue())
            {
                found.fileName = std::string(*name);
            }
            const auto size = osp::parseJavaInt(file->attribute("file-size"));
            if (!size && strict())
            {
                return osp::makeUnexpected(Thrown{});
            }
            found.size = size ? *size : 0;
        }
        return found;
    }

    /// LibraryComPADRE.processURL: "&amp;" becomes "&" (the DOM has already decoded one level).
    static std::string processUrl(std::string_view url)
    {
        std::string processed;
        while (true)
        {
            const auto entity = url.find("&amp;");
            if (entity == std::string_view::npos)
            {
                processed += url;
                return processed;
            }
            processed += url.substr(0, entity + 1);
            url.remove_prefix(entity + 5);
        }
    }

    const ComPadreOptions* m_Options;
};

bool loadSubtrees(Resource& collection, const Node& parent, const std::string& attributeType, const std::string& serviceParameter)
{
    bool success = false;
    for (const auto& node : parent.children)
    {
        if (node.type != Node::Type::Element || node.name != "sub-tree-set" || node.attribute("type") != attributeType)
        {
            continue;
        }
        const auto subTrees = node.childrenNamed("sub-tree");
        if (subTrees.empty())
        {
            continue;
        }
        std::optional<std::string> unclassified;
        for (const auto* subtree : subTrees)
        {
            const auto name = subtree->attribute("name");
            auto parameter = serviceParameter + "&" + nonUriPath(subtree->attribute("service-parameter"));
            if (name == "Unclassified")
            {
                // Processed last: its records belong to the parent.
                unclassified = std::move(parameter);
                continue;
            }
            auto sub = makeCollection(name);
            if (subtree->childrenNamed("sub-tree-set").empty())
            {
                setDescription(sub, "<h2>" + std::string(name) + "</h2><blockquote>...</blockquote>");
                setTarget(sub, parameter, {});
            }
            else
            {
                loadSubtrees(sub, *subtree, attributeType + "-detail", parameter);
            }
            collection.children.push_back(std::move(sub));
            success = true;
        }
        if (unclassified)
        {
            setTarget(collection, *unclassified, {});
        }
    }
    return success;
}

/// java.net.URL(String) accepts the text: a protocol that has a handler in the JDK.
bool isJavaUrl(std::string_view spec)
{
    spec = osp::javaTrim(spec);
    if (spec.size() >= 4 && asciiLower(spec.substr(0, 4)) == "url:")
    {
        spec.remove_prefix(4);
    }
    const auto colon = spec.find_first_of(":/#");
    if (colon == std::string_view::npos || colon == 0 || spec[colon] != ':')
    {
        return false;
    }
    const auto protocol = asciiLower(spec.substr(0, colon));
    if (!((protocol.front() >= 'a' && protocol.front() <= 'z')) ||
        !std::ranges::all_of(protocol,
                             [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-'; }))
    {
        return false;
    }
    constexpr std::array<std::string_view, 7> HANDLERS = {"http", "https", "ftp", "file", "mailto", "jrt", "jmod"};
    if (protocol == "jar")
    {
        return spec.find("!/") != std::string_view::npos;
    }
    return std::ranges::find(HANDLERS, protocol) != HANDLERS.end();
}

/// Text for HTML content or a quoted attribute.
std::string escapeHtml(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text)
    {
        switch (character)
        {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&#39;";
            break;
        default:
            escaped += character;
        }
    }
    return escaped;
}
std::string insertLineBreaks(std::string_view text)
{
    const auto parts = javaSplit(text, "\n");
    std::string joined;
    for (std::size_t index = 0; index < parts.size(); ++index)
    {
        joined += parts[index];
        if (index + 1 < parts.size())
        {
            joined += "<br>";
        }
    }
    return joined;
}

} // namespace

bool isComPadrePath(std::string_view path) noexcept
{
    return path.starts_with(COMPADRE_QUERY);
}

bool isPrimarySubjectOnly(std::string_view path) noexcept
{
    return path.find(COMPADRE_PRIMARY_ONLY) != std::string_view::npos;
}

std::string_view comPadreCollectionName(std::string_view path) noexcept
{
    if (path.starts_with(COMPADRE_EJS_TREE))
    {
        return COMPADRE_EJS_COLLECTION_NAME;
    }
    if (path.starts_with(COMPADRE_TRACKER_TREE))
    {
        return COMPADRE_TRACKER_COLLECTION_NAME;
    }
    return COMPADRE_GENERIC_COLLECTION_NAME;
}

std::string comPadreCollectionPath(std::string_view path, bool primarySubjectOnly)
{
    const bool isPrimary = path.ends_with(COMPADRE_PRIMARY_ONLY);
    if (isPrimary == primarySubjectOnly)
    {
        return std::string(path);
    }
    if (primarySubjectOnly)
    {
        return std::string(path) + std::string(COMPADRE_PRIMARY_ONLY);
    }
    return std::string(path.substr(0, path.size() - COMPADRE_PRIMARY_ONLY.size()));
}

bool loadComPadreTree(Resource& collection, std::string_view xmlText)
{
    const auto document = xml::parse(xmlText);
    if (!document)
    {
        return false;
    }
    bool success = false;
    for (const auto* identify : xml::elementsByTagName(*document, "Identify"))
    {
        success = loadSubtrees(collection, *identify, "osp-subject", "") || success;
    }
    return success;
}

Resource comPadreCollection(std::string_view queryPath, std::string_view treeXml)
{
    const auto name = comPadreCollectionName(queryPath);
    auto collection = makeCollection(name);
    if (name == COMPADRE_EJS_COLLECTION_NAME)
    {
        setHtmlPath(collection, COMPADRE_EJS_INFO_URL);
    }
    else if (name == COMPADRE_TRACKER_COLLECTION_NAME)
    {
        setHtmlPath(collection, COMPADRE_TRACKER_INFO_URL);
    }
    auto about = makeResource(COMPADRE_ABOUT_OSP);
    setHtmlPath(about, COMPADRE_OSP_INFO_URL);
    collection.children.push_back(std::move(about));
    loadComPadreTree(collection, treeXml);
    // Generic collections use the EJS record query, as OSP does.
    std::string base(name == COMPADRE_TRACKER_COLLECTION_NAME ? COMPADRE_TRACKER_RECORDS : COMPADRE_EJS_RECORDS);
    if (isPrimarySubjectOnly(queryPath))
    {
        base += COMPADRE_PRIMARY_ONLY;
    }
    setBasePath(collection, base);
    return collection;
}

ComPadreRecordsResult
loadComPadreRecords(Resource& collection, std::string_view xmlText, std::string_view reloadUrl, const ComPadreOptions& options)
{
    ComPadreRecordsResult result;
    if (!collection.isCollection())
    {
        return result;
    }
    const auto clear = [&collection]()
    {
        setDescription(collection, "");
        setTarget(collection, "", {});
    };
    const auto document = xml::parse(xmlText);
    if (!document)
    {
        result.error = document.error().message;
        result.completed = true;
        clear();
        return result;
    }
    const auto records = xml::elementsByTagName(*document, "record");
    result.records = records.size();
    const RecordMapper mapper(options);
    for (const auto* record : records)
    {
        const auto added = mapper.loadNode(collection, *record, reloadUrl);
        if (!added)
        {
            // Neither onFound nor onNothingNew runs, so the remaining records and the final clean-up are skipped.
            return result;
        }
        result.added += *added;
        result.success = true;
    }
    result.completed = true;
    clear();
    return result;
}

std::string comPadreHtmlBody(std::string_view title,
                             std::string_view type,
                             std::optional<std::string_view> thumbnail,
                             std::optional<std::string_view> description,
                             std::string_view authors,
                             std::optional<std::string_view> informationUrl,
                             const std::optional<ComPadreAttachment>& attachment,
                             const ComPadreLabels& labels,
                             ComPadreMode mode)
{
    // Tracker inserts the response's text into its HTML unchanged; MoTrack escapes text and keeps only web links.
    const bool safe = mode == ComPadreMode::MoTrack;
    const auto text = [safe](std::string_view value)
    {
        return safe ? escapeHtml(value) : std::string(value);
    };
    const auto linkable = [safe](std::string_view url)
    {
        return !safe || isHttp(url);
    };
    std::string body;
    if (!title.empty())
    {
        body += "\n      <h2>" + text(title) + "</h2>";
    }
    body += "\n      <blockquote>";
    if (!type.empty())
    {
        const auto known = resourceTypeFromName(type);
        body += "\n        <b>" + (known ? labels.types.at(static_cast<std::size_t>(*known)) : text(type)) + "</b>";
    }
    if (thumbnail && !thumbnail->empty() && linkable(*thumbnail))
    {
        auto image = "<p><img src=\"" + text(*thumbnail) + "\"";
        if (!title.empty())
        {
            image += " alt=\"" + text(title) + "\"";
        }
        body += "\n        " + image + "></p>";
    }
    if (description && !description->empty())
    {
        // The description is HTML in both modes; the browser renders it without scripts or external loads (I6).
        std::string paragraph(*description);
        if (!paragraph.starts_with("<p>"))
        {
            paragraph = "<p>" + insertLineBreaks(paragraph) + "</p>";
        }
        body += "\n        " + paragraph;
    }
    if (!authors.empty())
    {
        body += "\n        <p><b>" + labels.author + ":  </b>" + text(authors) + "</p>";
    }
    if (attachment && attachment->fileName)
    {
        body += "\n        <p><b>" + labels.resource + ":  </b>" + text(*attachment->fileName) + " (" +
                comPadreMegabytes(attachment->size, labels.decimalSeparator) + "MB)</p>";
    }
    if (informationUrl && !informationUrl->empty() && isJavaUrl(*informationUrl) && linkable(osp::javaTrim(*informationUrl)))
    {
        const auto url = text(*informationUrl);
        body += "\n        <p><b>" + labels.moreInformation + "  </b><a href=\"" + url + "\">" + url + "</a></p>";
    }
    body += "\n      </blockquote>";
    return body;
}

std::string comPadreMegabytes(std::int32_t bytes, char decimalSeparator)
{
    constexpr std::int64_t MEGABYTE = std::int64_t{1} << 20;
    const auto magnitude = bytes < 0 ? -static_cast<std::int64_t>(bytes) : static_cast<std::int64_t>(bytes);
    // bytes / 2^20 is exact in binary, so HALF_EVEN rounding to tenths can be done with integers.
    const auto scaled = magnitude * 10;
    auto tenths = scaled / MEGABYTE;
    const auto remainder = scaled % MEGABYTE;
    if (remainder * 2 > MEGABYTE || (remainder * 2 == MEGABYTE && (tenths % 2) != 0))
    {
        ++tenths;
    }
    return (bytes < 0 ? "-" : "") + std::to_string(tenths / 10) + decimalSeparator + std::to_string(tenths % 10);
}

} // namespace motrack::library
