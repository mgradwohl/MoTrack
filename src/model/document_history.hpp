#pragma once

#include "model/commands.hpp"
#include "model/families.hpp"
#include "model/tracker_document.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

/// The undo history and revisions of one live document (docs/tracker-document.md, "Ownership, revisions and caches").
/// The owner (a document session or archive-group member) holds the live `TrackerDocument` and this history together;
/// revisions belong to the owner, not to the value, so copying or restoring a value never moves them backward. GUI
/// (owner) thread only. Standard-only interface.
namespace motrack::model
{

/// The outputs whose saved state a history tracks: the native companion and the standard Tracker file.
enum class SaveTarget : std::uint8_t
{
    Native,
    Tracker,
};

class DocumentHistory
{
  public:
    /// An empty history with a runtime identity no other history of this process has.
    DocumentHistory();
    // A copy would share the runtime identity, so caches keyed by it could confuse the two owners.
    DocumentHistory(const DocumentHistory&) = delete;
    DocumentHistory& operator=(const DocumentHistory&) = delete;
    DocumentHistory(DocumentHistory&&) = delete;
    DocumentHistory& operator=(DocumentHistory&&) = delete;
    ~DocumentHistory() = default;

    /// Identifies the live owner for the process's lifetime; Revert and replacement keep it.
    [[nodiscard]] std::uint64_t runtimeIdentity() const noexcept;
    /// Advances on every command, undo, redo and clear, as TrackEdits' revision does; save checkpoints use it.
    [[nodiscard]] std::uint64_t revision() const noexcept;
    /// Advances whenever the history revision does and on every update outside history, so caches keyed by
    /// (runtime identity, document revision) never match an earlier or replaced value.
    [[nodiscard]] std::uint64_t documentRevision() const noexcept;

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;

    /// Applies a new command to `live` and records it, or refuses it and changes nothing: not the document, the
    /// history, either revision or the save heads. A command that advances the identity counter leaves every saved
    /// output stale, since undo never rewinds the counter.
    std::expected<void, CommandError> apply(TrackerDocument& live, const Command& command);
    /// Applies the inverse of the last applied command, or refuses without moving the head.
    std::expected<void, CommandError> undo(TrackerDocument& live);
    std::expected<void, CommandError> redo(TrackerDocument& live);

    /// Installs a replacement or a reverted value: `live` becomes `incoming` with its graph's nodes marked changed above
    /// both graphs' revisions, the history is cleared and both revisions advance.
    void assign(TrackerDocument& live, TrackerDocument incoming);
    /// Installs a value changed outside history (such as a discovered media extent): the history and save heads stay,
    /// the document revision advances and the graph is floored as for an assignment.
    void update(TrackerDocument& live, TrackerDocument next);
    /// Forgets every entry and marks the current state saved for every output.
    void clear() noexcept;

    void markSaved(SaveTarget target = SaveTarget::Native) noexcept;
    /// The output no longer represents a known history state (for example, a completed stale save).
    void invalidateSaved(SaveTarget target = SaveTarget::Native) noexcept;
    [[nodiscard]] bool isModified(SaveTarget target = SaveTarget::Native) const noexcept;

  private:
    struct Entry
    {
        /// The command that moves the document across this entry next: its undo while the head is past it, its redo
        /// otherwise.
        Command next;
        /// The clip timeline before the command was first applied (see ApplyOptions::restoresTimeline).
        ClipTimeline timelineBefore;
    };

    void install(TrackerDocument& live, TrackerDocument value);

    std::uint64_t m_RuntimeIdentity;
    std::vector<Entry> m_Entries;
    std::size_t m_Head = 0;
    std::array<std::optional<std::size_t>, 2> m_SavedHeads{0, 0};
    std::uint64_t m_Revision = 0;
    std::uint64_t m_DocumentRevision = 0;
};

} // namespace motrack::model
