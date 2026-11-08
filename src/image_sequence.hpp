#pragma once

#include "legacy_project.hpp"
#include "local_media_path.hpp"

#include <QImage>
#include <QStringList>

#include <map>
#include <optional>

namespace motrack
{

struct ImageFrame
{
    int number;
    QImage image;
};

class ImageSequence
{
  public:
    static std::optional<ImageSequence>
    open(const QString& projectPath, const Clip& clip, const std::map<std::string, QString>& mediaLocations = {});

    [[nodiscard]] int stepCount() const;
    [[nodiscard]] std::optional<ImageFrame> readStep(int step) const;

  private:
    QStringList m_Paths;
    int m_FirstFrame = 0;
    int m_StepSize = 1;
    int m_StepCount = 0;
};

} // namespace motrack
