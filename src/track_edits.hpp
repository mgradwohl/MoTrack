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

/// An edit history for one open project. Track slots stay stable until their creation is undone.
class TrackEdits
{
  public:
    void clear();
    void createPointMass(LegacyProject& project, std::string name);
    bool markPointMass(LegacyProject& project, std::size_t track, Mark mark);
    bool calibrateDistance(LegacyProject& project, CalibrationDistance distance);
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
        Mark after;
        std::optional<Mark> before;
    };

    struct ChangeCalibration
    {
        Calibration after;
        std::optional<Calibration> before;
        std::string afterUnit;
        std::string beforeUnit;
    };

    using Edit = std::variant<AddTrack, ChangeMark, ChangeCalibration>;
    static void apply(LegacyProject& project, const Edit& edit, bool forward);
    void append(LegacyProject& project, Edit edit);

    std::vector<Edit> m_Commands;
    std::size_t m_Head = 0;
};

} // namespace motrack
