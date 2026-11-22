#pragma once

#include <optional>
#include <span>
#include <vector>

namespace motrack::science
{

/// A planar world-space quantity: a position (length units), or a velocity or acceleration in length
/// units per clip step, per second, or per second squared, as each function documents.
struct PlanarValue
{
    double x;
    double y;

    friend bool operator==(const PlanarValue&, const PlanarValue&) = default;
};

/// Tracker's video-clip lattice. Clip step k is original frame `firstFrame + k * stepSize`.
///
/// Tracker evaluates motion data only on these frames (VideoClip.includesFrame). The step index is
/// what the kernels operate on; the frame numbers matter only where Tracker's arithmetic uses them.
struct ClipLattice
{
    int firstFrame = 0;
    int stepSize = 1;
    int stepCount = 0;

    /// True for a non-negative first frame, a positive stride and a step count whose last frame fits
    /// in an `int`, which is what Tracker's VideoClip can represent.
    [[nodiscard]] bool valid() const noexcept;

    /// The original frame number of clip step `step` (requires a valid lattice and step in range).
    [[nodiscard]] int frame(int step) const noexcept;

    friend bool operator==(const ClipLattice&, const ClipLattice&) = default;
};

/// A saved mark: the world position of a point mass at an original frame number.
struct FramePosition
{
    int frame;
    PlanarValue position;
};

/// Places marks on the clip steps, the way PointMass gathers its derivative input.
///
/// Marks on frames outside the clip or between its steps are ignored (Tracker only flags clip frames as
/// valid). A mark with a non-finite coordinate is missing: PointMass.refreshData removes NaN steps, and
/// MoTrack treats infinities the same way. If a frame repeats, its last mark wins, like an array store.
/// Throws std::invalid_argument for an invalid lattice.
[[nodiscard]] std::vector<std::optional<PlanarValue>> clipStepPositions(const ClipLattice& clip, std::span<const FramePosition> marks);

} // namespace motrack::science
