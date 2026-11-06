#pragma once

#include "measurements.hpp"

#include <QCoreApplication>
#include <QWidget>

#include <functional>
#include <optional>

namespace motrack
{

class PositionPlot final : public QWidget
{
    Q_DECLARE_TR_FUNCTIONS(PositionPlot)

  public:
    PositionPlot();
    void setSeries(PlotSeries series, const QString& horizontalLabel, const QString& verticalLabel);
    void selectFrame(std::optional<int> frame);
    std::function<void(int)> seekFrame;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

  private:
    struct Bounds
    {
        double left;
        double right;
        double bottom;
        double top;
    };

    [[nodiscard]] QRectF plotArea() const;
    [[nodiscard]] QPointF position(const PlotSample& sample) const;

    PlotSeries m_Series;
    Bounds m_Bounds{};
    QString m_HorizontalLabel;
    QString m_VerticalLabel;
    std::optional<int> m_SelectedFrame;
};

} // namespace motrack
