#pragma once

#include "library/resource.hpp"
#include "osp/expected.hpp"
#include "osp/render.hpp"
#include "osp/source.hpp"
#include "osp/value.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/// Loading and saving Digital Library collection files (`LibraryResource`/`LibraryCollection` XMLControl
/// documents) through motrack_osp. See docs/library-collections.md.
namespace motrack::library
{

struct LoadError
{
    enum class Code : std::uint8_t
    {
        Parse,              ///< motrack_osp could not read the document (`parse` holds the reason).
        NotLibraryDocument, ///< The root class is not LibraryResource or LibraryCollection.
        Malformed,          ///< OSP's loader throws on this content, so Tracker cannot open the collection.
    };
    Code code = Code::Malformed;
    std::string location{}; ///< Property path such as "root.resources[2].metadata".
    std::string message{};
    std::optional<osp::ParseError> parse{};
};

/// Content OSP's loader ignores and MoTrack keeps for saving, for example array elements of an unknown class.
struct Diagnostic
{
    std::string location{};
    std::string message{};
};

/// The root class of a parsed OSP document.
enum class DocumentKind : std::uint8_t
{
    Resource,   ///< org.opensourcephysics.tools.LibraryResource
    Collection, ///< org.opensourcephysics.tools.LibraryCollection
    Library,    ///< org.opensourcephysics.tools.Library ("my library" files)
    Other,
};

[[nodiscard]] DocumentKind documentKind(const osp::Source& source);

/// A loaded collection file. `source` is the immutable baseline: saving re-renders only what changed in
/// `root`. A document created in memory has no source.
struct CollectionDocument
{
    std::shared_ptr<const osp::Source> source{};
    Resource root{};
    std::vector<Diagnostic> diagnostics{};
};

/// Loads as LibraryBrowser.loadResource does: the root must be a LibraryResource or LibraryCollection, which
/// `XMLControlElement.loadObject` reads with LibraryResource.Loader/LibraryCollection.Loader. Where Java would
/// throw, MoTrack returns `Malformed` with the location. A `resources` collection (ArrayList) is accepted as
/// OSP's swingJS tip does; Tracker 6.3.5 cannot read it (a diagnostic says so).
[[nodiscard]] osp::Expected<CollectionDocument, LoadError> loadCollection(std::shared_ptr<const osp::Source> source);
[[nodiscard]] osp::Expected<CollectionDocument, LoadError> loadCollection(std::string bytes, const osp::ParseLimits& limits = {});

struct SaveError
{
    enum class Code : std::uint8_t
    {
        Render,             ///< motrack_osp refused the edit (for example a value OSP cannot read back).
        VerificationFailed, ///< Reloading the output did not give the saved model.
    };
    Code code = Code::Render;
    std::string message{};
};

/// Saves `document.root`. When the root was loaded from `document.source`, unchanged resources keep their bytes
/// and every property MoTrack does not model survives; changed fields are rendered with OSP's layout, removed
/// resources are deleted, and resources added after the existing ones are appended. Other edits to a
/// collection's list (reordering, inserting before an existing resource) rewrite that `resources` array with
/// each resource's original logical content. A document without a source is written as `writeCollection`
/// does, but resources loaded elsewhere keep their unknown properties. The output is reloaded and must equal
/// the model as OSP would load it.
[[nodiscard]] osp::Expected<std::string, SaveError> saveCollection(const CollectionDocument& document);

/// The XMLControl tree that LibraryCollection.Loader/LibraryResource.Loader.saveObject write: name always;
/// description, base path, target and metadata/properties when not empty; the HTML path when it is not empty
/// and not inside the target archive; the type unless Unknown; `resources` for a non-empty collection.
[[nodiscard]] osp::ObjectValue collectionObjectValue(const Resource& resource);
/// `new XMLControlElement(resource).toXML()`. Ignores origins; use `saveCollection` to keep loaded content.
[[nodiscard]] osp::Expected<std::string, SaveError> writeCollection(const Resource& resource,
                                                                    osp::LineEnding lineEnding = osp::LineEnding::Lf);

/// `resource` as OSP's loader would read it back after saving: trimmed text fields, metadata normalized by
/// addMetadata, a resource with an HTML path but no target typed HTML when it is Unknown, a collection without
/// children with a trimmed target. `saveCollection` verifies its output against this.
[[nodiscard]] Resource reloadedForm(const Resource& resource);

} // namespace motrack::library
