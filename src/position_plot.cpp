#include "position_plot.hpp"

#include <QLocale>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <utility>

namespace motrack
{
namespace
{

double fraction(double value, double minimum, double maximum)
{
    const auto span = maximum / 2.0 - minimum / 2.0;
    const auto ratio = span != 0.0 ? (value / 2.0 - minimum / 2.0) / span : 0.5;
    return std::isfinite(ratio) ? ratio : 0.5;
}

} // namespace

PositionPlot::PositionPlot()
{
    setObjectName("positionPlot");
    setMinimumSize(160, 120);
}

void PositionPlot::setSeries(PlotSeries series, const QString& horizontalLabel, const QString& verticalLabel)
{
    m_Series = std::move(series);
    m_HorizontalLabel = horizontalLabel;
    m_VerticalLabel = verticalLabel;
    setAccessibleName(tr("%1 versus %2").arg(verticalLabel, horizontalLabel));
    if (!m_Series.samples.empty())
    {
        const auto x = std::ranges::minmax_element(m_Series.samples, {}, &PlotSample::horizontal);
        const auto y = std::ranges::minmax_element(m_Series.samples, {}, &PlotSample::vertical);
        m_Bounds = {.left = x.min->horizontal, .right = x.max->horizontal, .bottom = y.min->vertical, .top = y.max->vertical};
    }
    setFitVisible(m_ShowFit);
}

void PositionPlot::selectFrame(std::optional<int> frame)
{
    m_SelectedFrame = frame;
    update();
}

void PositionPlot::setFitVisible(bool visible)
{
    m_ShowFit = visible;
    m_Fit = visible ? fitStraightLine(m_Series.samples) : std::nullopt;
    update();
}

QString PositionPlot::fitSummary() const
{
    if (!m_Fit)
    {
        return tr("Linear fit needs two distinct horizontal values.");
    }
    const QLocale locale;
    const auto& fit = *m_Fit;
    const auto rSquared = fit.rSquared ? locale.toString(*fit.rSquared, 'g', 6) : QString::fromUtf8("—");
    return tr("Slope %1 · intercept %2 · R² %3").arg(locale.toString(fit.slope, 'g', 6), locale.toString(fit.intercept, 'g', 6), rSquared);
}

QRectF PositionPlot::plotArea() const
{
    return QRectF(rect()).adjusted(56, 24, -16, -40);
}

QPointF PositionPlot::position(const PlotSample& sample) const
{
    const auto area = plotArea();
    return {area.left() + fraction(sample.horizontal, m_Bounds.left, m_Bounds.right) * area.width(),
            area.bottom() - fraction(sample.vertical, m_Bounds.bottom, m_Bounds.top) * area.height()};
}

void PositionPlot::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().base());
    painter.setPen(palette().text().color());
    if (m_Series.samples.empty())
    {
        painter.drawText(rect(), Qt::AlignCenter, tr("No measurements to plot."));
        return;
    }

    const auto area = plotArea();
    painter.drawText(QRectF(0, 0, width(), area.top()), Qt::AlignCenter, m_VerticalLabel);
    painter.drawText(QRectF(0, height() - 20, width(), 20), Qt::AlignCenter, m_HorizontalLabel);
    painter.drawText(QRectF(2, area.top() - 8, 50, 16), Qt::AlignRight, QString::number(m_Bounds.top, 'g', 5));
    painter.drawText(QRectF(2, area.bottom() - 8, 50, 16), Qt::AlignRight, QString::number(m_Bounds.bottom, 'g', 5));
    painter.drawText(QRectF(area.left(), area.bottom() + 2, area.width(), 16), Qt::AlignLeft, QString::number(m_Bounds.left, 'g', 5));
    if (m_Bounds.left != m_Bounds.right)
    {
        painter.drawText(QRectF(area.left(), area.bottom() + 2, area.width(), 16), Qt::AlignRight, QString::number(m_Bounds.right, 'g', 5));
    }
    painter.setPen(palette().mid().color());
    painter.drawRect(area);
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_Fit)
    {
        const auto left = std::fma(m_Fit->slope, m_Bounds.left, m_Fit->intercept);
        const auto right = std::fma(m_Fit->slope, m_Bounds.right, m_Fit->intercept);
        if (std::isfinite(left) && std::isfinite(right))
        {
            painter.save();
            painter.setClipRect(area);
            painter.setPen(QPen(palette().text().color(), 1, Qt::DashLine));
            painter.drawLine(position({.frame = 0, .horizontal = m_Bounds.left, .vertical = left}),
                             position({.frame = 0, .horizontal = m_Bounds.right, .vertical = right}));
            painter.restore();
        }
    }
    painter.setPen(Qt::NoPen);
    for (const auto& sample : m_Series.samples)
    {
        const bool selected = m_SelectedFrame == sample.frame;
        const double radius = selected ? 5.0 : 3.0;
        painter.setBrush(selected ? palette().highlight() : palette().link());
        painter.drawEllipse(position(sample), radius, radius);
    }
}

void PositionPlot::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && seekFrame)
    {
        const auto distance = [this, event](const PlotSample& sample)
        {
            const auto delta = position(sample) - event->position();
            return std::hypot(delta.x(), delta.y());
        };
        const auto nearest = std::ranges::min_element(m_Series.samples, {}, distance);
        if (nearest != m_Series.samples.end() && distance(*nearest) <= 10.0)
        {
            seekFrame(nearest->frame);
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

} // namespace motrack
