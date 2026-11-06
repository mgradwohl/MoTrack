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
    bool removePointMass(LegacyProject& project, std::size_t track);
    bool markPointMass(LegacyProject& project, std::size_t track, Mark mark);
    bool removeMark(LegacyProject& project, std::size_t track, int frame);
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

    using Edit = std::variant<AddTrack, RenameTrack, RemoveTrack, ChangeMark, ChangeCalibration, ChangeClip>;
    static void apply(LegacyProject& project, const Edit& edit, bool forward);
    void append(LegacyProject& project, Edit edit);

    std::vector<Edit> m_Commands;
    std::size_t m_Head = 0;
    std::optional<std::size_t> m_SavedHead = 0;
};

} // namespace motrack
