#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace motrack
{

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
};

struct Clip
{
    int firstFrame = 0;
    int stepSize = 1;
    std::optional<int> stepCount;
    std::vector<std::string> mediaReferences;
};

struct LegacyProject
{
    std::string version;
    std::string description;
    Clip clip;
    std::vector<PointMass> pointMasses;
    std::vector<std::string> unsupportedTrackTypes;
};

class ProjectError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

LegacyProject parseLegacyProject(std::string_view xml);
LegacyProject readLegacyProject(const std::filesystem::path& path);

} // namespace motrack
