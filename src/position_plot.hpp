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
    /// The numbers drawn at the plot's corners: the vertical range beside the top and bottom of the plot area, the
    /// horizontal range below its left and right ends. dataDisplayLocale() with five significant digits; right is
    /// empty when the horizontal range is a single value, and all are empty without samples.
    struct AxisLabels
    {
        QString top;
        QString bottom;
        QString left;
        QString right;
    };

    PositionPlot();
    void setSeries(PlotSeries series, const QString& horizontalLabel, const QString& verticalLabel);
    void selectFrame(std::optional<int> frame);
    void setFitVisible(bool visible);
    [[nodiscard]] AxisLabels axisLabels() const;
    [[nodiscard]] QString fitSummary() const;
    [[nodiscard]] std::string csv() const;
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
    std::optional<LinearFit> m_Fit;
    bool m_ShowFit = false;
};

} // namespace motrack
