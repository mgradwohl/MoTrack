#include "science/fit/curve_fit.hpp"

#include "science/fit/fit_internal.hpp"
#include "science/fit/minimizers.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// A port of DatasetCurveFitter.fit (OSP d56207b, Tracker 6.3.5) and the methods it calls. The Java
// fitter fits a "test function" whose fixed parameters are substituted, minimizes through an objective
// that writes every evaluated parameter vector into that function, and then reads the function's
// parameters: the last vector evaluated, which is not always the minimizer's result. `Curve` carries
// that state. The Corrected dialect keeps each minimizer's best vector instead and treats a vector for
// which the model is undefined at a valid point as undefined rather than as zero deviations.

namespace motrack::science
{
namespace
{

using fit_detail::evaluatePolynomial;

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();
/// Dataset.append keeps at most the last Dataset.defaultMaxPoints (16 * 1024) rows times
/// Dataset.maxPointsMultiplier, which Tracker.start sets to 32: 524288 rows, a deliberate capacity limit.
constexpr std::size_t TRACKER_DATASET_CAPACITY = std::size_t{16} * 1024 * 32;
/// getUncertainties widens or narrows its parameter step at most ten times.
constexpr int UNCERTAINTY_TRIES = 10;
/// getUncertainties aims for 0.001 <= chi2(p - delta) + chi2(p + delta) - 2 chi2_min <= 2.
constexpr double SMALLEST_CURVATURE = 0.001;
/// Model evaluations between two calls of the cancellation callback.
constexpr std::size_t CANCEL_POLL_EVALUATIONS = 4096;
/// Corrected restarts LevenbergMarquardt from its best point while it keeps lowering the deviation
/// without reaching its tolerance (each run stops after about three accepted steps), at most this often.
constexpr int LEVENBERG_MARQUARDT_RUNS = 100;

struct Points
{
    std::vector<double> x;
    std::vector<double> y;

    [[nodiscard]] std::size_t size() const noexcept
    {
        return x.size();
    }
};

/// What the fitter receives in Tracker: Dataset.append moves rows with NaN/infinite x or infinite y
/// to the front without shortening the arrays (it ignores removeBadData's count), so the last rows
/// appear twice; getValidXPoints then drops NaN y.
Points trackerPoints(std::span<const double> x, std::span<const double> y)
{
    const std::size_t offset = x.size() > TRACKER_DATASET_CAPACITY ? x.size() - TRACKER_DATASET_CAPACITY : 0;
    std::vector<double> rowsX(x.begin() + static_cast<std::ptrdiff_t>(offset), x.end());
    std::vector<double> rowsY(y.begin() + static_cast<std::ptrdiff_t>(offset), y.end());
    std::size_t removed = 0;
    for (std::size_t i = 0; i < rowsX.size(); ++i)
    {
        const double xi = rowsX[i];
        const double yi = rowsY[i];
        if (std::isnan(xi) || std::isinf(xi) || std::isinf(yi))
        {
            ++removed;
            continue;
        }
        if (removed > 0)
        {
            rowsX[i - removed] = xi;
            rowsY[i - removed] = yi;
        }
    }
    Points points;
    for (std::size_t i = 0; i < rowsX.size(); ++i)
    {
        if (!std::isnan(rowsY[i]))
        {
            points.x.push_back(rowsX[i]);
            points.y.push_back(rowsY[i]);
        }
    }
    return points;
}

Points finitePoints(std::span<const double> x, std::span<const double> y)
{
    Points points;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        if (std::isfinite(x[i]) && std::isfinite(y[i]))
        {
            points.x.push_back(x[i]);
            points.y.push_back(y[i]);
        }
    }
    return points;
}

/// A fit function with some parameters fixed, evaluated as the Java object would be: a KnownPolynomial
/// by Horner's rule, a UserFunction through the model with a NaN result reported as 0 plus a flag.
class Curve
{
  public:
    Curve(const FitFunction& function, std::vector<double> values, const std::vector<bool>& fixed, bool knownPolynomial)
        : m_Function(&function), m_Values(std::move(values)), m_KnownPolynomial(knownPolynomial)
    {
        for (std::size_t i = 0; i < m_Values.size(); ++i)
        {
            if (fixed.empty() || !fixed[i])
            {
                m_Free.push_back(i);
            }
        }
    }

    [[nodiscard]] const std::vector<double>& values() const noexcept
    {
        return m_Values;
    }

    [[nodiscard]] const std::vector<std::size_t>& free() const noexcept
    {
        return m_Free;
    }

    [[nodiscard]] std::vector<double> freeValues() const
    {
        std::vector<double> result;
        result.reserve(m_Free.size());
        for (const std::size_t index : m_Free)
        {
            result.push_back(m_Values[index]);
        }
        return result;
    }

    void setFreeValues(std::span<const double> values)
    {
        for (std::size_t k = 0; k < m_Free.size(); ++k)
        {
            m_Values[m_Free[k]] = values[k];
        }
    }

    void setValues(std::vector<double> values)
    {
        m_Values = std::move(values);
    }

    /// f(x); `undefined` becomes true when a UserFunction evaluated to NaN (OSP returns 0 then).
    [[nodiscard]] double value(double x, bool& undefined) const
    {
        if (m_KnownPolynomial)
        {
            m_Coefficients.assign(m_Values.rbegin(), m_Values.rend());
            return evaluatePolynomial(m_Coefficients, x);
        }
        const double result = m_Function->model(x, m_Values);
        if (std::isnan(result))
        {
            undefined = true;
            return 0.0;
        }
        return result;
    }

    /// getTestFunction(f, name, value): a UserFunction with one more parameter fixed.
    [[nodiscard]] Curve fixing(std::size_t parameter, double value) const
    {
        Curve test = *this;
        test.m_KnownPolynomial = false;
        test.m_Values[parameter] = value;
        std::erase(test.m_Free, parameter);
        return test;
    }

  private:
    const FitFunction* m_Function;
    std::vector<double> m_Values;
    std::vector<std::size_t> m_Free;
    bool m_KnownPolynomial;
    mutable std::vector<double> m_Coefficients;
};

/// The state DatasetCurveFitter keeps across one fit() call.
struct Session
{
    const Points& points;
    FitDialect dialect;
    const std::function<bool()>& cancelled;
    /// fitEvaluatedToNaN after the latest getDevSquared.
    bool nonReal = false;
    bool stopped = false;
    std::size_t sincePoll = 0;

    [[nodiscard]] bool corrected() const noexcept
    {
        return dialect == FitDialect::Corrected;
    }

    bool cancel()
    {
        if (!stopped && cancelled && cancelled())
        {
            stopped = true;
        }
        return stopped;
    }

    /// Counts `evaluations` model evaluations and calls the cancellation callback every CANCEL_POLL_EVALUATIONS.
    bool poll(std::size_t evaluations)
    {
        sincePoll += evaluations;
        if (sincePoll < CANCEL_POLL_EVALUATIONS)
        {
            return stopped;
        }
        sincePoll = 0;
        return cancel();
    }
};

/// getDevSquared: the sum of squared deviations, or NaN when a UserFunction evaluated to NaN.
double deviationSquared(const Curve& curve, Session& session)
{
    bool undefined = false;
    double total = 0;
    for (std::size_t i = 0; i < session.points.size(); ++i)
    {
        const double next = curve.value(session.points.x[i], undefined);
        const double deviation = next - session.points.y[i];
        total += deviation * deviation;
    }
    session.nonReal = undefined;
    return undefined ? NOT_A_NUMBER : total;
}

struct UserFitOutcome
{
    double deviation = 0.0;
    bool skipped = false;
    bool restored = false;
    bool levenbergMarquardt = false;
    /// Corrected: the deviation decreased, but the minimizer whose result is kept did not reach its tolerance.
    bool unconverged = false;
};

/// The UserFunction branch of fit(): HessianMinimize, then LevenbergMarquardt from the Hessian result
/// when that is worse than the start, restoring the start when neither improves.
///
/// Tracker compares and keeps the last vector each minimizer evaluated, often one it rejected, and counts
/// an undefined model value as zero deviation. Corrected keeps each minimizer's best vector, makes a vector
/// undefined anywhere on the data undefined, and falls back to LevenbergMarquardt whenever Newton stopped
/// short of its tolerance, kept an earlier vector or did not lower the deviation, restarting it from its best point while it keeps
/// lowering the deviation without reaching its tolerance. It keeps a result that lowered the deviation, or
/// one at the start when LevenbergMarquardt's shrinking steps found nothing lower (a minimum already), and
/// marks it unconverged unless the minimizer it came from reached its tolerance.
UserFitOutcome minimizeUserFunction(Curve& curve, double previousDeviation, Session& session)
{
    const std::size_t count = curve.free().size();
    if (count == 0 || count > session.points.size())
    {
        return {.deviation = 0.0, .skipped = true, .restored = false, .levenbergMarquardt = false};
    }
    std::vector<double> params = curve.freeValues();
    const std::vector<double> previous = params;
    // MinimizeUserFunction: writes the parameters into the function, NaN values count as 0.
    const MultiVarObjective objective = [&curve, &session](std::span<const double> values)
    {
        curve.setFreeValues(values);
        const Points& points = session.points;
        // Once cancelled every vector is undefined, which ends both minimizers within a few evaluations.
        if (session.poll(points.size()))
        {
            return NOT_A_NUMBER;
        }
        bool undefined = false;
        double sum = 0.0;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            const double deviation = points.y[i] - curve.value(points.x[i], undefined);
            sum += deviation * deviation;
        }
        return session.corrected() && undefined ? NOT_A_NUMBER : sum;
    };
    const HessianMinimizeResult hessian = hessianMinimize(objective, params, TRACKER_FIT_ITERATIONS, TRACKER_FIT_TOLERANCE);
    if (session.corrected())
    {
        curve.setFreeValues(params);
        double deviation = deviationSquared(curve, session);
        bool usedLevenbergMarquardt = false;
        // Newton converged only if the vector it kept is its last iterate, not the start or the crude guess.
        bool converged = hessian.reachedTolerance && hessian.keptLastIterate;
        if (!converged || !(deviation < previousDeviation))
        {
            usedLevenbergMarquardt = true;
            for (int run = 0; run < LEVENBERG_MARQUARDT_RUNS && !session.stopped; ++run)
            {
                const double before = deviation;
                converged = levenbergMarquardtMinimize(objective, params, TRACKER_FIT_ITERATIONS, TRACKER_FIT_TOLERANCE).reachedTolerance;
                curve.setFreeValues(params);
                deviation = deviationSquared(curve, session);
                if (converged || !(deviation < before))
                {
                    break;
                }
            }
        }
        if (deviation < previousDeviation || (deviation == previousDeviation && converged))
        {
            return {.deviation = deviation,
                    .skipped = false,
                    .restored = false,
                    .levenbergMarquardt = usedLevenbergMarquardt,
                    .unconverged = !converged};
        }
        curve.setFreeValues(previous);
        return {.deviation = previousDeviation,
                .skipped = false,
                .restored = true,
                .levenbergMarquardt = usedLevenbergMarquardt,
                .unconverged = false};
    }
    double deviation = deviationSquared(curve, session);
    bool success = true;
    bool usedLevenbergMarquardt = false;
    if (deviation > previousDeviation)
    {
        usedLevenbergMarquardt = true;
        curve.setFreeValues(previous);
        success = levenbergMarquardtMinimize(objective, params, TRACKER_FIT_ITERATIONS, TRACKER_FIT_TOLERANCE).converged;
        deviation = deviationSquared(curve, session);
    }
    if (!success || deviation > previousDeviation)
    {
        curve.setFreeValues(previous);
        return {.deviation = previousDeviation, .skipped = false, .restored = true, .levenbergMarquardt = true};
    }
    return {.deviation = deviation, .skipped = false, .restored = false, .levenbergMarquardt = usedLevenbergMarquardt};
}

/// fit(test) for a test function inside getUncertainties.
void fitTestFunction(Curve& test, Session& session)
{
    const double previous = deviationSquared(test, session);
    double deviation = 0;
    if (!std::isnan(previous))
    {
        deviation = minimizeUserFunction(test, previous, session).deviation;
    }
    if (deviation == 0)
    {
        static_cast<void>(deviationSquared(test, session));
    }
}

struct Uncertainties
{
    std::vector<double> sigmas;
    std::vector<std::vector<double>> band;
};

/// No sigma (0, undetermined) for each free parameter and NaN for each fixed one, where Tracker would estimate them.
std::optional<Uncertainties> undeterminedUncertainties(const Curve& fitted, const Session& session)
{
    if (fitted.free().empty() || session.points.size() <= fitted.free().size())
    {
        return std::nullopt;
    }
    Uncertainties result{.sigmas = std::vector<double>(fitted.values().size(), NOT_A_NUMBER), .band = {}};
    for (const std::size_t index : fitted.free())
    {
        result.sigmas[index] = 0.0;
    }
    return result;
}

/// getUncertainties: sigma_i = delta * sqrt(2 / (chi2(p_i - delta) + chi2(p_i + delta) - 2 chi2_min)), with
/// chi squared calibrated to N - free parameters at the best fit and the other parameters refitted at each
/// shifted value (Bevington, Data Reduction and Error Analysis, eq. 8.13). Tracker accepts any positive
/// curvature; Corrected leaves sigma undetermined unless the ten tries reached 0.001, since a smaller
/// difference is rounding noise along a direction the data do not determine.
std::optional<Uncertainties> curvatureUncertainties(const Curve& fitted, std::span<const double> original, Session& session)
{
    const std::size_t fitCount = fitted.free().size();
    const std::size_t pointCount = session.points.size();
    if (fitCount == 0 || pointCount <= fitCount)
    {
        return std::nullopt;
    }
    const auto freedom = static_cast<double>(pointCount - fitCount);
    const double sigmaYSquared = deviationSquared(fitted, session) / freedom;
    const double minimumChiSquared = freedom;
    const std::size_t count = fitted.values().size();
    Uncertainties result{.sigmas = std::vector<double>(count, 0.0), .band = {}};
    for (std::size_t i = 0; i < count; ++i)
    {
        if (std::ranges::find(fitted.free(), i) == fitted.free().end())
        {
            result.sigmas[i] = NOT_A_NUMBER;
            continue;
        }
        const double value = fitted.values()[i];
        double delta = (std::abs(value) + 1.0) / 1e5;
        double twiceDeltaChiSquared = 0;
        int tries = 0;
        while ((twiceDeltaChiSquared < SMALLEST_CURVATURE || twiceDeltaChiSquared > 2) && tries < UNCERTAINTY_TRIES)
        {
            if (session.cancel())
            {
                return std::nullopt;
            }
            if (twiceDeltaChiSquared < SMALLEST_CURVATURE && twiceDeltaChiSquared != 0)
            {
                delta *= 10;
            }
            else if (twiceDeltaChiSquared > 2)
            {
                delta /= 10;
            }
            double chiSquared = 0;
            for (int j = 0; j < 2; ++j)
            {
                Curve test = fitted.fixing(i, j == 0 ? value - delta : value + delta);
                fitTestFunction(test, session);
                chiSquared += deviationSquared(test, session) / sigmaYSquared;
            }
            twiceDeltaChiSquared = chiSquared - (2 * minimumChiSquared);
            ++tries;
        }
        const bool determined = session.corrected() ? twiceDeltaChiSquared >= SMALLEST_CURVATURE && std::isfinite(twiceDeltaChiSquared)
                                                    : twiceDeltaChiSquared > 0;
        if (determined)
        {
            const double sigma = delta * std::sqrt(2 / twiceDeltaChiSquared);
            result.sigmas[i] = sigma;
            if (session.cancel())
            {
                return std::nullopt;
            }
            for (int j = 0; j < 2; ++j)
            {
                const double shifted = j == 0 ? value - sigma : value + sigma;
                Curve test = fitted.fixing(i, shifted);
                fitTestFunction(test, session);
                // Tracker's inner `continue` overwrites the refitted values with the original ones.
                std::vector<double> set =
                    session.dialect == FitDialect::Tracker ? std::vector<double>(original.begin(), original.end()) : test.values();
                set[i] = shifted;
                result.band.push_back(std::move(set));
            }
        }
    }
    return result;
}

struct Regression
{
    double correlation = NOT_A_NUMBER;
    /// doLinearRegression discards the uncertainties when x or y is constant (three or more points).
    bool discardUncertainties = false;
};

/// doLinearRegression: the squared Pearson correlation of the data.
Regression linearRegression(const Points& points)
{
    Regression result;
    const std::size_t n = points.size();
    if (n < 3)
    {
        return result;
    }
    const auto count = static_cast<double>(n);
    double meanX = points.x[0];
    double meanY = points.y[0];
    for (std::size_t i = 1; i < n; ++i)
    {
        meanX += points.x[i];
        meanY += points.y[i];
    }
    meanX /= count;
    meanY /= count;
    double sumSquaresX = 0;
    double sumSquaresY = 0;
    double sumCoproduct = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double deltaX = points.x[i] - meanX;
        const double deltaY = points.y[i] - meanY;
        sumSquaresX += deltaX * deltaX;
        sumSquaresY += deltaY * deltaY;
        sumCoproduct += deltaX * deltaY;
    }
    if (sumSquaresX == 0 || sumSquaresY == 0)
    {
        result.discardUncertainties = true;
        return result;
    }
    const double populationX = sumSquaresX / count;
    const double populationY = sumSquaresY / count;
    const double covariance = sumCoproduct / count;
    result.correlation = (covariance * covariance) / (populationX * populationY);
    return result;
}

struct PolynomialFit
{
    FitStatus status = FitStatus::Fitted;
    std::vector<double> parameters;
    std::optional<Uncertainties> uncertainties;
};

/// Corrected polynomial least squares: the normal equations in x' = (x - centre) / halfRange, which keeps
/// them well conditioned for offset or very small x, converted back to powers of x. Standard errors and
/// band come from the covariance s^2 T (V'^T V')^-1 T^T, which the curvature method estimates numerically.
PolynomialFit scaledPolynomialFit(const Points& points, std::span<const double> current)
{
    PolynomialFit fit{.status = FitStatus::Fitted, .parameters = {current.begin(), current.end()}, .uncertainties = std::nullopt};
    const std::size_t count = current.size();
    const std::size_t n = points.size();
    if (n < count)
    {
        fit.status = FitStatus::TooFewPoints;
        return fit;
    }
    std::vector<double> sorted = points.x;
    std::ranges::sort(sorted);
    const auto distinct = static_cast<std::size_t>(std::distance(sorted.begin(), std::ranges::unique(sorted).begin()));
    if (distinct < count)
    {
        fit.status = FitStatus::DegenerateData;
        return fit;
    }
    const double low = sorted.front();
    const double high = sorted.back();
    const double centre = (low / 2) + (high / 2);
    const double scale = (high / 2) - (low / 2);
    std::vector<double> u(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        u[i] = (points.x[i] - centre) / scale;
    }
    std::vector<double> matrix(count * count, 0.0);
    std::vector<double> constants(count, 0.0);
    for (std::size_t i = 0; i < n; ++i)
    {
        double power = 1;
        std::vector<double> powers(count);
        for (std::size_t k = 0; k < count; ++k)
        {
            powers[k] = power;
            power *= u[i];
        }
        for (std::size_t j = 0; j < count; ++j)
        {
            constants[j] += powers[j] * points.y[i];
            for (std::size_t k = 0; k < count; ++k)
            {
                matrix[(j * count) + k] += powers[j] * powers[k];
            }
        }
    }
    fit_detail::LupDecomposition system(matrix, count);
    const std::optional<std::vector<double>> inverse = system.inverse();
    if (!inverse)
    {
        fit.status = FitStatus::DegenerateData;
        return fit;
    }
    const std::vector<double> scaled = system.solve(constants);
    // transform[j][k]: coefficient of x^j in ((x - centre) / scale)^k.
    std::vector<double> transform(count * count, 0.0);
    for (std::size_t k = 0; k < count; ++k)
    {
        std::vector<double> basis(count, 0.0);
        basis[0] = 1;
        for (std::size_t step = 0; step < k; ++step)
        {
            for (std::size_t j = count; j-- > 0;)
            {
                const double shifted = j > 0 ? basis[j - 1] : 0.0;
                basis[j] = (shifted - (centre * basis[j])) / scale;
            }
        }
        for (std::size_t j = 0; j < count; ++j)
        {
            transform[(j * count) + k] = basis[j];
        }
    }
    std::vector<double> coefficients(count, 0.0);
    for (std::size_t j = 0; j < count; ++j)
    {
        for (std::size_t k = 0; k < count; ++k)
        {
            coefficients[j] += transform[(j * count) + k] * scaled[k];
        }
    }
    for (std::size_t j = 0; j < count; ++j)
    {
        fit.parameters[count - 1 - j] = coefficients[j];
    }
    if (!std::ranges::all_of(fit.parameters, [](double value) { return std::isfinite(value); }))
    {
        fit.parameters.assign(current.begin(), current.end());
        fit.status = FitStatus::DegenerateData;
        return fit;
    }
    if (n == count)
    {
        return fit;
    }
    double residualSquares = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double residual = points.y[i] - evaluatePolynomial(scaled, u[i]);
        residualSquares += residual * residual;
    }
    const double variance = residualSquares / static_cast<double>(n - count);
    // covariance of the power coefficients, indexed by power.
    std::vector<double> covariance(count * count, 0.0);
    for (std::size_t a = 0; a < count; ++a)
    {
        for (std::size_t b = 0; b < count; ++b)
        {
            double sum = 0;
            for (std::size_t k = 0; k < count; ++k)
            {
                for (std::size_t l = 0; l < count; ++l)
                {
                    sum += transform[(a * count) + k] * (*inverse)[(k * count) + l] * transform[(b * count) + l];
                }
            }
            covariance[(a * count) + b] = variance * sum;
        }
    }
    Uncertainties uncertainties{.sigmas = std::vector<double>(count, 0.0), .band = {}};
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t powerIndex = count - 1 - i;
        const double sigma = std::sqrt(covariance[(powerIndex * count) + powerIndex]);
        if (!(sigma > 0))
        {
            continue;
        }
        uncertainties.sigmas[i] = sigma;
        // Holding coefficient i at value -/+ sigma, least squares moves the others along its covariance column.
        for (const double sign : {-1.0, 1.0})
        {
            std::vector<double> set = fit.parameters;
            for (std::size_t k = 0; k < count; ++k)
            {
                set[count - 1 - k] += sign * covariance[(k * count) + powerIndex] / sigma;
            }
            set[i] = fit.parameters[i] + (sign * sigma);
            uncertainties.band.push_back(std::move(set));
        }
    }
    fit.uncertainties = std::move(uncertainties);
    return fit;
}

/// KnownPolynomial.setParameterValue ignores NaN; UserFunction stores it.
void assign(std::vector<double>& values, std::size_t index, double value, bool knownPolynomial)
{
    if (knownPolynomial && std::isnan(value))
    {
        return;
    }
    values[index] = value;
}

bool validRequest(const FitFunction& function, std::span<const double> x, std::span<const double> y, const FitOptions& options)
{
    const std::size_t count = function.parameters.size();
    return x.size() == y.size() && static_cast<bool>(function.model) && (options.fixed.empty() || options.fixed.size() == count) &&
           (options.scratchFallback.empty() || options.scratchFallback.size() == count) && function.polynomialDegree >= 0 &&
           (function.polynomialDegree == 0 || count == static_cast<std::size_t>(function.polynomialDegree) + 1);
}

} // namespace

FitResult fitCurve(const FitFunction& function, std::span<const double> x, std::span<const double> y, const FitOptions& options)
{
    FitResult result;
    const std::size_t count = function.parameters.size();
    std::vector<double> values;
    values.reserve(count);
    for (const auto& parameter : function.parameters)
    {
        values.push_back(parameter.value);
    }
    result.parameters = values;
    if (!validRequest(function, x, y, options))
    {
        return result;
    }
    const FitDialect dialect = options.dialect;
    const bool polynomial = function.polynomialDegree > 0;
    std::vector<bool> fixed = options.fixed.empty() ? std::vector<bool>(count, false) : options.fixed;
    const Points points = dialect == FitDialect::Tracker ? trackerPoints(x, y) : finitePoints(x, y);
    Session session{.points = points, .dialect = dialect, .cancelled = options.cancelled};
    const auto cancelledResult = [&result]
    {
        result.status = FitStatus::Cancelled;
        return result;
    };
    result.points = points.size();
    if (session.cancel())
    {
        return cancelledResult();
    }

    if (options.start == FitStart::FromScratch && function.builtIn)
    {
        const std::vector<double> fallback =
            options.scratchFallback.empty() ? fit_detail::builtInDefaults(*function.builtIn) : options.scratchFallback;
        if (fallback.size() == count)
        {
            if (const auto guess = fit_detail::initialGuess(*function.builtIn, points.x, points.y, fallback, dialect))
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    if (!fixed[i])
                    {
                        assign(values, i, (*guess)[i], polynomial);
                    }
                }
            }
        }
    }

    const bool allFixed = std::ranges::all_of(fixed, [](bool flag) { return flag; });
    const bool anyFixed = std::ranges::any_of(fixed, [](bool flag) { return flag; });
    double deviation = 0;
    std::optional<Uncertainties> uncertainties;
    if (allFixed)
    {
        result.status = FitStatus::NothingToFit;
    }
    else
    {
        const bool knownPolynomial = polynomial && !anyFixed;
        Curve fitted(function, values, fixed, knownPolynomial);
        const std::vector<double> original = values;
        // Measured on the function itself: a polynomial with a fixed coefficient is evaluated by Horner's
        // rule here and through its expression while minimizing.
        const double previousDeviation = deviationSquared(Curve(function, values, {}, polynomial), session);
        if (!options.autofit)
        {
            result.status = FitStatus::Manual;
        }
        else if (std::isnan(previousDeviation))
        {
            result.status = FitStatus::UndefinedAtStart;
        }
        else if (knownPolynomial && dialect == FitDialect::Corrected)
        {
            PolynomialFit fit = scaledPolynomialFit(points, values);
            result.status = fit.status;
            result.method = fit.status == FitStatus::Fitted ? FitMethod::ClosedForm : FitMethod::None;
            fitted.setValues(std::move(fit.parameters));
            uncertainties = std::move(fit.uncertainties);
        }
        else
        {
            if (knownPolynomial)
            {
                std::vector<double> coefficients(values.rbegin(), values.rend());
                const bool solved = fit_detail::solvePolynomialNormalEquations(points.x, points.y, coefficients);
                fitted.setValues(std::vector<double>(coefficients.rbegin(), coefficients.rend()));
                const bool finite = std::ranges::all_of(coefficients, [](double value) { return std::isfinite(value); });
                result.status = points.size() < count ? FitStatus::TooFewPoints
                                : solved && finite    ? FitStatus::Fitted
                                                      : FitStatus::DegenerateData;
                result.method = points.size() < count ? FitMethod::None : FitMethod::ClosedForm;
            }
            else
            {
                if (session.cancel())
                {
                    return cancelledResult();
                }
                const UserFitOutcome outcome = minimizeUserFunction(fitted, previousDeviation, session);
                if (session.stopped)
                {
                    return cancelledResult();
                }
                deviation = outcome.deviation;
                result.status = outcome.skipped       ? FitStatus::TooFewPoints
                                : outcome.restored    ? FitStatus::NotImproved
                                : outcome.unconverged ? FitStatus::NotConverged
                                                      : FitStatus::Fitted;
                result.method = outcome.skipped              ? FitMethod::None
                                : outcome.levenbergMarquardt ? FitMethod::LevenbergMarquardt
                                                             : FitMethod::Hessian;
            }
            // With nothing fixed the "original" function is the fitted one; otherwise it still holds the
            // values from before the fit, which Tracker's band therefore uses. Corrected estimates no
            // uncertainty away from a minimum: around starting values the minimizers could not improve, or
            // where they stopped short of their tolerance.
            const bool atMinimum = result.status != FitStatus::NotImproved && result.status != FitStatus::NotConverged;
            uncertainties = dialect == FitDialect::Corrected && !atMinimum
                                ? undeterminedUncertainties(fitted, session)
                                : curvatureUncertainties(fitted, anyFixed ? std::span<const double>(original) : fitted.values(), session);
            if (session.stopped)
            {
                return cancelledResult();
            }
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            if (knownPolynomial)
            {
                values[i] = fitted.values()[i];
            }
            else if (!fixed[i])
            {
                assign(values, i, fitted.values()[i], polynomial);
            }
        }
    }

    const Regression regression = linearRegression(points);
    result.correlation = regression.correlation;
    const Curve final(function, values, {}, polynomial);
    if (deviation == 0)
    {
        deviation = deviationSquared(final, session);
    }
    if (dialect == FitDialect::Corrected)
    {
        static_cast<void>(deviationSquared(final, session));
    }
    result.nonRealValues = session.nonReal;
    result.rmsDeviation =
        count > points.size() && options.autofit ? NOT_A_NUMBER : std::sqrt(deviation / static_cast<double>(points.size()));
    if (uncertainties)
    {
        // Tracker keeps drawing the band after doLinearRegression discards the uncertainties.
        if (dialect == FitDialect::Tracker || !regression.discardUncertainties)
        {
            result.uncertaintyBand = std::move(uncertainties->band);
        }
        if (!regression.discardUncertainties)
        {
            result.uncertainties = std::move(uncertainties->sigmas);
            for (std::size_t i = 0; i < count; ++i)
            {
                const double sigma = result.uncertainties[i];
                result.uncertaintyStatus.push_back(fixed[i]    ? UncertaintyStatus::Fixed
                                                   : sigma > 0 ? UncertaintyStatus::Estimated
                                                               : UncertaintyStatus::Undetermined);
            }
        }
    }
    result.parameters = std::move(values);
    return result;
}

} // namespace motrack::science
