#pragma once

#include <QImage>
#include <QPoint>

#include <array>
#include <optional>

namespace motrack
{

/// A fixed grayscale image patch for following a marked point across nearby frames.
class TemplateTracker
{
  public:
    [[nodiscard]] static std::optional<TemplateTracker> fromImage(const QImage& image, QPoint center);
    [[nodiscard]] std::optional<QPoint> find(const QImage& image, QPoint predicted) const;

  private:
    static constexpr int PATCH_RADIUS = 5;
    static constexpr int PATCH_WIDTH = PATCH_RADIUS * 2 + 1;
    static constexpr int PATCH_PIXELS = PATCH_WIDTH * PATCH_WIDTH;

    std::array<double, PATCH_PIXELS> m_Weights{};
};

} // namespace motrack
