#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace motrack
{

inline constexpr double MIN_POINT_MASS = 1e-30;
inline constexpr auto MAX_TRACKER_XML_BYTES = std::size_t{32} * 1024 * 1024;
using TrackId = std::uint64_t;

struct Mark
{
    int frame;
    double x;
    double y;
};

struct PointMass
{
    std::string name;
    std::vector<Mark> marks;
    double mass = 1.0;
    TrackId id = 0;
};

struct VectorSample
{
    int frame;
    double tailX;
    double tailY;
    double componentX;
    double componentY;
};

struct VectorTrack
{
    std::string name;
    std::vector<VectorSample> samples;
    TrackId id = 0;
};

struct Clip
{
    int firstFrame = 0;
    int stepSize = 1;
    std::optional<int> stepCount;
    std::vector<std::string> mediaReferences;
    std::optional<double> frameDurationMs;
    double startTimeMs = 0.0;
    std::optional<int> sourceFrameCount = std::nullopt;
};

struct CalibrationKey
{
    int frame;
    double originX;
    double originY;
    double angleDegrees;
    double scaleX;
    double scaleY;
};

struct Calibration
{
    /// Sorted by original video frame. Each key applies until the next key.
    std::vector<CalibrationKey> keyframes;
    bool fixedOrigin = false;
    bool fixedAngle = false;
    bool fixedScale = false;
};

struct LegacyProject
{
    std::string version;
    std::string description;
    Clip clip;
    std::optional<Calibration> calibration;
    std::string lengthUnit;
    std::string massUnit = "kg";
    std::vector<PointMass> pointMasses;
    std::vector<VectorTrack> vectors{};
    std::vector<std::string> unsupportedTrackTypes;
    /// Document-scoped allocation counter. Never rewind after deleting or undoing a track.
    TrackId nextTrackId = 1;
};

class ProjectError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

enum class TrackKind : std::uint8_t
{
    PointMass,
    Vector
};

struct TrackSourceBinding
{
    TrackId trackId;
    /// Zero-based ordinal among all direct property children of the original tracks property.
    std::size_t sourceIndex;
    TrackKind kind;
    bool operator==(const TrackSourceBinding&) const = default;
};

/// Immutable import baseline, kept separately from editable measurements. Bytes retain the XML encoding,
/// comments, unknown objects and properties. This is not a serialization of subsequent edits.
struct TrackerSource
{
    std::string xml;
    /// Null means source associations were never recorded, including migration from native versions 1-3.
    /// Known bindings retain deleted tracks so their source objects can later be removed on export.
    std::optional<std::vector<TrackSourceBinding>> bindings = std::nullopt;
};

struct ImportedTrackerProject
{
    LegacyProject project;
    TrackerSource source;
};

LegacyProject parseLegacyProject(std::string_view xml);
LegacyProject readLegacyProject(const std::filesystem::path& path);
/// Reads and parses one bounded snapshot, retaining exactly the bytes that produced the project.
ImportedTrackerProject readTrackerProject(const std::filesystem::path& path);
/// Validate source XML and, when known, complete and unambiguous bindings to its editable track objects.
/// Deleted track IDs are allowed, but all bound IDs must remain reserved by the document counter.
void validateTrackerSource(const TrackerSource& source, const LegacyProject& project);

} // namespace motrack
