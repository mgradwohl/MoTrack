#include "video_canvas.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace motrack
{
namespace
{

constexpr double ZOOM_STEP = 1.25;
constexpr double MAX_ZOOM = 8.0;

} // namespace

void VideoCanvas::setCalibrationPoint(std::optional<QPointF> position)
{
    m_CalibrationPoint = position;
    update();
}

void VideoCanvas::setAxes(std::optional<CalibrationKey> axes)
{
    m_Axes = axes;
    update();
}

void VideoCanvas::setMeasurementPolyline(QList<QPointF> points)
{
    m_MeasurementPolyline = std::move(points);
    update();
}

void VideoCanvas::display(QImage image, QList<QPointF> marks, const QString& message, QList<QLineF> vectors)
{
    m_Image = std::move(image);
    m_Marks = std::move(marks);
    m_Vectors = std::move(vectors);
    m_Message = message;
    if (!m_Image.isNull())
    {
        clampPan();
    }
    setAccessibleName(message);
    update();
}

void VideoCanvas::mousePressEvent(QMouseEvent* event)
{
    const auto button = event->button();
    const bool pan = button == Qt::MiddleButton || (button == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier));
    if (!m_Image.isNull() && m_Zoom > 1.0 && pan)
    {
        m_LastPanPoint = event->position();
        m_PanButton = button;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (!m_Image.isNull() && pick && event->button() == Qt::LeftButton)
    {
        const auto area = imageArea();
        const auto position = event->position();
        if (area.contains(position) && !area.isEmpty())
        {
            const QPointF pixel((position.x() - area.left()) * m_Image.width() / area.width(),
                                (position.y() - area.top()) * m_Image.height() / area.height());
            if (pick(pixel, event->modifiers().testFlag(Qt::ShiftModifier)))
            {
                event->accept();
                return;
            }
        }
    }
    QWidget::mousePressEvent(event);
}

void VideoCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (m_LastPanPoint)
    {
        const auto position = event->position();
        m_Pan += position - *m_LastPanPoint;
        m_LastPanPoint = position;
        clampPan();
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void VideoCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_LastPanPoint && event->button() == m_PanButton)
    {
        m_LastPanPoint.reset();
        m_PanButton = Qt::NoButton;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void VideoCanvas::wheelEvent(QWheelEvent* event)
{
    if (!m_Image.isNull() && event->modifiers().testFlag(Qt::ControlModifier) && event->angleDelta().y() != 0)
    {
        zoomAt(std::pow(ZOOM_STEP, event->angleDelta().y() / 120.0), event->position());
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

void VideoCanvas::resizeEvent(QResizeEvent* event)
{
    clampPan();
    QWidget::resizeEvent(event);
}

void VideoCanvas::zoomIn()
{
    zoomAt(ZOOM_STEP, QRectF(rect()).center());
}

void VideoCanvas::zoomOut()
{
    zoomAt(1.0 / ZOOM_STEP, QRectF(rect()).center());
}

void VideoCanvas::resetZoom()
{
    m_Zoom = 1.0;
    m_Pan = {};
    m_LastPanPoint.reset();
    m_PanButton = Qt::NoButton;
    unsetCursor();
    update();
}

void VideoCanvas::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().base());
    if (m_Image.isNull())
    {
        painter.setPen(palette().text().color());
        painter.drawText(rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap, m_Message);
        return;
    }

    const auto area = imageArea();
    painter.drawImage(area, m_Image);
    painter.setClipRect(area);
    drawAxes(painter, area);
    painter.setPen(QPen(Qt::magenta, 2));
    for (qsizetype index = 0; index < m_MeasurementPolyline.size(); ++index)
    {
        const auto point = canvasPoint(m_MeasurementPolyline.at(index), area);
        if (index > 0)
        {
            painter.drawLine(canvasPoint(m_MeasurementPolyline.at(index - 1), area), point);
        }
        painter.drawEllipse(point, 4, 4);
    }
    painter.setPen(QPen(Qt::green, 2));
    for (const auto& vector : m_Vectors)
    {
        const auto tail = canvasPoint(vector.p1(), area);
        const auto tip = canvasPoint(vector.p2(), area);
        const auto offset = tip - tail;
        const auto length = std::hypot(offset.x(), offset.y());
        if (length > 0.0 && std::isfinite(length))
        {
            const auto back = offset * (std::min(length, 10.0) / length);
            const QPointF wing(-back.y() / 2.0, back.x() / 2.0);
            painter.drawLine(tail, tip);
            painter.drawLine(tip, tip - back + wing);
            painter.drawLine(tip, tip - back - wing);
        }
    }
    painter.setPen(QPen(Qt::yellow, 2));
    for (const auto& mark : m_Marks)
    {
        painter.drawEllipse(canvasPoint(mark, area), 5, 5);
    }
    if (m_CalibrationPoint)
    {
        painter.setPen(QPen(Qt::cyan, 2));
        painter.drawEllipse(canvasPoint(*m_CalibrationPoint, area), 6, 6);
    }
}

QRectF VideoCanvas::imageArea() const
{
    const auto scaled = fittedImageSize() * m_Zoom;
    return {QPointF((width() - scaled.width()) / 2.0, (height() - scaled.height()) / 2.0) + m_Pan, scaled};
}

QSizeF VideoCanvas::fittedImageSize() const
{
    return {m_Image.size().scaled(size(), Qt::KeepAspectRatio)};
}

void VideoCanvas::clampPan()
{
    const auto scaled = fittedImageSize() * m_Zoom;
    const auto x = std::max(0.0, (scaled.width() - width()) / 2.0);
    const auto y = std::max(0.0, (scaled.height() - height()) / 2.0);
    m_Pan = {std::clamp(m_Pan.x(), -x, x), std::clamp(m_Pan.y(), -y, y)};
}

void VideoCanvas::zoomAt(double factor, const QPointF& anchor)
{
    const auto before = imageArea();
    const auto zoom = std::clamp(m_Zoom * factor, 1.0, MAX_ZOOM);
    if (before.isEmpty() || zoom == m_Zoom)
    {
        return;
    }
    const auto offset = anchor - before.topLeft();
    m_Zoom = zoom;
    const auto after = imageArea();
    m_Pan += anchor - after.topLeft() - QPointF(offset.x() * after.width() / before.width(), offset.y() * after.height() / before.height());
    clampPan();
    update();
}

QPointF VideoCanvas::canvasPoint(const QPointF& pixel, const QRectF& area) const
{
    return {area.left() + pixel.x() * area.width() / m_Image.width(), area.top() + pixel.y() * area.height() / m_Image.height()};
}

void VideoCanvas::drawAxes(QPainter& painter, const QRectF& area) const
{
    if (!m_Axes || area.isEmpty())
    {
        return;
    }
    const QPointF pixelOrigin(m_Axes->originX, m_Axes->originY);
    const auto scale = std::max(std::abs(m_Axes->scaleX), std::abs(m_Axes->scaleY));
    if (!QRectF(m_Image.rect()).contains(pixelOrigin) || scale == 0.0)
    {
        return;
    }
    const auto origin = canvasPoint(pixelOrigin, area);
    const auto angle = m_Axes->angleDegrees * (std::numbers::pi / 180.0);
    const auto x = m_Axes->scaleX / scale * area.width() / m_Image.width();
    const auto y = m_Axes->scaleY / scale * area.height() / m_Image.height();
    const auto cosine = std::cos(angle);
    const auto sine = std::sin(angle);
    const auto reach = std::hypot(area.width(), area.height());
    const auto drawAxis = [&](QPointF direction, const QString& label)
    {
        const auto length = std::hypot(direction.x(), direction.y());
        if (length > 0.0)
        {
            direction /= length;
            painter.drawLine(origin - direction * reach, origin + direction * reach);
            painter.drawText(origin + direction * 24.0 + QPointF(4.0, -4.0), label);
        }
    };
    painter.setPen(QPen(Qt::green, 1));
    drawAxis({x * cosine, -y * sine}, "x");
    drawAxis({-x * sine, -y * cosine}, "y");
    painter.drawEllipse(origin, 4, 4);
}

} // namespace motrack
