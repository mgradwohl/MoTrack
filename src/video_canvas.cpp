#include "video_canvas.hpp"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace motrack
{

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

void VideoCanvas::display(QImage image, QList<QPointF> marks, const QString& message)
{
    m_Image = std::move(image);
    m_Marks = std::move(marks);
    m_Message = message;
    setAccessibleName(message);
    update();
}

void VideoCanvas::mousePressEvent(QMouseEvent* event)
{
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
    const auto scaled = m_Image.size().scaled(size(), Qt::KeepAspectRatio);
    return {QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2), scaled};
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
