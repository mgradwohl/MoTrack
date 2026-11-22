#pragma once

#include "library/resource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// The ComPADRE digital library as LibraryComPADRE (OSP d56207b, Tracker 6.3.5) maps its REST responses to
/// collections: the subject tree of a `verb=Identify` query and the `<record>` lists of record queries. The
/// functions are pure: the caller fetches the XML (slice I6). See docs/library-collections.md.
namespace motrack::library
{

inline constexpr std::string_view COMPADRE_QUERY = "https://www.compadre.org/osp/services/REST/osp";
inline constexpr std::string_view COMPADRE_TRACKER_TREE =
    "https://www.compadre.org/osp/services/REST/osp_tracker.cfm?verb=Identify&OSPType=Tracker";
inline constexpr std::string_view COMPADRE_TRACKER_RECORDS = "https://www.compadre.org/osp/services/REST/osp_tracker.cfm?OSPType=Tracker";
inline constexpr std::string_view COMPADRE_EJS_TREE =
    "https://www.compadre.org/osp/services/REST/osp_jars.cfm?verb=Identify&OSPType=EJS%20Model&AttachedDocument=Source%20Code";
inline constexpr std::string_view COMPADRE_EJS_RECORDS =
    "https://www.compadre.org/osp/services/REST/osp_jars.cfm?OSPType=EJS%20Model&AttachedDocument=Source%20Code";
inline constexpr std::string_view COMPADRE_PRIMARY_ONLY = "&OSPPrimary=Subject";
inline constexpr std::string_view COMPADRE_TRACKER_COLLECTION_NAME = "OSP Tracker Collection";
inline constexpr std::string_view COMPADRE_EJS_COLLECTION_NAME = "OSP EJS Collection";
inline constexpr std::string_view COMPADRE_GENERIC_COLLECTION_NAME = "ComPADRE OSP Collection";
inline constexpr std::string_view COMPADRE_TRACKER_INFO_URL = "https://opensourcephysics.github.io/resources/CAB/comPADRE_collection.html";
inline constexpr std::string_view COMPADRE_EJS_INFO_URL = "https://www.compadre.org/osp/online_help/EjsDL/DLModels.html";
inline constexpr std::string_view COMPADRE_ABOUT_OSP = "About OSP and ComPADRE";
inline constexpr std::string_view COMPADRE_OSP_INFO_URL = "https://www.compadre.org/osp/online_help/EjsDL/OSPCollection.html";

/// LibraryComPADRE.isComPADREPath: starts with COMPADRE_QUERY.
[[nodiscard]] bool isComPadrePath(std::string_view path) noexcept;
/// LibraryComPADRE.isPrimarySubjectOnly.
[[nodiscard]] bool isPrimarySubjectOnly(std::string_view path) noexcept;
/// LibraryComPADRE.getCollectionName: by the tree query prefix.
[[nodiscard]] std::string_view comPadreCollectionName(std::string_view path) noexcept;
/// LibraryComPADRE.getCollectionPath: adds or removes the trailing COMPADRE_PRIMARY_ONLY.
[[nodiscard]] std::string comPadreCollectionPath(std::string_view path, bool primarySubjectOnly);

/// Strings LibraryResource.getHTMLBody takes from ToolsRes; the defaults are Tracker's English resources.
struct ComPadreLabels
{
    /// "LibraryResource.Type.<type>", indexed by ResourceType.
    std::array<std::string, 10> types = {
        "Other", "Collection", "Tracker Experiment", "EJS Model", "Video", "Image", "HTML Page", "PDF Document", "Data", "URL"};
    std::string author = "Author";                        ///< LibraryTreePanel.Label.Author
    std::string resource = "Resource";                    ///< LibraryResource.Description.Resource
    std::string moreInformation = "More information at:"; ///< LibraryComPADRE.Description.InfoField
    char decimalSeparator = '.';                          ///< The locale's, for the download size.
};

enum class ComPadreMode : std::uint8_t
{
    /// MoTrack's behavior: each record opens its own attachment, matched by file identifier before Tracker's
    /// type order (#213), and a malformed record (missing identifier, size, contributors or text) neither stops
    /// the remaining records nor loses its description.
    MoTrack,
    /// Tracker 6.3.5 exactly, for the oracle: attachments by type order, and a record that makes
    /// LibraryComPADRE.loadNode throw ends the list.
    Tracker635,
};

struct ComPadreOptions
{
    /// LibraryComPADRE.desiredOSPType; Tracker sets "Tracker" when it opens the Library Browser.
    std::string desiredOspType = "Tracker";
    ComPadreMode mode = ComPadreMode::MoTrack;
    ComPadreLabels labels{};
};

/// The attachment a record opens (LibraryResource.Attachment).
struct ComPadreAttachment
{
    std::string type{}; ///< The matched file type ("Main", "Primary", "Supplemental", ...).
    std::optional<std::string> url{};
    std::optional<std::string> fileName{};
    std::int32_t size = 0; ///< Bytes, from the file-name element's file-size attribute.
};

/// LibraryComPADRE.load(collection, query): for every `Identify` element, a sub-collection for each `sub-tree`
/// of the "osp-subject" `sub-tree-set`, recursively through "-detail" sets. A leaf's target is its accumulated
/// "&param" query and its description a placeholder; an "Unclassified" sub-tree's query becomes the parent's
/// target. Returns false when nothing was added, including when the XML cannot be parsed.
bool loadComPadreTree(Resource& collection, std::string_view xml);

/// LibraryComPADRE.getCollection(path): the collection Tracker shows for a ComPADRE tree query, built from that
/// query's response.
[[nodiscard]] Resource comPadreCollection(std::string_view queryPath, std::string_view treeXml);

struct ComPadreRecordsResult
{
    std::size_t records = 0;            ///< `record` elements in the response.
    std::size_t added = 0;              ///< Resources appended to the collection.
    bool completed = false;             ///< Every record was processed (Tracker635 stops at a loadNode exception).
    bool success = false;               ///< At least one record was processed: Tracker's onSuccess.
    std::optional<std::string> error{}; ///< The XML could not be parsed.
};

/// LibraryComPADRE.loadResources/loadNode/setRecord: appends a resource for every record of the desired type that
/// has a downloadable attachment: name from the title, target from the download URL, type Tracker (for the
/// default desired type), the download_filename and reload_url properties, the thumbnail URL, Keywords and Author
/// metadata, and an HTML description built like LibraryResource.getHTMLBody. When processing completes, or the
/// response has no records, the collection's own description and target are cleared, as Tracker does after
/// loading a ComPADRE node.
ComPadreRecordsResult
loadComPadreRecords(Resource& collection, std::string_view xml, std::string_view reloadUrl, const ComPadreOptions& options = {});

/// LibraryResource.getHTMLBody for a ComPADRE record (the arguments LibraryComPADRE.setRecord passes). Tracker
/// inserts every value as it is. `ComPadreMode::MoTrack` escapes the title, type, authors, file name and URLs and
/// keeps only http(s) thumbnails and links; the description stays HTML in both modes, so the browser must render
/// it without scripts or external resources.
[[nodiscard]] std::string comPadreHtmlBody(std::string_view title,
                                           std::string_view type,
                                           std::optional<std::string_view> thumbnail,
                                           std::optional<std::string_view> description,
                                           std::string_view authors,
                                           std::optional<std::string_view> informationUrl,
                                           const std::optional<ComPadreAttachment>& attachment,
                                           const ComPadreLabels& labels,
                                           ComPadreMode mode = ComPadreMode::MoTrack);

/// The megabyte text of LibraryResource.getHTMLBody: bytes / 1048576 with DecimalFormat "0.0" (HALF_EVEN).
[[nodiscard]] std::string comPadreMegabytes(std::int32_t bytes, char decimalSeparator);

} // namespace motrack::library
