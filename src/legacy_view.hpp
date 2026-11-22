#pragma once

#include "legacy_project.hpp"
#include "model/tracker_document.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

/// The read-only `LegacyProject` view of a typed document and the conversion from the reduced model of native
/// versions 1–6 (docs/tracker-document.md, "What LegacyProject keeps until B3" and "Reading versions 1–6").
/// Nothing writes through the view: index-based consumers read it until B3 replaces them with ID-based ones.
namespace motrack
{

/// The reduced project that B1's import policy presents for the typed state: track lists in presentation order,
/// B1's defaults and clamps, and `unsupportedTrackTypes` from the Unknown tracks.
[[nodiscard]] LegacyProject legacyProject(const model::TrackerDocument& document);

/// The `TrackerSource` that writer code still takes: the baseline bytes and, with known associations, a binding for
/// every bound point mass and vector (live or removed) by its `tracks` ordinal. Nothing without a baseline.
[[nodiscard]] std::optional<TrackerSource> trackerSourceView(const model::TrackerDocument& document);

/// Imports Tracker XML as a typed document, with B1's size bound, parse diagnostics and refusals. Throws ProjectError.
[[nodiscard]] model::TrackerDocument importTypedTracker(std::string_view xml);

/// Converts the reduced model a native file of versions 1–6 holds, as the native reader returns it (migrated to the
/// current reading where that is safe, docs/native-format.md), into a typed document, for B2.4a's native reader.
/// `reading` is the reading the values follow (the body's `trackerReading`; versions 1–4 count as revision 0) and
/// becomes the document's reader revision; the values are never transformed again. Without a source: native-only
/// objects. With a source but no bindings, or with `associationsDropped` (opening dropped stale bindings or kept an
/// outdated reading), or with values of an earlier reading: the baseline preserved but not indexed (associations
/// unknown, or stale when dropped). Otherwise the baseline's typed import with the stored identities, the stored
/// values of edited fields, created tracks unbound and bound tracks missing from the model removed; a field whose
/// stored value equals the baseline's reading keeps that reading, so no conversion turns it into an edit. Throws
/// ProjectError when the result's legacy view would differ from `project`, so a conversion never changes data
/// silently.
[[nodiscard]] model::TrackerDocument typedDocumentFromReduced(const LegacyProject& project,
                                                              const std::optional<TrackerSource>& source,
                                                              bool associationsDropped = false,
                                                              TrackerReading reading = CURRENT_TRACKER_READING);

/// The key of an owner's caches: the live owner's runtime identity and its document revision, which also advances on
/// every update outside history and every assignment, so a restored or replaced value never matches an old view.
struct DocumentStamp
{
    std::uint64_t runtime = 0;
    std::uint64_t revision = 0;

    bool operator==(const DocumentStamp&) const = default;
};

/// The legacy view cache of one live owner (a document session or archive-group member). It is rebuilt at most once
/// per stamp, and a copy of the owner starts with an empty cache, so a view is never shared between copies.
/// Owner-thread only; the view it returns is immutable and may be handed to workers.
class LegacyViewCache
{
  public:
    LegacyViewCache() = default;
    LegacyViewCache(const LegacyViewCache& /*other*/) noexcept
    {}
    LegacyViewCache& operator=(const LegacyViewCache& other) noexcept
    {
        if (this != &other)
        {
            clear();
        }
        return *this;
    }
    LegacyViewCache(LegacyViewCache&&) noexcept = default;
    LegacyViewCache& operator=(LegacyViewCache&&) noexcept = default;
    ~LegacyViewCache() = default;

    /// The view of `document` at `stamp`, built only when the stamp differs from the cached one.
    [[nodiscard]] std::shared_ptr<const LegacyProject> view(const model::TrackerDocument& document, DocumentStamp stamp);
    /// Adopts a view built elsewhere, such as the first view built on the open worker.
    void prime(DocumentStamp stamp, std::shared_ptr<const LegacyProject> view);
    void clear() noexcept;
    [[nodiscard]] std::optional<DocumentStamp> stamp() const noexcept;

  private:
    std::optional<DocumentStamp> m_Stamp;
    std::shared_ptr<const LegacyProject> m_View;
};

} // namespace motrack
