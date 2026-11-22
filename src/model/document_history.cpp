#include "model/document_history.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace motrack::model
{
namespace
{

std::uint64_t nextRuntimeIdentity() noexcept
{
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::size_t index(SaveTarget target) noexcept
{
    return static_cast<std::size_t>(target);
}

} // namespace

DocumentHistory::DocumentHistory() : m_RuntimeIdentity(nextRuntimeIdentity())
{}

std::uint64_t DocumentHistory::runtimeIdentity() const noexcept
{
    return m_RuntimeIdentity;
}

std::uint64_t DocumentHistory::revision() const noexcept
{
    return m_Revision;
}

std::uint64_t DocumentHistory::documentRevision() const noexcept
{
    return m_DocumentRevision;
}

bool DocumentHistory::canUndo() const noexcept
{
    return m_Head != 0;
}

bool DocumentHistory::canRedo() const noexcept
{
    return m_Head < m_Entries.size();
}

std::expected<void, CommandError> DocumentHistory::apply(TrackerDocument& live, const Command& command)
{
    if (!bind(live))
    {
        return foreign();
    }
    // Reserve the history slot first, so that recording the entry after the document changes cannot fail.
    if (m_Entries.capacity() < m_Head + 1)
    {
        m_Entries.reserve(m_Head + std::max<std::size_t>(1, m_Head / 2));
    }
    auto prepared = prepare(live, command, {.mode = ApplyMode::New});
    if (!prepared)
    {
        return std::unexpected(std::move(prepared.error()));
    }
    const bool allocates = prepared->document.nextObjectId() != live.nextObjectId();
    Entry entry{.next = std::move(prepared->inverse), .timelineBefore = clipTimeline(live)};
    // Commit: swaps, destruction of the discarded redo entries and a push into reserved capacity, none of which throw.
    live.swap(prepared->document);
    for (auto& saved : m_SavedHeads)
    {
        if (saved > m_Head)
        {
            saved.reset(); // A saved state on the discarded redo branch can no longer be reached.
        }
    }
    m_Entries.erase(m_Entries.begin() + static_cast<std::ptrdiff_t>(m_Head), m_Entries.end());
    m_Entries.push_back(std::move(entry));
    ++m_Head;
    ++m_Revision;
    ++m_DocumentRevision;
    if (allocates)
    {
        // Undo removes the object, but the advanced identity counter must still be saved. (The countdown low-water
        // mark is not saved before native version 7.)
        m_SavedHeads.fill(std::nullopt);
    }
    return {};
}

std::expected<void, CommandError> DocumentHistory::undo(TrackerDocument& live)
{
    if (!bind(live))
    {
        return foreign();
    }
    if (!canUndo())
    {
        return std::unexpected(CommandError{.code = CommandErrorCode::NothingToUndo, .message = "Nothing to undo."});
    }
    auto& entry = m_Entries[m_Head - 1];
    auto prepared = prepare(live, entry.next, {.mode = ApplyMode::Undo, .restoresTimeline = entry.timelineBefore});
    if (!prepared)
    {
        return std::unexpected(std::move(prepared.error()));
    }
    live.swap(prepared->document);
    entry.next = std::move(prepared->inverse);
    --m_Head;
    ++m_Revision;
    ++m_DocumentRevision;
    return {};
}

std::expected<void, CommandError> DocumentHistory::redo(TrackerDocument& live)
{
    if (!bind(live))
    {
        return foreign();
    }
    if (!canRedo())
    {
        return std::unexpected(CommandError{.code = CommandErrorCode::NothingToUndo, .message = "Nothing to redo."});
    }
    auto& entry = m_Entries[m_Head];
    auto prepared = prepare(live, entry.next, {.mode = ApplyMode::Redo});
    if (!prepared)
    {
        return std::unexpected(std::move(prepared.error()));
    }
    live.swap(prepared->document);
    entry.next = std::move(prepared->inverse);
    ++m_Head;
    ++m_Revision;
    ++m_DocumentRevision;
    return {};
}

void DocumentHistory::install(TrackerDocument& live, TrackerDocument value)
{
    if (!bind(live))
    {
        throw std::logic_error("A document history belongs to one live document.");
    }
    auto installed = installedValue(std::move(value), live);
    live.swap(installed);
}

bool DocumentHistory::bind(const TrackerDocument& live) noexcept
{
    if (m_Live == nullptr)
    {
        m_Live = &live;
    }
    return m_Live == &live;
}

std::unexpected<CommandError> DocumentHistory::foreign()
{
    return std::unexpected(
        CommandError{.code = CommandErrorCode::ForeignDocument, .message = "A document history belongs to one live document."});
}

void DocumentHistory::assign(TrackerDocument& live, TrackerDocument incoming)
{
    install(live, std::move(incoming));
    clear();
}

void DocumentHistory::update(TrackerDocument& live, TrackerDocument next)
{
    install(live, std::move(next));
    ++m_DocumentRevision;
}

void DocumentHistory::clear() noexcept
{
    m_Entries.clear();
    m_Head = 0;
    m_SavedHeads.fill(0);
    ++m_Revision;
    ++m_DocumentRevision;
}

void DocumentHistory::markSaved(SaveTarget target) noexcept
{
    m_SavedHeads[index(target)] = m_Head;
}

void DocumentHistory::invalidateSaved(SaveTarget target) noexcept
{
    m_SavedHeads[index(target)].reset();
}

bool DocumentHistory::isModified(SaveTarget target) const noexcept
{
    return m_SavedHeads[index(target)] != m_Head;
}

} // namespace motrack::model
