#pragma once

#include "legacy_project.hpp"

#include <QImage>
#include <QStringList>

#include <optional>

namespace motrack
{

struct ImageFrame
{
    int number;
    QImage image;
};

/// Resolves a local media reference from a Tracker document. Archive and URL references return empty.
QString resolveLocalMediaPath(const QString& projectPath, const std::string& reference);

class ImageSequence
{
  public:
    static std::optional<ImageSequence> open(const QString& projectPath, const Clip& clip);

    [[nodiscard]] int stepCount() const;
    [[nodiscard]] std::optional<ImageFrame> readStep(int step) const;

  private:
    QStringList m_Paths;
    int m_FirstFrame = 0;
    int m_StepSize = 1;
    int m_StepCount = 0;
};

} // namespace motrack
