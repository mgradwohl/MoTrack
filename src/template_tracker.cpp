#include "template_tracker.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace motrack
{
namespace
{

constexpr int SEARCH_RADIUS = 16;
constexpr int PEAK_RADIUS = 2;
constexpr double MIN_CORRELATION = 0.80;
constexpr double MIN_PEAK_SEPARATION = 0.04;
constexpr double MIN_VARIANCE = 4.0;

struct Candidate
{
    QPoint point;
    double score;
};

} // namespace

std::optional<TemplateTracker> TemplateTracker::fromImage(const QImage& image, QPoint center)
{
    if (image.width() < PATCH_WIDTH || image.height() < PATCH_WIDTH ||
        !image.rect().adjusted(PATCH_RADIUS, PATCH_RADIUS, -PATCH_RADIUS, -PATCH_RADIUS).contains(center))
    {
        return std::nullopt;
    }
    const QRect patch(center - QPoint(PATCH_RADIUS, PATCH_RADIUS), QSize(PATCH_WIDTH, PATCH_WIDTH));
    const auto gray = image.copy(patch).convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull())
    {
        return std::nullopt;
    }
    TemplateTracker result;
    std::size_t index = 0;
    for (int y = 0; y < PATCH_WIDTH; ++y)
    {
        const auto* row = gray.constScanLine(y);
        for (int x = 0; x < PATCH_WIDTH; ++x)
        {
            result.m_Weights.at(index++) = row[x];
        }
    }
    const auto mean = std::accumulate(result.m_Weights.begin(), result.m_Weights.end(), 0.0) / PATCH_PIXELS;
    double squared = 0.0;
    for (auto& value : result.m_Weights)
    {
        value -= mean;
        squared += value * value;
    }
    if (squared < PATCH_PIXELS * MIN_VARIANCE)
    {
        return std::nullopt;
    }
    const auto norm = std::sqrt(squared);
    for (auto& value : result.m_Weights)
    {
        value /= norm;
    }
    return result;
}

std::optional<QPoint> TemplateTracker::find(const QImage& image, QPoint predicted) const
{
    if (image.width() < PATCH_WIDTH || image.height() < PATCH_WIDTH || !image.rect().contains(predicted))
    {
        return std::nullopt;
    }
    const QRect search(predicted - QPoint(SEARCH_RADIUS, SEARCH_RADIUS), predicted + QPoint(SEARCH_RADIUS, SEARCH_RADIUS));
    const auto bounds = search.intersected(image.rect().adjusted(PATCH_RADIUS, PATCH_RADIUS, -PATCH_RADIUS, -PATCH_RADIUS));
    if (bounds.isEmpty())
    {
        return std::nullopt;
    }
    const auto region = bounds.adjusted(-PATCH_RADIUS, -PATCH_RADIUS, PATCH_RADIUS, PATCH_RADIUS);
    const auto gray = image.copy(region).convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull())
    {
        return std::nullopt;
    }
    std::vector<Candidate> candidates;
    for (int y = bounds.top(); y <= bounds.bottom(); ++y)
    {
        for (int x = bounds.left(); x <= bounds.right(); ++x)
        {
            std::size_t index = 0;
            double sum = 0.0;
            double squared = 0.0;
            double dot = 0.0;
            for (int row = y - PATCH_RADIUS; row <= y + PATCH_RADIUS; ++row)
            {
                const auto* pixels = gray.constScanLine(row - region.top());
                for (int column = x - PATCH_RADIUS; column <= x + PATCH_RADIUS; ++column)
                {
                    const double value = pixels[column - region.left()];
                    sum += value;
                    squared += value * value;
                    dot += m_Weights.at(index++) * value;
                }
            }
            const auto energy = squared - sum * sum / PATCH_PIXELS;
            if (energy >= PATCH_PIXELS * MIN_VARIANCE)
            {
                candidates.push_back({.point = {x, y}, .score = dot / std::sqrt(energy)});
            }
        }
    }
    const auto best = std::ranges::max_element(candidates, {}, &Candidate::score);
    if (best == candidates.end() || best->score < MIN_CORRELATION)
    {
        return std::nullopt;
    }
    const bool ambiguous = std::ranges::any_of(candidates,
                                               [&best](const auto& alternative)
                                               {
                                                   const auto offset = alternative.point - best->point;
                                                   return std::max(std::abs(offset.x()), std::abs(offset.y())) > PEAK_RADIUS &&
                                                          alternative.score > best->score - MIN_PEAK_SEPARATION;
                                               });
    return ambiguous ? std::nullopt : std::optional(best->point);
}

} // namespace motrack
