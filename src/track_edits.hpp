#pragma once

#include "legacy_project.hpp"

#include <cstddef>
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

[[nodiscard]] ClipRange clipRange(const Clip& clip);

/// An edit history for one project. Undo restores track order before earlier edits are replayed.
class TrackEdits
{
  public:
    void clear();
    void markSaved();
    [[nodiscard]] bool isModified() const;
    void createPointMass(LegacyProject& project, std::string name);
    bool renamePointMass(LegacyProject& project, std::size_t track, std::string name);
    bool setPointMassValue(LegacyProject& project, std::size_t track, double mass);
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
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    bool undo(LegacyProject& project);
    bool redo(LegacyProject& project);

  private:
    struct AddTrack
    {
        std::string name;
    };

    struct ChangeMark
    {
        std::size_t track;
        int frame;
        std::optional<Mark> after;
        std::optional<Mark> before;
    };

    struct RenameTrack
    {
        std::size_t track;
        std::string after;
        std::string before;
    };

    struct RemoveTrack
    {
        std::size_t track;
        PointMass before;
    };

    struct ChangeMass
    {
        std::size_t track;
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

    struct ChangeClip
    {
        ClipRange after;
        ClipRange before;
    };

    struct AddVector
    {
        std::string name;
    };
    struct RenameVector
    {
        std::size_t track;
        std::string after;
        std::string before;
    };
    struct RemoveVector
    {
        std::size_t track;
        VectorTrack before;
    };
    struct ChangeVectorSample
    {
        std::size_t track;
        int frame;
        std::optional<VectorSample> after;
        std::optional<VectorSample> before;
    };

    using Edit = std::variant<AddTrack,
                              RenameTrack,
                              RemoveTrack,
                              ChangeMass,
                              ChangeMark,
                              ChangeCalibration,
                              ChangeClip,
                              AddVector,
                              RenameVector,
                              RemoveVector,
                              ChangeVectorSample>;
    static void apply(LegacyProject& project, const Edit& edit, bool forward);
    void append(LegacyProject& project, Edit edit);

    std::vector<Edit> m_Commands;
    std::size_t m_Head = 0;
    std::optional<std::size_t> m_SavedHead = 0;
};

} // namespace motrack
