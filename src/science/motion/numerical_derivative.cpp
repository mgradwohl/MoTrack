#include "science/motion/numerical_derivative.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace motrack::science
{

double centeredDerivative(const RealFunction& f, double x, double h)
{
    return (f(x + h) - f(x - h)) / h / 2.0;
}

double forwardDerivative(const RealFunction& f, double x, double h)
{
    return (-f(x + 2 * h) + 4 * f(x + h) - 3 * f(x)) / h / 2.0;
}

double backwardDerivative(const RealFunction& f, double x, double h)
{
    return (f(x - 2 * h) - 4 * f(x - h) + 3 * f(x)) / h / 2.0;
}

double secondDerivative(const RealFunction& f, double x, double h)
{
    return (f(x + h) - 2 * f(x) + f(x - h)) / h / h;
}

double partialDerivative(const MultivariateFunction& f, std::span<const double> point, std::size_t index, double h)
{
    if (index >= point.size())
    {
        throw std::out_of_range("Partial derivative index outside the point");
    }
    std::vector<double> x(point.begin(), point.end());
    const double n0 = x[index];
    x[index] = n0 + h;
    double d = f(x);
    x[index] = n0 - h;
    d = (d - f(x)) / 2.0 / h;
    return d;
}

RombergResult rombergDerivative(const RealFunction& f, double x, double h, double tolerance)
{
    // Util.defaultNumericalPrecision = Math.sqrt(Double.MIN_VALUE).
    const double precision = std::sqrt(std::numeric_limits<double>::denorm_min());
    constexpr int COLUMNS = 6;
    std::array<double, COLUMNS> d{};
    d[0] = (f(x + h) - f(x - h)) / h / 2.0;
    auto status = RombergStatus::NotConverged;
    for (int j = 1; j <= COLUMNS - 1; ++j)
    {
        d[j] = 0.0;
        double d1 = d[0];
        const double h2 = h;
        h *= 0.5;
        if (h < precision)
        {
            status = RombergStatus::StepBelowPrecision;
            break;
        }
        d[0] = (f(x + h) - f(x - h)) / h2;
        for (int m = 4, i = 1; i <= j; i++, m *= 4)
        {
            const double d2 = d[i];
            d[i] = (m * d[i - 1] - d1) / (m - 1);
            d1 = d2;
        }
        if (std::fabs(d[j] - d[j - 1]) < tolerance)
        {
            return {.value = d[j], .status = RombergStatus::Converged};
        }
    }
    return {.value = d[0], .status = status};
}

} // namespace motrack::science
