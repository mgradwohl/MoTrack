#include "science/motion/bounce_derivatives.hpp"

#include "science/motion/java_semantics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

// A port of Tracker 6.3.5's BounceDerivatives, BounceModel, BounceParameters and the QR solver of
// BounceMatrix (Kevin Karplus, 2011). Each expression keeps Java's operand order; the library is
// compiled with -ffp-contract=off, so the results equal Tracker's stride-1 results bit for bit.

namespace motrack::science::motion_detail
{
namespace
{

constexpr int BOUNCE_SPILL = 3;                   // PointMass.bounceDerivsSpill
constexpr int WINDOW = 1 + BOUNCE_SPILL * 2;      // window_size: the data points of each fit
constexpr int DEGREE = 2;                         // constant-acceleration polynomial
constexpr int MAX_PARAMETERS = DEGREE + 3;        // the unknown-step model adds a step size and an offset
constexpr int UNKNOWN_STEP_AT = (WINDOW + 1) / 2; // the unknown-step model looks for a step in (3, 4); integer division as in Java
constexpr int DIMENSIONS = 2;

using Sample = std::array<double, DIMENSIONS>;
using Window = std::array<Sample, WINDOW>;

/// BounceModel: the design matrix over window times t = 0 .. 6 and its least-squares pseudo-inverse.
struct Model
{
    int parameterCount = DEGREE + 1;
    bool unknownStep = false;
    bool usesStep = false; // use_step || use_unknown_step
    double stepAt = 0.0;
    std::array<std::array<double, MAX_PARAMETERS>, WINDOW> mapping{};
    std::array<std::array<double, WINDOW>, MAX_PARAMETERS> inverse{};
};

/// BounceParameters: a fit of a model to one window, optionally after removing a known velocity step.
struct Fit
{
    bool exists = false;
    int parameterCount = 0;
    double modelStepAt = 0.0;
    std::array<Sample, MAX_PARAMETERS> parameters{};
    double squareError = 0.0;
    bool initialStep = false;
    double initialStepAt = 0.0;
    Sample initialStepSize{};
};

/// The hypot of JAMA's QRDecomposition (BounceMatrix), which differs from Math.hypot in rounding.
double jamaHypot(double a, double b)
{
    if (std::fabs(a) > std::fabs(b))
    {
        const double r = b / a;
        return std::fabs(a) * std::sqrt(1 + r * r);
    }
    if (b != 0)
    {
        const double r = a / b;
        return std::fabs(b) * std::sqrt(1 + r * r);
    }
    return 0.0;
}

/// BounceMatrix.inverse(): Householder QR of the WINDOW x n design, then solve(identity). The design is
/// always of full rank: its step columns are nonzero at t = 6 and independent of the quadratic.
void invert(Model& model)
{
    const int n = model.parameterCount;
    auto qr = model.mapping;
    std::array<double, MAX_PARAMETERS> diagonal{};
    for (int k = 0; k < n; ++k)
    {
        double norm = 0;
        for (int i = k; i < WINDOW; ++i)
        {
            norm = jamaHypot(norm, qr[i][k]);
        }
        if (norm != 0.0)
        {
            if (qr[k][k] < 0)
            {
                norm = -norm;
            }
            for (int i = k; i < WINDOW; ++i)
            {
                qr[i][k] /= norm;
            }
            qr[k][k] += 1.0;
            for (int j = k + 1; j < n; ++j)
            {
                double s = 0.0;
                for (int i = k; i < WINDOW; ++i)
                {
                    s += qr[i][k] * qr[i][j];
                }
                s = -s / qr[k][k];
                for (int i = k; i < WINDOW; ++i)
                {
                    qr[i][j] += s * qr[i][k];
                }
            }
        }
        diagonal[k] = -norm;
    }
    std::array<std::array<double, WINDOW>, WINDOW> x{};
    for (int i = 0; i < WINDOW; ++i)
    {
        x[i][i] = 1.0;
    }
    for (int k = 0; k < n; ++k)
    {
        for (int j = 0; j < WINDOW; ++j)
        {
            double s = 0.0;
            for (int i = k; i < WINDOW; ++i)
            {
                s += qr[i][k] * x[i][j];
            }
            s = -s / qr[k][k];
            for (int i = k; i < WINDOW; ++i)
            {
                x[i][j] += s * qr[i][k];
            }
        }
    }
    for (int k = n - 1; k >= 0; --k)
    {
        for (int j = 0; j < WINDOW; ++j)
        {
            x[k][j] /= diagonal[k];
        }
        for (int i = 0; i < k; ++i)
        {
            for (int j = 0; j < WINDOW; ++j)
            {
                x[i][j] -= x[k][j] * qr[i][k];
            }
        }
    }
    for (int k = 0; k < n; ++k)
    {
        model.inverse[k] = x[k];
    }
}

/// new BounceModel(WINDOW, DEGREE, whenStep): a pure quadratic for whenStep outside (0, 6), a velocity
/// step at whenStep otherwise, and an unknown step near the window centre for NaN.
Model makeModel(double whenStep)
{
    Model model;
    model.unknownStep = std::isnan(whenStep);
    bool useStep = false;
    if (model.unknownStep)
    {
        model.stepAt = UNKNOWN_STEP_AT;
    }
    else
    {
        useStep = whenStep > 0 && whenStep < WINDOW - 1;
        model.stepAt = useStep ? whenStep : 0;
    }
    model.usesStep = useStep || model.unknownStep;
    model.parameterCount = DEGREE + 1 + (model.unknownStep ? 2 : (useStep ? 1 : 0));
    for (int t = 0; t < WINDOW; ++t)
    {
        int power = 1;
        for (int d = 0; d <= DEGREE; ++d)
        {
            model.mapping[t][d] = power;
            power *= t;
        }
        if (model.usesStep)
        {
            model.mapping[t][DEGREE + 1] = t >= model.stepAt ? (t - model.stepAt) : 0;
        }
        if (model.unknownStep)
        {
            model.mapping[t][DEGREE + 2] = t >= model.stepAt ? 1 : 0;
        }
    }
    invert(model);
    return model;
}

/// BounceModel.fit_xy: parameters = inverse * data and the summed squared residual of both coordinates.
Fit fit(const Model& model, const Window& data)
{
    Fit result{.exists = true, .parameterCount = model.parameterCount, .modelStepAt = model.stepAt};
    const int n = model.parameterCount;
    for (int j = 0; j < DIMENSIONS; ++j)
    {
        for (int i = 0; i < n; ++i)
        {
            double s = 0;
            for (int k = 0; k < WINDOW; ++k)
            {
                s += model.inverse[i][k] * data[k][j];
            }
            result.parameters[i][j] = s;
        }
    }
    Window error{};
    for (int j = 0; j < DIMENSIONS; ++j)
    {
        for (int i = 0; i < WINDOW; ++i)
        {
            double s = 0;
            for (int k = 0; k < n; ++k)
            {
                s += model.mapping[i][k] * result.parameters[k][j];
            }
            error[i][j] = s - data[i][j];
        }
    }
    double squareError = 0;
    for (int t = 0; t < WINDOW; ++t)
    {
        squareError += error[t][0] * error[t][0];
        squareError += error[t][1] * error[t][1];
    }
    result.squareError = squareError;
    return result;
}

/// The 6-argument BounceModel.fit_xy: fits the pure quadratic after subtracting a known velocity step.
/// No fit (Tracker: null) if the adjusted data are NaN, which only overflowing coordinates can cause.
Fit fitWithStep(const Model& model, Window data, double initialStepAt, Sample initialStepSize)
{
    for (int t = 0; t < WINDOW; ++t)
    {
        for (int dimension = 0; dimension < DIMENSIONS; ++dimension)
        {
            data[t][dimension] = data[t][dimension] - (t > initialStepAt ? initialStepSize[dimension] * (t - initialStepAt) : 0);
            if (std::isnan(data[t][dimension]))
            {
                return {};
            }
        }
    }
    Fit result = fit(model, data);
    result.initialStep = true;
    result.initialStepAt = initialStepAt;
    result.initialStepSize = initialStepSize;
    return result;
}

/// The 4-argument fit_xy of the unknown-step model: estimate the step time from each coordinate, refit
/// with a fixed step there and at their dv^2-weighted mean, and keep the fit with the least error.
/// MoTrack skips a non-finite estimate; Tracker would construct another unknown-step model from it and
/// recurse until the stack overflows (only coordinates beyond about 1e154 overflow these weights).
Fit fitUnknownStep(const Model& model, const Window& data)
{
    const Fit estimate = fit(model, data);
    Fit best;
    double combinedStep = 0;
    double weight = 0;
    for (int dimension = 0; dimension < DIMENSIONS; ++dimension)
    {
        const double velocityStep = estimate.parameters[DEGREE + 1][dimension];
        const double extra = estimate.parameters[DEGREE + 2][dimension];
        if (velocityStep == 0)
        {
            continue;
        }
        double tryStep = model.stepAt - extra / velocityStep;
        if (tryStep < 0)
        {
            tryStep = 0.001;
        }
        else if (tryStep >= WINDOW - 1)
        {
            tryStep = WINDOW - 1.001;
        }
        if (std::isnan(tryStep))
        {
            continue;
        }
        const Fit candidate = fit(makeModel(tryStep), data);
        if (!best.exists || best.squareError > candidate.squareError)
        {
            best = candidate;
        }
        combinedStep += velocityStep * velocityStep * tryStep;
        weight += velocityStep * velocityStep;
    }
    if (weight > 0)
    {
        combinedStep /= weight;
    }
    if (!std::isnan(combinedStep))
    {
        const Fit candidate = fit(makeModel(combinedStep), data);
        if (!best.exists || best.squareError > candidate.squareError)
        {
            best = candidate;
        }
    }
    return best;
}

/// BounceParameters.getStepSize for a fit with a step and no initial step: its last parameter. (A fit
/// without a step has modelStepAt == 0, which every caller skips before asking for the size.)
Sample stepSize(const Fit& fit)
{
    Sample result{};
    for (int dimension = 0; dimension < DIMENSIONS; ++dimension)
    {
        result[dimension] += fit.parameters[fit.parameterCount - 1][dimension];
    }
    return result;
}

// Derivatives of a quadratic fit (every fit that BounceDerivatives evaluates uses the step-free model) at
// window time t, plus the removed step: BounceModel.first_deriv/second_deriv and BounceParameters.

PlanarValue firstDerivative(const Fit& fit, double t)
{
    Sample result{};
    double power = 1.;
    for (int d = 1; d <= DEGREE; ++d)
    {
        for (int dimension = 0; dimension < DIMENSIONS; ++dimension)
        {
            result[dimension] += power * d * fit.parameters[d][dimension];
        }
        power *= t;
    }
    if (fit.initialStep && fit.initialStepAt < t)
    {
        for (int dimension = 0; dimension < DIMENSIONS; ++dimension)
        {
            result[dimension] += fit.initialStepSize[dimension];
        }
    }
    return {.x = result[0], .y = result[1]};
}

PlanarValue secondDerivative(const Fit& fit, double t)
{
    Sample result{};
    double power = 1.;
    for (int d = 2; d <= DEGREE; ++d)
    {
        for (int dimension = 0; dimension < DIMENSIONS; ++dimension)
        {
            result[dimension] += power * d * (d - 1) * fit.parameters[d][dimension];
        }
        power *= t;
    }
    // The velocity step becomes an acceleration pulse at the step nearest the window time.
    if (fit.initialStep && javaRoundsToZero(fit.initialStepAt - t))
    {
        for (int dimension = 0; dimension < DIMENSIONS; ++dimension)
        {
            result[dimension] += fit.initialStepSize[dimension];
        }
    }
    return {.x = result[0], .y = result[1]};
}

} // namespace

StepDerivatives bounceStepDerivatives(const ClipLattice& clip, std::span<const std::optional<PlanarValue>> positions)
{
    const int count = clip.stepCount;
    StepDerivatives result{.first = std::vector<std::optional<PlanarValue>>(positions.size()),
                           .second = std::vector<std::optional<PlanarValue>>(positions.size())};
    // Like Tracker's input arrays, a missing step holds NaN.
    constexpr double MISSING = std::numeric_limits<double>::quiet_NaN();
    std::vector<Sample> values(positions.size(), Sample{MISSING, MISSING});
    for (std::size_t step = 0; step < positions.size(); ++step)
    {
        if (const auto& position = positions[step]; position)
        {
            values[step] = {position->x, position->y};
        }
    }
    const auto present = [&](int step)
    {
        return step >= 0 && step < count && !std::isnan(values[static_cast<std::size_t>(step)][0]);
    };
    const auto window = [&](int startStep)
    {
        Window data{};
        for (int t = 0; t < WINDOW; ++t)
        {
            data[t] = values[static_cast<std::size_t>(startStep) + static_cast<std::size_t>(t)];
        }
        return data;
    };
    const auto at = [](auto& items, int step) -> auto&
    {
        return items[static_cast<std::size_t>(step)];
    };

    const Model quadratic = makeModel(0.0);
    const Model unknownStep = makeModel(MISSING);
    std::vector<Fit> polynomialFit(positions.size());
    std::vector<Fit> stepFit(positions.size());
    // The window position of each step: 3 for a centred window, other values for a shifted one.
    std::vector<int> centre(positions.size(), WINDOW / 2);

    for (int c = 0; c < count; ++c)
    {
        int& offset = at(centre, c);
        int highestBad = -1;
        for (int inWindow = WINDOW - 1; inWindow >= 0; --inWindow)
        {
            if (!present(c + inWindow - offset))
            {
                highestBad = inWindow;
                break;
            }
        }
        if (highestBad >= 0)
        {
            // Shift the window just past the nearest missing steps, upward or downward, whichever is closer.
            int lowestBad = WINDOW;
            for (int inWindow = 0; inWindow < WINDOW; ++inWindow)
            {
                if (!present(c + inWindow - offset))
                {
                    lowestBad = inWindow;
                    break;
                }
            }
            const int moveUp = highestBad + 1;
            const int moveDown = WINDOW - lowestBad;
            offset -= moveUp <= moveDown ? moveUp : -moveDown;
            bool complete = true;
            for (int inWindow = WINDOW - 1; inWindow >= 0 && complete; --inWindow)
            {
                complete = present(c + inWindow - offset);
            }
            if (!complete)
            {
                continue;
            }
        }
        const Window data = window(c - offset);
        at(polynomialFit, c) = fit(quadratic, data);
        at(stepFit, c) = fitUnknownStep(unknownStep, data);
    }

    // How much each candidate step reduces the squared error of the windows around it. Tracker writes this
    // search in frame units (i_step_time) and, for a stride above 1, visits frames between clip steps and
    // throws. MoTrack evaluates it on virtual frames start + c, which is Tracker's arithmetic for a stride
    // of 1, so every stride gives Tracker's stride-1 result for the same samples bit for bit.
    const int start = clip.firstFrame;
    const int lastFrame = count == 0 ? start : start + (count - 1);
    std::vector<double> stepValue(positions.size(), 0.0);
    for (int c = 0; c < count; ++c)
    {
        const Fit& candidate = at(stepFit, c);
        if (!candidate.exists || candidate.modelStepAt == 0.0)
        {
            continue;
        }
        const int frame = start + c;
        // i_step_time = i + index_step * (step time - c_at) with index_step = 1 (multiplying by 1 is exact).
        const double frameStepTime = frame + (candidate.modelStepAt - at(centre, c));
        const Sample size = stepSize(candidate);
        const int low = std::max(start, javaDoubleToInt(frameStepTime - 0.5 * (WINDOW - 1) + 0.999)) - start;
        const int high = std::min(lastFrame, javaDoubleToInt(frameStepTime + 0.5 * (WINDOW - 1) + 0.001)) - start;
        for (int windowStep = low; windowStep <= high; ++windowStep)
        {
            const Fit& polynomial = at(polynomialFit, windowStep);
            if (!polynomial.exists)
            {
                continue;
            }
            const int windowFrame = start + windowStep;
            const int windowCentre = at(centre, windowStep);
            const Fit refit = fitWithStep(quadratic, window(windowStep - windowCentre), (frameStepTime - windowFrame) + windowCentre, size);
            if (refit.exists)
            {
                at(stepValue, c) += polynomial.squareError - refit.squareError;
            }
        }
    }

    // Choose steps so that no window holds two, taking the most valuable first (a stable sort, as
    // Java's TimSort with Double.compare).
    std::vector<int> order(positions.size());
    for (int c = 0; c < count; ++c)
    {
        at(order, c) = c;
    }
    std::ranges::stable_sort(order,
                             [&](int first, int second) { return javaDoubleCompare(at(stepValue, second), at(stepValue, first)) < 0; });
    std::vector<Fit> chosen(positions.size());
    const double large = 0.6 * WINDOW;
    for (const int c : order)
    {
        const Fit& candidate = at(stepFit, c);
        if (at(chosen, c).exists || !candidate.exists)
        {
            continue;
        }
        if (at(stepValue, c) < large * at(polynomialFit, c).squareError || candidate.modelStepAt == 0.0)
        {
            continue;
        }
        const double clipStepTime = c + candidate.modelStepAt - at(centre, c);
        const Sample size = stepSize(candidate);
        const int below = javaDoubleToInt(clipStepTime);
        for (int windowStep = std::max(0, below - WINDOW + 2); windowStep < below + WINDOW && windowStep < count; ++windowStep)
        {
            const int windowCentre = at(centre, windowStep);
            if (clipStepTime <= windowStep - windowCentre || clipStepTime >= windowStep - windowCentre + WINDOW - 1)
            {
                continue; // the step lies outside this window
            }
            if (at(chosen, windowStep).exists || !at(polynomialFit, windowStep).exists)
            {
                continue;
            }
            at(chosen, windowStep) =
                fitWithStep(quadratic, window(windowStep - windowCentre), clipStepTime - windowStep + windowCentre, size);
        }
    }

    for (int c = 0; c < count; ++c)
    {
        const Fit& polynomial = at(polynomialFit, c);
        if (!polynomial.exists)
        {
            continue;
        }
        const Fit& model = at(chosen, c).exists ? at(chosen, c) : polynomial;
        const double t = at(centre, c);
        at(result.first, c) = firstDerivative(model, t);
        at(result.second, c) = secondDerivative(model, t);
    }
    return result;
}

} // namespace motrack::science::motion_detail
