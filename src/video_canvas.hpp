#pragma once

#include "legacy_project.hpp"

#include <QImage>
#include <QPointF>
#include <QWidget>

#include <functional>
#include <optional>

namespace motrack
{

class VideoCanvas final : public QWidget
{
  public:
    std::function<bool(QPointF, bool)> pick;

    void setCalibrationPoint(std::optional<QPointF> position);
    void setAxes(std::optional<CalibrationKey> axes);
    void setMeasurementPolyline(QList<QPointF> points);
    void display(QImage image, QList<QPointF> marks, const QString& message);

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    [[nodiscard]] QRectF imageArea() const;
    [[nodiscard]] QPointF canvasPoint(const QPointF& pixel, const QRectF& area) const;
    void drawAxes(QPainter& painter, const QRectF& area) const;

    QImage m_Image;
    QList<QPointF> m_Marks;
    QString m_Message;
    std::optional<QPointF> m_CalibrationPoint;
    std::optional<CalibrationKey> m_Axes;
    QList<QPointF> m_MeasurementPolyline;
};

} // namespace motrack
