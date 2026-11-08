#pragma once

#include "legacy_project.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace motrack
{

struct CalibrationDistance
{
    int frame;
    double imageDeltaX;
    double imageDeltaY;
    double knownLength;
    double defaultOriginX;
    double defaultOriginY;
    std::string unit;
};

struct ClipRange
{
    int firstFrame;
    int stepSize;
    std::optional<int> stepCount;
    bool operator==(const ClipRange&) const = default;
};

struct AxesPlacement
{
    int frame;
    double originX;
    double originY;
    double positiveX;
    double positiveY;
};

struct ClipTiming
{
    std::optional<double> frameDurationMs;
    double startTimeMs;
    bool operator==(const ClipTiming&) const = default;
};

[[nodiscard]] ClipRange clipRange(const Clip& clip);
[[nodiscard]] ClipTiming clipTiming(const Clip& clip);

enum class SaveTarget : std::uint8_t
{
    Native,
    Tracker
};

/// An edit history for one project. GUI indices are resolved to persistent IDs when commands are recorded.
/// Undo/redo reject missing or ambiguous targets without advancing history. Clear on document replacement.
class TrackEdits
{
  public:
    void clear();
    void markSaved(SaveTarget target = SaveTarget::Native);
    /// The file no longer represents a known history state (for example, a completed stale save).
    void invalidateSaved(SaveTarget target = SaveTarget::Native);
    [[nodiscard]] bool isModified(SaveTarget target = SaveTarget::Native) const;
    /// Changes after each edit, undo, redo or document reset; detects stale asynchronous save snapshots.
    [[nodiscard]] std::uint64_t revision() const;
    bool createPointMass(LegacyProject& project, std::string name);
    bool renamePointMass(LegacyProject& project, std::size_t track, std::string name);
    bool setPointMassValue(LegacyProject& project, std::size_t track, double mass);
    /// Change the project-wide mass label without converting numerical mass values.
    bool setMassUnit(LegacyProject& project, std::string unit);
    /// Change the time label without converting timing or derived numerical data.
    bool setTimeUnit(LegacyProject& project, std::string unit);
    bool removePointMass(LegacyProject& project, std::size_t track);
    bool markPointMass(LegacyProject& project, std::size_t track, Mark mark);
    bool removeMark(LegacyProject& project, std::size_t track, int frame);
    bool createVector(LegacyProject& project, std::string name);
    bool renameVector(LegacyProject& project, std::size_t track, std::string name);
    bool removeVector(LegacyProject& project, std::size_t track);
    /// Store image-pixel tail and components at an original video frame, independent of calibration.
    bool markVector(LegacyProject& project, std::size_t track, VectorSample sample);
    bool removeVectorSample(LegacyProject& project, std::size_t track, int frame);
    bool calibrateDistance(LegacyProject& project, CalibrationDistance distance);
    bool setCoordinateAxes(LegacyProject& project, AxesPlacement axes);
    bool setClipRange(LegacyProject& project, int firstFrame, int lastFrame, int stepSize);
    /// Change an existing bounded media-free clip's constant timing, in milliseconds.
    /// Model/data and unknown track providers are refused without changing project or history.
    bool setClipTiming(LegacyProject& project, double frameDurationMs, double startTimeMs);
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    bool undo(LegacyProject& project);
    bool redo(LegacyProject& project);

  private:
    struct AddTrack
    {
        std::string name;
        TrackId id;
        std::size_t position;
    };

    struct ChangeMark
    {
        TrackId id;
        int frame;
        std::optional<Mark> after;
        std::optional<Mark> before;
    };

    struct RenameTrack
    {
        TrackId id;
        std::string after;
        std::string before;
    };

    struct RemoveTrack
    {
        std::size_t position;
        PointMass before;
    };

    struct ChangeMass
    {
        TrackId id;
        double after;
        double before;
    };

    struct ChangeCalibration
    {
        Calibration after;
        std::optional<Calibration> before;
        std::string afterUnit;
        std::string beforeUnit;
    };

    struct ChangeMassUnit
    {
        std::string after;
        std::string before;
    };

    struct ChangeClip
    {
        ClipRange after;
        ClipRange before;
        ClipTiming originalTiming;
    };

    struct ChangeTimeUnit
    {
        std::string after;
        std::string before;
    };

    struct ChangeClipTiming
    {
        ClipTiming after;
        ClipTiming before;
        ClipRange originalRange;
    };

    struct AddVector
    {
        std::string name;
        TrackId id;
        std::size_t position;
    };
    struct RenameVector
    {
        TrackId id;
        std::string after;
        std::string before;
    };
    struct RemoveVector
    {
        std::size_t position;
        VectorTrack before;
    };
    struct ChangeVectorSample
    {
        TrackId id;
        int frame;
        std::optional<VectorSample> after;
        std::optional<VectorSample> before;
    };

    using Edit = std::variant<AddTrack,
                              RenameTrack,
                              RemoveTrack,
                              ChangeMass,
                              ChangeMassUnit,
                              ChangeTimeUnit,
                              ChangeMark,
                              ChangeCalibration,
                              ChangeClip,
                              ChangeClipTiming,
                              AddVector,
                              RenameVector,
                              RemoveVector,
                              ChangeVectorSample>;
    static bool apply(LegacyProject& project, const Edit& edit, bool forward);
    bool append(LegacyProject& project, Edit edit);

    std::vector<Edit> m_Commands;
    std::size_t m_Head = 0;
    std::array<std::optional<std::size_t>, 2> m_SavedHeads{0, 0};
    std::uint64_t m_Revision = 0;
};

} // namespace motrack
