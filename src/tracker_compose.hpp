#pragma once

#include "legacy_project.hpp"
#include "model/tracker_document.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>

/// Composition of standard Tracker exports (docs/tracker-document.md, "From state to SourceEdits", slice B2.3).
///
/// `composeTrackerExport` composes the edits of an export from a typed document's state through the field descriptors
/// and renders them under O8. The checks that encode Tracker's behavior are admission checks, run on the legacy view and
/// the baseline before anything is composed and on the composed output before anything is written; the last of them is
/// the reimport check. B1's composition, which diffs the reduced project against a fresh import of its source, stays the
/// production export until B2.4b switches to this one; until then every B1 export can be compared with it.
namespace motrack
{

/// The linked media of a standard export.
enum class LinkedMedia : std::uint8_t
{
    None,
    Images,
    Movie
};

struct ExportMedia
{
    LinkedMedia kind = LinkedMedia::None;
    /// A linked movie's decoded frame times in seconds, which its path patch checks against the clip timing.
    std::span<const double> frameTimes{};
};

/// The standard export of a typed document with known associations: its baseline with the edits its state implies.
/// The media extension holds the references as the destination spells them. Throws ProjectError with the reason an
/// export is refused; returns the baseline's bytes when nothing changed.
[[nodiscard]] std::string composeTrackerExport(const model::TrackerDocument& document, const ExportMedia& media = {});

namespace detail
{

/// B1's composition (production export until B2.4b; B3 deletes it): the reduced project diffed against a fresh import
/// of its source, sharing the admission checks and patchers of the descriptor composition. Throws ProjectError.
[[nodiscard]] std::string composeLegacyExport(const LegacyProject& project, const TrackerSource& source, const ExportMedia& media);

/// A composition's result: the exported XML, or the refusal message.
struct CompositionRefusal
{
    std::string message;

    bool operator==(const CompositionRefusal&) const = default;
};
using CompositionOutcome = std::variant<std::string, CompositionRefusal>;

/// The descriptor composition of the same export as B1's, compared with B1's outcome: the exit criterion of B2.3 is the
/// same decision, and the same bytes where both accept.
struct CompositionComparison
{
    CompositionOutcome legacy;
    CompositionOutcome descriptor;

    [[nodiscard]] bool sameDecision() const noexcept;
    /// The same decision and, for exports, the same bytes; refusals may give different reasons.
    [[nodiscard]] bool equivalent() const noexcept;
    /// Also the same refusal message.
    [[nodiscard]] bool identical() const noexcept;
    /// A one-line account of a difference, or of the agreement.
    [[nodiscard]] std::string describe() const;
};

/// Composes the export B1 composed from `project` and `source` with the descriptors, from the typed document the reduced
/// model converts to (as B2.4a's documents will hold it), and compares the two outcomes.
[[nodiscard]] CompositionComparison compareWithDescriptorComposition(const LegacyProject& project,
                                                                     const TrackerSource& source,
                                                                     const ExportMedia& media,
                                                                     CompositionOutcome legacy);

/// Called with every production composition and its outcome, so tests and the corpus probe compare each B1 export with
/// the descriptor composition (B2.3's exit criterion). Unset in production; tests install it before exporting.
using CompositionObserver = void (*)(const LegacyProject& project,
                                     const TrackerSource& source,
                                     const ExportMedia& media,
                                     const CompositionOutcome& outcome);
void setCompositionObserver(CompositionObserver observer) noexcept;
[[nodiscard]] CompositionObserver compositionObserver() noexcept;

} // namespace detail
} // namespace motrack
