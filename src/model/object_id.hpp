#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace motrack::model
{

/// Persistent identity of an addressable document object: a track, function, parameter, filter or view.
/// It generalizes TrackId and uses the same 64-bit values from the document's never-rewinding counter, so
/// `ObjectId{trackId}` names the same object. Zero is reserved for "no object". An identity survives
/// renames, reordering, deletion and undo, and native save/reopen; a Tracker name is only a label.
/// The wrapper keeps identities from being mixed up with list indices or revisions.
struct ObjectId
{
    std::uint64_t value = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return value != 0;
    }

    constexpr auto operator<=>(const ObjectId&) const = default;
};

} // namespace motrack::model

template <> struct std::hash<motrack::model::ObjectId>
{
    std::size_t operator()(const motrack::model::ObjectId& id) const noexcept
    {
        return std::hash<std::uint64_t>{}(id.value);
    }
};
