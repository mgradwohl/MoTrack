#pragma once

#include "science/filters/filter_image.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace motrack::science
{

/// Every filter in Tracker 6.3.5's registry (TrackerPanel), one per OSP class.
enum class VideoFilterKind : std::uint8_t
{
    Deinterlace,
    Ghost,
    Strobe,
    DarkGhost,
    Negative,
    GrayScale,
    Log,
    Brightness,
    Baseline,
    Sum,
    Resize,
    Rotate,
    Perspective,
    RadialDistortion,
};

/// The kinds in Tracker's registration order.
[[nodiscard]] std::span<const VideoFilterKind> trackerVideoFilterKinds() noexcept;
/// The fully qualified OSP class name saved in Tracker XML, e.g. org.opensourcephysics.media.core.GhostFilter.
[[nodiscard]] std::string_view videoFilterClassName(VideoFilterKind kind) noexcept;
/// The kind for a saved class name; nullopt for an unknown or unregistered class (BarrelPincushionFilter).
[[nodiscard]] std::optional<VideoFilterKind> videoFilterKindFromClassName(std::string_view className) noexcept;
/// True for filters whose output depends on earlier evaluations (ghost, dark ghost, strobe, sum).
[[nodiscard]] bool isTemporalVideoFilter(VideoFilterKind kind) noexcept;

/// Where Tracker 6.3.5 has an arithmetic defect, a filter either corrects it (MoTrack's default) or
/// reproduces the reference bit for bit. Tracker635 exists for the oracle tests only: by the maintainer's
/// decision of 2026-11-22 MoTrack offers no user-facing Tracker-compatible mode. See docs/video-filters.md.
enum class FilterArithmetic : std::uint8_t
{
    Corrected,
    Tracker635,
};

/// A fresh identity for an image buffer. Identities are never reused within a process.
[[nodiscard]] std::uint64_t newFilterImageIdentity() noexcept;

/**
 * Base of the OSP `Filter` ports. A filter owns its output image and its temporal state. It is not
 * thread-safe: one worker owns it, and parameter changes must be marshalled to that worker (or applied to a
 * clone()) rather than made while it evaluates.
 *
 * As in OSP, a filter (re)initializes when it is evaluated for the first time, when its input is a
 * different image buffer than last time (a different identity or size), and after a change that OSP
 * implements by dropping its source (`source = null`): enabling or disabling, or clearing, a ghost or
 * strobe filter; switching strobe trails; any resize factor change; a rotation change. Initializing
 * allocates a new, black output with a new identity, which in turn re-initializes every enabled filter
 * after it in a stack. docs/video-filters.md lists the state and seek rules.
 */
class VideoFilter
{
  public:
    virtual ~VideoFilter() = default;
    VideoFilter& operator=(const VideoFilter&) = delete;
    VideoFilter(VideoFilter&&) = delete;
    VideoFilter& operator=(VideoFilter&&) = delete;

    [[nodiscard]] VideoFilterKind kind() const noexcept
    {
        return m_Kind;
    }

    [[nodiscard]] bool isEnabled() const noexcept
    {
        return m_Enabled;
    }

    /// Tracker's Enabled menu item and Disable/Enable button. A disabled filter is skipped by a stack.
    virtual void setEnabled(bool enabled);
    /// Tracker's Clear (or Reset) button; filters without one ignore it.
    virtual void clear();

    /// An independent copy with the same parameters, temporal state, output image and buffer identities, so
    /// that the copy continues exactly where this filter stands. Used to evaluate speculatively (a seek
    /// that may be cancelled) and keep only the surviving state.
    [[nodiscard]] virtual std::unique_ptr<VideoFilter> clone() const = 0;

    /**
     * One evaluation: OSP `Filter.getFilteredImage` for an enabled filter. Temporal filters advance their
     * state on every evaluation, including a repeated evaluation of the same frame.
     *
     * @param input the image to filter; its colour bytes are read, its alpha ignored.
     * @param inputIdentity identifies the buffer holding `input` (newFilterImageIdentity()); pass the
     *        same value while successive frames are drawn into the same buffer.
     * @param videoFrame the video frame number shown (not the clip step); perspective key frames use it.
     * @return the filter's output, valid until its next evaluation or destruction. A disabled filter, an
     *         empty input, or an output size with a zero dimension or more than INT_MAX pixels (where OSP
     *         throws, #222), returns `input` itself unchanged and leaves the filter uninitialized.
     */
    const FilterImage& apply(const FilterImage& input, std::uint64_t inputIdentity, int videoFrame = 0);
    /// The result may be the argument itself, so a temporary would dangle.
    const FilterImage& apply(FilterImage&& input, std::uint64_t inputIdentity, int videoFrame = 0) = delete;

    /// The identity of the current output buffer; it changes each time the filter initializes.
    [[nodiscard]] std::uint64_t outputIdentity() const noexcept
    {
        return m_OutputIdentity;
    }

    /// True once initialized for the current input buffer (OSP's `source != null`).
    [[nodiscard]] bool isInitialized() const noexcept
    {
        return m_Initialized;
    }

  protected:
    explicit VideoFilter(VideoFilterKind kind) noexcept : m_Kind(kind)
    {}

    /// For clone(): copies every member, including the output image and its identity.
    VideoFilter(const VideoFilter&) = default;

    /// OSP's `source = null`: the next evaluation re-initializes.
    void invalidateSource() noexcept
    {
        m_Initialized = false;
    }

    /// Output size for an input size (OSP initializeSource). May adjust parameters, as ResizeFilter does.
    virtual std::pair<int, int> outputSize(int width, int height);
    /// OSP initializeSubclass: called after a new output has been allocated.
    virtual void initialize(const FilterImage& input);
    /// OSP setOutputPixels.
    virtual void render(const FilterImage& input, FilterImage& output, int videoFrame) = 0;

  private:
    VideoFilterKind m_Kind;
    bool m_Enabled = true;
    bool m_Initialized = false;
    std::uint64_t m_SourceIdentity = 0;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    std::uint64_t m_OutputIdentity = 0;
    FilterImage m_Output;
};

/// A new filter of `kind` with Tracker's defaults.
[[nodiscard]] std::unique_ptr<VideoFilter> makeVideoFilter(VideoFilterKind kind);

/**
 * An ordered filter stack: OSP `FilterStack` as Tracker's VideoAdapter drives it. Filters apply in
 * index order; disabled filters are skipped; a stack without an enabled filter returns the frame itself.
 *
 * The stack stands in for VideoAdapter's single TYPE_INT_RGB frame buffer: successive frames of one
 * size count as the same buffer, so temporal state carries across frame changes in either direction,
 * frame skips and clip changes. A new frame size, or replaceSource() (for example a different video),
 * re-initializes every enabled filter. Like its filters, a stack belongs to one worker at a time.
 */
class VideoFilterStack
{
  public:
    VideoFilterStack();

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Filters.size();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_Filters.empty();
    }

    /// The filter at `index`; throws std::out_of_range.
    [[nodiscard]] VideoFilter& at(std::size_t index);
    [[nodiscard]] const VideoFilter& at(std::size_t index) const;

    /// Appends a filter (FilterStack.addFilter). Throws std::invalid_argument for null.
    VideoFilter& add(std::unique_ptr<VideoFilter> filter);
    /// Inserts at `index`, clamped to [0, size()] (FilterStack.insertFilter).
    VideoFilter& insert(std::size_t index, std::unique_ptr<VideoFilter> filter);
    /// Removes and returns the filter at `index`; nullptr when out of range.
    std::unique_ptr<VideoFilter> remove(std::size_t index);
    /// Removes every filter (Video > Filters > Clear).
    void clear() noexcept;

    /// TrackerPanel's step-number change: every sum filter, enabled or not, adds its next input.
    void stepChanged();
    /// Treats the next frame as a new frame buffer.
    void replaceSource() noexcept;

    /// A copy of every filter and the source state (see VideoFilter::clone), including pending sum steps:
    /// evaluate the copy for a seek that may be cancelled, then keep it or discard it.
    [[nodiscard]] VideoFilterStack clone() const;

    /**
     * Evaluates the stack once for `frame` (VideoAdapter.getImage with an invalid filtered image).
     * Tracker evaluates after each frame change and after any filter property change, so repeated
     * evaluations of one frame advance temporal filters; callers decide when to evaluate.
     *
     * @return the last enabled filter's output, or `frame` itself; valid until the next evaluation or a
     *         change to the stack.
     */
    const FilterImage& apply(const FilterImage& frame, int videoFrame = 0);
    const FilterImage& apply(FilterImage&& frame, int videoFrame = 0) = delete;

  private:
    std::vector<std::unique_ptr<VideoFilter>> m_Filters;
    std::uint64_t m_SourceIdentity;
    int m_SourceWidth = -1;
    int m_SourceHeight = -1;
};

} // namespace motrack::science
