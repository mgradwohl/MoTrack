#pragma once

#include "model/tracker_loaders.hpp"
#include "osp/expected.hpp"
#include "osp/source.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

/// The object families B2 types (docs/tracker-document.md, "Object families in B2"). Each family's value is
/// immutable once shared: a document copy shares it, and an edit replaces the whole value, except frame-keyed
/// fields, which are stored in shared chunks so that an edit copies one chunk rather than a whole track.
///
/// Scalar fields hold Java accessor results (`tracker::Read`): the value Tracker's loader reads, or why it reads
/// none (`Missing` when the file does not save the property, otherwise an unreadable value). An object created in
/// MoTrack has every field it saves set and the others `Missing`. Frame-keyed fields map Tracker's loaded frame
/// number to a finite value in image pixels, and each element keeps the node it was read from, so an edit leaves
/// unknown properties of retained frames in place. Standard-only interface; no Qt.
namespace motrack::model
{

template <typename T> using Read = tracker::Read<T>;

/// An accessor result for a property the file does not save.
template <typename T> [[nodiscard]] Read<T> missing()
{
    return osp::makeUnexpected(osp::DecodeError::Missing);
}

/// The families, in the order of `TypedValue`'s alternatives.
enum class Family : std::uint8_t
{
    Panel,
    Clip,
    Video,
    ClipControl,
    Coordinates,
    PointMass,
    Vector,
    /// Any other element of the panel's `tracks`, or a singleton that B1's import policy cannot present (a
    /// coordinate system with unusable keys or under a reference frame). Preserved, not modeled.
    Unknown,
};

/// Stable spelling used in dumps, e.g. "point-mass".
[[nodiscard]] std::string_view familyName(Family family) noexcept;

/// Where an object lives in the Tracker document: one of the panel's singletons, or an element of `tracks`.
enum class Role : std::uint8_t
{
    Panel,       ///< The root TrackerPanel.
    Clip,        ///< TrackerPanel `videoclip`.
    Video,       ///< VideoClip `video`.
    ClipControl, ///< TrackerPanel `clipcontrol`.
    Coordinates, ///< TrackerPanel `coords`.
    Track,       ///< An element of TrackerPanel `tracks`.
};

[[nodiscard]] std::string_view roleName(Role role) noexcept;

/// PointMass$FrameData: a mark in image pixels (y down).
struct Mark
{
    double x = 0.0;
    double y = 0.0;

    bool operator==(const Mark&) const = default;
};

/// Vector$FrameData: tail position and components in image pixels.
struct VectorSample
{
    double xTail = 0.0;
    double yTail = 0.0;
    double xComponent = 0.0;
    double yComponent = 0.0;

    bool operator==(const VectorSample&) const = default;
};

/// ImageCoordSystem$FrameData as saved: image origin in pixels, angle in degrees, pixels per world unit.
struct CoordKey
{
    double xOrigin = 0.0;
    double yOrigin = 0.0;
    double angle = 0.0;
    double xScale = 0.0;
    double yScale = 0.0;

    bool operator==(const CoordKey&) const = default;
};

/// One element of a frame-keyed field and the baseline node of its frame object, if it was read from one.
template <typename T> struct FrameElement
{
    T value{};
    std::optional<osp::NodeId> node{};

    bool operator==(const FrameElement&) const = default;
};

/// A frame-keyed field: Tracker's loaded frame number to an element, in ascending frame order. Copies share the
/// chunks of 256 consecutive frames; `set` and `erase` copy only the chunk they change.
template <typename T> class FrameSeries
{
  public:
    using Element = FrameElement<T>;
    using Entry = std::pair<std::int32_t, Element>;
    static constexpr std::int32_t CHUNK_FRAMES = 256;

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Size;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_Size == 0;
    }

    [[nodiscard]] std::size_t chunkCount() const noexcept
    {
        return m_Chunks.size();
    }

    /// Borrowed until the next change; null when the frame has no element.
    [[nodiscard]] const Element* find(std::int32_t frame) const
    {
        const auto chunk = m_Chunks.find(chunkOf(frame));
        if (chunk == m_Chunks.end())
        {
            return nullptr;
        }
        const auto& entries = chunk->second->entries;
        const auto found = std::ranges::lower_bound(entries, frame, {}, &Entry::first);
        return found != entries.end() && found->first == frame ? &found->second : nullptr;
    }

    /// Inserts or replaces the element at `frame`.
    void set(std::int32_t frame, Element element)
    {
        const auto key = chunkOf(frame);
        auto copy = std::make_shared<Chunk>();
        if (const auto chunk = m_Chunks.find(key); chunk != m_Chunks.end())
        {
            copy->entries = chunk->second->entries;
        }
        auto& entries = copy->entries;
        const auto found = std::ranges::lower_bound(entries, frame, {}, &Entry::first);
        if (found != entries.end() && found->first == frame)
        {
            found->second = std::move(element);
        }
        else
        {
            entries.insert(found, Entry{frame, std::move(element)});
            ++m_Size;
        }
        m_Chunks.insert_or_assign(key, std::move(copy));
    }

    /// Removes the element at `frame`; false when there is none.
    bool erase(std::int32_t frame)
    {
        const auto key = chunkOf(frame);
        const auto chunk = m_Chunks.find(key);
        if (chunk == m_Chunks.end())
        {
            return false;
        }
        const auto& entries = chunk->second->entries;
        const auto found = std::ranges::lower_bound(entries, frame, {}, &Entry::first);
        if (found == entries.end() || found->first != frame)
        {
            return false;
        }
        if (entries.size() == 1)
        {
            m_Chunks.erase(chunk);
        }
        else
        {
            auto copy = std::make_shared<Chunk>(*chunk->second);
            copy->entries.erase(copy->entries.begin() + (found - entries.begin()));
            chunk->second = std::move(copy);
        }
        --m_Size;
        return true;
    }

    /// Calls `visit(frame, element)` in ascending frame order.
    template <typename Visit> void forEach(Visit&& visit) const
    {
        for (const auto& [key, chunk] : m_Chunks)
        {
            for (const auto& [frame, element] : chunk->entries)
            {
                visit(frame, element);
            }
        }
    }

    [[nodiscard]] std::vector<Entry> entries() const
    {
        std::vector<Entry> result;
        result.reserve(m_Size);
        forEach([&](std::int32_t frame, const Element& element) { result.emplace_back(frame, element); });
        return result;
    }

    /// The first and last frames; absent when empty.
    [[nodiscard]] std::optional<std::pair<std::int32_t, std::int32_t>> range() const
    {
        if (m_Chunks.empty())
        {
            return std::nullopt;
        }
        return std::pair{m_Chunks.begin()->second->entries.front().first, m_Chunks.rbegin()->second->entries.back().first};
    }

    /// Whether two series share every chunk, as an unedited copy does.
    [[nodiscard]] bool sharesChunksWith(const FrameSeries& other) const noexcept
    {
        return m_Chunks == other.m_Chunks;
    }

    /// How many chunks the two series share, so that a test or benchmark can check that an edit copied one.
    [[nodiscard]] std::size_t sharedChunkCount(const FrameSeries& other) const
    {
        return static_cast<std::size_t>(std::ranges::count_if(m_Chunks,
                                                              [&](const auto& chunk)
                                                              {
                                                                  const auto found = other.m_Chunks.find(chunk.first);
                                                                  return found != other.m_Chunks.end() && found->second == chunk.second;
                                                              }));
    }

    friend bool operator==(const FrameSeries& left, const FrameSeries& right)
    {
        if (left.m_Size != right.m_Size)
        {
            return false;
        }
        if (left.sharesChunksWith(right))
        {
            return true;
        }
        return left.entries() == right.entries();
    }

  private:
    struct Chunk
    {
        std::vector<Entry> entries;
    };

    [[nodiscard]] static std::int32_t chunkOf(std::int32_t frame) noexcept
    {
        // Floor division, so negative frames (never loaded, but representable) keep ascending chunk order.
        return frame >= 0 ? frame / CHUNK_FRAMES : -1 - ((-1 - frame) / CHUNK_FRAMES);
    }

    std::map<std::int32_t, std::shared_ptr<const Chunk>> m_Chunks;
    std::size_t m_Size = 0;
};

/// TrackerPanel fields. `referenceFrame` and `selectedTrack` are the saved names; they bind through the graph
/// and the view references respectively, never by these strings alone.
struct PanelState
{
    Read<std::string> semanticVersion = missing<std::string>();
    Read<std::string> description = missing<std::string>();
    Read<std::string> lengthUnit = missing<std::string>();
    Read<std::string> massUnit = missing<std::string>();
    Read<std::string> timeUnit = missing<std::string>();
    Read<std::string> referenceFrame = missing<std::string>();
    Read<std::string> selectedTrack = missing<std::string>();

    bool operator==(const PanelState&) const = default;
};

/// VideoClip fields; frame numbers are the file's (`startframe` before Tracker subtracts `frameshift`).
struct ClipState
{
    Read<std::int32_t> startFrame = missing<std::int32_t>();
    Read<std::int32_t> stepSize = missing<std::int32_t>();
    Read<std::int32_t> stepCount = missing<std::int32_t>();
    Read<std::int32_t> frameCount = missing<std::int32_t>(); ///< `video_framecount`
    Read<std::int32_t> frameShift = missing<std::int32_t>();
    Read<double> startTime = missing<double>(); ///< Milliseconds.

    bool operator==(const ClipState&) const = default;
};

/// A video (ImageVideo, XuggleVideo or another class) as saved. The current media spellings live in the
/// document's media extension, not here.
struct VideoState
{
    Read<std::string> path = missing<std::string>();
    /// ImageVideo `paths` in index order; absent when the video saves no `paths` array.
    std::optional<std::vector<std::string>> paths{};
    Read<double> deltaT = missing<double>(); ///< Milliseconds.

    bool operator==(const VideoState&) const = default;
};

/// StepperClipControl, VideoClipControl or another clip control.
struct ClipControlState
{
    Read<double> rate = missing<double>();
    Read<double> deltaT = missing<double>(); ///< Milliseconds; edited by clip timing commands.
    Read<std::int32_t> frame = missing<std::int32_t>();
    Read<bool> looping = missing<bool>();

    bool operator==(const ClipControlState&) const = default;
};

/// ImageCoordSystem: calibration keys by original frame and the fixed flags.
struct CoordinatesState
{
    Read<bool> fixedOrigin = missing<bool>();
    Read<bool> fixedAngle = missing<bool>();
    Read<bool> fixedScale = missing<bool>();
    Read<bool> locked = missing<bool>();
    FrameSeries<CoordKey> keys{};

    bool operator==(const CoordinatesState&) const = default;
};

/// PointMass: marks by Tracker's loaded frame, with the saved `keyFrames`.
struct PointMassState
{
    Read<std::string> name = missing<std::string>();
    Read<double> mass = missing<double>();
    FrameSeries<Mark> marks{};
    Read<std::vector<std::int32_t>> keyFrames = missing<std::vector<std::int32_t>>();

    bool operator==(const PointMassState&) const = default;
};

/// An independent Vector: samples by Tracker's loaded frame.
struct VectorState
{
    Read<std::string> name = missing<std::string>();
    FrameSeries<VectorSample> samples{};

    bool operator==(const VectorState&) const = default;
};

/// An object that is preserved, not modeled: only its label (TTrack's `name` when readable).
struct UnknownState
{
    std::optional<std::string> label{};

    bool operator==(const UnknownState&) const = default;
};

/// One family value; the alternative index is the `Family`.
using TypedValue = std::variant<PanelState, ClipState, VideoState, ClipControlState, CoordinatesState, PointMassState, VectorState, UnknownState>;

[[nodiscard]] inline Family familyOf(const TypedValue& value) noexcept
{
    return static_cast<Family>(value.index());
}

// Tracker's loader semantics for the values its objects hold, shared by the O3 dump and the legacy view.

/// PointMass.setMass: the absolute saved mass, at least MINIMUM_MASS; 1 when none is saved or it is not finite.
inline constexpr double MINIMUM_POINT_MASS = 1e-30;
[[nodiscard]] double loadedMass(const PointMassState& state);
/// PointMass.Loader's key frames (a TreeSet): the saved `keyFrames` when it lists any, otherwise every loaded mark,
/// since "early pointmass xml" saved none. Ascending and unique.
[[nodiscard]] std::vector<std::int32_t> loadedKeyFrames(const PointMassState& state);
/// TTrack's name as B1 presents it: the saved string, or empty.
[[nodiscard]] std::string loadedName(const Read<std::string>& name);
/// A finite saved double, or nothing (Java's NaN for a missing or unreadable value, and infinities).
[[nodiscard]] std::optional<double> finiteValue(const Read<double>& value);

/// The timing-provider guard: constant clip timing edits require media-free experiments whose untyped tracks all
/// leave time alone. Point masses, independent vectors and retained coordinate axes and protractors qualify; any
/// other class (a model, data track or future class) could supply time and blocks the edit.
[[nodiscard]] bool supportsConstantClipTimingEdits(std::span<const std::string> mediaReferences, std::span<const std::string> unknownTrackClasses);

} // namespace motrack::model
