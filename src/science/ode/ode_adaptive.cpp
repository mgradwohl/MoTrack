// Adaptive OSP solvers: the embedded 4(5) pairs and the two fixed-interval multistep wrappers.
// Control flow, step-size formulas and comparisons follow the Java classes exactly, including
// their quirks; the deliberate difference (a missing `break` in ODEMultistepSolver.minus) is marked.

#include "science/ode/ode_solvers_internal.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace motrack::science::detail
{

double javaMax(double a, double b) noexcept
{
    if (a != a)
    {
        return a;
    }
    if (a == 0.0 && b == 0.0 && std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(-0.0))
    {
        return b;
    }
    return a >= b ? a : b;
}

double javaMin(double a, double b) noexcept
{
    if (a != a)
    {
        return a;
    }
    if (a == 0.0 && b == 0.0 && std::bit_cast<std::uint64_t>(b) == std::bit_cast<std::uint64_t>(-0.0))
    {
        return b;
    }
    return a <= b ? a : b;
}

namespace
{

constexpr int EMBEDDED_PAIR_ATTEMPTS = 10;
constexpr int MULTISTEP_MAX_ITERATIONS = 200;

// CashKarp45.java: Cash-Karp coefficients (a, fifth-order b5 and b5 - b4).
constexpr EmbeddedPair CASH_KARP = {
    .a = {{{1.0 / 5.0},
           {3.0 / 40.0, 9.0 / 40.0},
           {3.0 / 10.0, -9.0 / 10.0, 6.0 / 5.0},
           {-11.0 / 54.0, 5.0 / 2.0, -70.0 / 27.0, 35.0 / 27.0},
           {1631.0 / 55296.0, 175.0 / 512.0, 575.0 / 13824.0, 44275.0 / 110592.0, 253.0 / 4096.0}}},
    .b5 = {37. / 378., 0., 250. / 621., 125. / 594., 0., 512. / 1771.},
    .er = {277. / 64512., 0., -6925. / 370944., 6925. / 202752., 277. / 14336., -277. / 7084.},
};

// DormandPrince45.java: the six-stage Dormand-Prince pair as written in OSP.
constexpr EmbeddedPair DORMAND_PRINCE = {
    .a = {{{1.0 / 5.0},
           {3.0 / 40.0, 9.0 / 40.0},
           {3.0 / 10.0, -9.0 / 10.0, 6.0 / 5.0},
           {226.0 / 729.0, -25.0 / 27.0, 880.0 / 729.0, 55.0 / 729.0},
           {-181.0 / 270.0, 5.0 / 2.0, -266.0 / 297.0, -91.0 / 27.0, 189.0 / 55.0}}},
    .b5 = {19.0 / 216.0, 0.0, 1000.0 / 2079.0, -125.0 / 216.0, 81.0 / 88.0, 5.0 / 56.0},
    .er = {-11.0 / 360.0, 0.0, 10.0 / 63.0, -55.0 / 72.0, 27.0 / 40.0, -11.0 / 280.0},
};

} // namespace

EmbeddedPairSolver::EmbeddedPairSolver(
    OdeSolverKind kind, const EmbeddedPair& pair, bool clampsTolerance, OdeRate rate, std::span<const double> initialState)
    : OdeSolver(kind, std::move(rate), initialState), m_Pair(&pair), m_ClampsTolerance(clampsTolerance)
{}

void EmbeddedPairSolver::construct()
{
    initialize(m_StepSize);
}

void EmbeddedPairSolver::initialize(double stepSize)
{
    m_StepSize = stepSize;
    const std::size_t numEqn = state().size();
    // OSP keeps its scratch arrays (and their old values) unless the equation count changes.
    if (m_NumEqn != numEqn)
    {
        m_NumEqn = numEqn;
        m_TempState.assign(numEqn, 0.0);
        for (auto& rate : m_K)
        {
            rate.assign(numEqn, 0.0);
        }
    }
}

void EmbeddedPairSolver::setStepSize(double stepSize)
{
    m_StepSize = stepSize;
}

double EmbeddedPairSolver::stepSize() const noexcept
{
    return m_StepSize;
}

bool EmbeddedPairSolver::setTolerance(double tolerance)
{
    m_Tol = std::fabs(tolerance);
    // Both classes print an error below 1e-12; only Dormand-Prince also raises the tolerance.
    if (m_Tol < 1.0e-12 && m_ClampsTolerance)
    {
        m_Tol = 1.0e-12;
    }
    return true;
}

std::optional<double> EmbeddedPairSolver::tolerance() const noexcept
{
    return m_Tol;
}

int EmbeddedPairSolver::ospErrorCode() const noexcept
{
    return m_ErrorCode;
}

OdeConvergenceFailure EmbeddedPairSolver::convergenceFailure() const noexcept
{
    return m_Failure;
}

double EmbeddedPairSolver::stepOnce()
{
    return doStep();
}

double EmbeddedPairSolver::doStep()
{
    m_ErrorCode = OSP_NO_ERROR;
    m_Failure = OdeConvergenceFailure::None;
    int iterations = EMBEDDED_PAIR_ATTEMPTS;
    double currentStep = 0.0;
    double error = 0;
    const auto state = this->state();
    const auto& pair = *m_Pair;
    evaluateRate(state, m_K[0]);
    do
    {
        --iterations;
        currentStep = m_StepSize;
        for (std::size_t s = 1; s < m_K.size(); ++s)
        {
            for (std::size_t i = 0; i < m_NumEqn; ++i)
            {
                m_TempState[i] = state[i];
                for (std::size_t j = 0; j < s; ++j)
                {
                    m_TempState[i] = m_TempState[i] + m_StepSize * pair.a[s - 1][j] * m_K[j][i];
                }
            }
            evaluateRate(m_TempState, m_K[s]);
        }
        error = 0;
        for (std::size_t i = 0; i < m_NumEqn; ++i)
        {
            double truncErr = 0;
            for (std::size_t s = 0; s < m_K.size(); ++s)
            {
                truncErr = truncErr + m_StepSize * pair.er[s] * m_K[s][i];
            }
            error = javaMax(error, std::fabs(truncErr));
        }
        if (error <= FLOAT_MIN_VALUE)
        {
            // Too small to be meaningful: grow the step tenfold.
            error = m_Tol / 1.0e5;
        }
        if (error > m_Tol)
        {
            // Shrink, by no more than a factor of ten.
            const double fac = 0.9 * std::pow(error / m_Tol, -0.25);
            m_StepSize = m_StepSize * javaMax(fac, 0.1);
        }
        else if (error < m_Tol / 10.0)
        {
            // Grow, by no more than a factor of ten; fac is below 1 when error/tol is close to 1.
            const double fac = 0.9 * std::pow(error / m_Tol, -0.2);
            if (fac > 1)
            {
                m_StepSize = m_StepSize * javaMin(fac, 10);
            }
        }
    } while (error > m_Tol && iterations > 0);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        for (std::size_t s = 0; s < m_K.size(); ++s)
        {
            state[i] += currentStep * pair.b5[s] * m_K[s][i];
        }
    }
    if (iterations == 0)
    {
        m_ErrorCode = OSP_DID_NOT_CONVERGE;
        m_Failure = OdeConvergenceFailure::StepRejected;
    }
    return currentStep;
}

CashKarp45Solver::CashKarp45Solver(OdeRate rate, std::span<const double> initialState)
    : EmbeddedPairSolver(OdeSolverKind::CashKarp45, CASH_KARP, false, std::move(rate), initialState)
{}

DormandPrince45Solver::DormandPrince45Solver(OdeRate rate, std::span<const double> initialState, OdeSolverKind kind)
    : EmbeddedPairSolver(kind, DORMAND_PRINCE, true, std::move(rate), initialState)
{}

Rk45MultiStepSolver::Rk45MultiStepSolver(OdeRate rate, std::span<const double> initialState)
    : DormandPrince45Solver(std::move(rate), initialState, OdeSolverKind::Rk45MultiStep)
{}

void Rk45MultiStepSolver::construct()
{
    // The DormandPrince45 constructor calls the overridden initialize(0.01), so the engine starts
    // at 0.005; the RK45MultiStep field initializer then resets the fixed step to 0.1.
    initialize(m_StepSize);
    m_FixedStepSize = 0.1;
}

void Rk45MultiStepSolver::initialize(double stepSize)
{
    m_FixedStepSize = stepSize;
    // Be conservative with the adaptive solver.
    DormandPrince45Solver::initialize(stepSize / 2);
}

void Rk45MultiStepSolver::setStepSize(double stepSize)
{
    m_FixedStepSize = stepSize;
    if (stepSize < 0)
    {
        DormandPrince45Solver::setStepSize(javaMax(-std::fabs(DormandPrince45Solver::stepSize()), stepSize));
    }
    else
    {
        DormandPrince45Solver::setStepSize(javaMin(DormandPrince45Solver::stepSize(), stepSize));
    }
    // OSP then overwrites the adaptive step with the full interval.
    DormandPrince45Solver::setStepSize(stepSize);
}

double Rk45MultiStepSolver::stepSize() const noexcept
{
    return m_FixedStepSize;
}

OdeConvergenceFailure Rk45MultiStepSolver::convergenceFailure() const noexcept
{
    return m_MultistepFailure;
}

double Rk45MultiStepSolver::doStep()
{
    m_ErrorCode = OSP_NO_ERROR;
    m_MultistepFailure = OdeConvergenceFailure::None;
    if (m_FixedStepSize > 0)
    {
        return m_FixedStepSize - plus();
    }
    return m_FixedStepSize - minus();
}

bool Rk45MultiStepSolver::failed(double oldRemainder, double remainder, int counter, bool forward)
{
    const double engineStep = DormandPrince45Solver::stepSize();
    const double floor = m_Tol * m_FixedStepSize / 10.0;
    auto failure = OdeConvergenceFailure::None;
    if (m_ErrorCode != OSP_NO_ERROR)
    {
        failure = OdeConvergenceFailure::SubstepRejected;
    }
    else if (std::fabs(oldRemainder - remainder) <= FLOAT_MIN_VALUE)
    {
        failure = OdeConvergenceFailure::NoProgress;
    }
    else if (forward ? floor > engineStep : floor < engineStep)
    {
        failure = OdeConvergenceFailure::StepTooSmall;
    }
    else if (counter > MULTISTEP_MAX_ITERATIONS)
    {
        failure = OdeConvergenceFailure::TooManySubsteps;
    }
    if (failure == OdeConvergenceFailure::None)
    {
        return false;
    }
    m_ErrorCode = OSP_DID_NOT_CONVERGE;
    m_MultistepFailure = failure;
    return true;
}

double Rk45MultiStepSolver::plus()
{
    double remainder = m_FixedStepSize;
    const double engineStep = DormandPrince45Solver::stepSize();
    if (engineStep <= 0 || engineStep > m_FixedStepSize || m_FixedStepSize - engineStep == m_FixedStepSize)
    {
        // Reset a non-positive, oversized or negligible step and let it adapt.
        DormandPrince45Solver::setStepSize(m_FixedStepSize);
    }
    int counter = 0;
    while (remainder > m_Tol * m_FixedStepSize)
    {
        ++counter;
        const double oldRemainder = remainder;
        if (remainder < DormandPrince45Solver::stepSize())
        {
            // Temporarily shorten the step to hit the interval exactly.
            const double tempStep = DormandPrince45Solver::stepSize();
            DormandPrince45Solver::setStepSize(remainder);
            const double delta = DormandPrince45Solver::doStep();
            remainder -= delta;
            DormandPrince45Solver::setStepSize(tempStep);
        }
        else
        {
            remainder -= DormandPrince45Solver::doStep();
        }
        if (failed(oldRemainder, remainder, counter, true))
        {
            break;
        }
    }
    return remainder;
}

double Rk45MultiStepSolver::minus()
{
    double remainder = m_FixedStepSize;
    const double engineStep = DormandPrince45Solver::stepSize();
    if (engineStep >= 0 || engineStep < m_FixedStepSize || m_FixedStepSize - engineStep == m_FixedStepSize)
    {
        DormandPrince45Solver::setStepSize(m_FixedStepSize);
    }
    int counter = 0;
    while (remainder < m_Tol * m_FixedStepSize)
    {
        ++counter;
        const double oldRemainder = remainder;
        if (remainder > DormandPrince45Solver::stepSize())
        {
            const double tempStep = DormandPrince45Solver::stepSize();
            DormandPrince45Solver::setStepSize(remainder);
            const double delta = DormandPrince45Solver::doStep();
            remainder -= delta;
            DormandPrince45Solver::setStepSize(tempStep);
        }
        else
        {
            remainder -= DormandPrince45Solver::doStep();
        }
        if (failed(oldRemainder, remainder, counter, false))
        {
            break;
        }
    }
    return remainder;
}

MultistepSolver::MultistepSolver(OdeRate rate, std::span<const double> initialState)
    : OdeSolver(OdeSolverKind::Multistep, std::move(rate), initialState)
{
    // InternalODE: the engine advances its own copy of the state and calls the original rate.
    m_Engine = std::make_unique<DormandPrince45Solver>(
        [this](std::span<const double> values, std::span<double> rate) { evaluateRate(values, rate); }, initialState);
}

void MultistepSolver::construct()
{
    // `new DormandPrince45(internalODE)` initializes the engine with its 0.01 default; the
    // wrapper keeps its 0.1 fixed-step field and does not call initialize.
    m_Engine->initialize(m_Engine->stepSize());
}

void MultistepSolver::setInitialConditions()
{
    if (m_Engine->state().size() == state().size())
    {
        std::ranges::copy(state(), m_Engine->state().begin());
    }
    else
    {
        // OSP reallocates the engine state here without reinitializing the engine (and then fails
        // with an index error); MoTrack reinitializes it with its current step instead.
        m_Engine->setState(state());
    }
}

void MultistepSolver::initialize(double stepSize)
{
    m_ErrCode = OSP_NO_ERROR;
    m_Failure = OdeConvergenceFailure::None;
    m_FixedStepSize = stepSize;
    setInitialConditions();
    m_Engine->initialize(stepSize);
}

void MultistepSolver::setStepSize(double stepSize)
{
    m_FixedStepSize = stepSize;
    if (stepSize < 0)
    {
        m_Engine->setStepSize(javaMax(-std::fabs(m_Engine->stepSize()), stepSize));
    }
    else
    {
        m_Engine->setStepSize(javaMin(m_Engine->stepSize(), stepSize));
    }
}

double MultistepSolver::stepSize() const noexcept
{
    return m_FixedStepSize;
}

bool MultistepSolver::setTolerance(double tolerance)
{
    return m_Engine->setTolerance(std::fabs(tolerance));
}

std::optional<double> MultistepSolver::tolerance() const noexcept
{
    return m_Engine->tolerance();
}

int MultistepSolver::ospErrorCode() const noexcept
{
    return m_ErrCode;
}

OdeConvergenceFailure MultistepSolver::convergenceFailure() const noexcept
{
    return m_Failure;
}

double MultistepSolver::doStep()
{
    m_ErrCode = OSP_NO_ERROR;
    m_Failure = OdeConvergenceFailure::None;
    setInitialConditions();
    const double remainder = m_FixedStepSize > 0 ? plus() : minus();
    // InternalODE.update(): copy the engine state back to the ODE.
    std::ranges::copy(m_Engine->state(), state().begin());
    return m_FixedStepSize - remainder;
}

bool MultistepSolver::failed(double oldRemainder, double remainder, int counter, bool forward)
{
    const double floor = m_Engine->tolerance().value_or(0.0) * m_FixedStepSize / 10.0;
    const double engineStep = m_Engine->stepSize();
    auto failure = OdeConvergenceFailure::None;
    if (m_Engine->ospErrorCode() != OSP_NO_ERROR)
    {
        failure = OdeConvergenceFailure::SubstepRejected;
    }
    else if (std::fabs(oldRemainder - remainder) <= FLOAT_MIN_VALUE)
    {
        failure = OdeConvergenceFailure::NoProgress;
    }
    else if (forward ? floor > engineStep : floor < engineStep)
    {
        failure = OdeConvergenceFailure::StepTooSmall;
    }
    else if (counter > MULTISTEP_MAX_ITERATIONS)
    {
        failure = OdeConvergenceFailure::TooManySubsteps;
    }
    if (failure == OdeConvergenceFailure::None)
    {
        return false;
    }
    m_ErrCode = OSP_DID_NOT_CONVERGE;
    m_Failure = failure;
    return true;
}

double MultistepSolver::plus()
{
    const double tol = m_Engine->tolerance().value_or(0.0);
    double remainder = m_FixedStepSize;
    if (m_Engine->stepSize() <= 0 || m_Engine->stepSize() > m_FixedStepSize || m_FixedStepSize - m_Engine->stepSize() == m_FixedStepSize)
    {
        m_Engine->setStepSize(m_FixedStepSize);
    }
    int counter = 0;
    while (remainder > tol * m_FixedStepSize)
    {
        ++counter;
        const double oldRemainder = remainder;
        if (remainder < m_Engine->stepSize())
        {
            const double tempStep = m_Engine->stepSize();
            m_Engine->setStepSize(remainder);
            const double delta = m_Engine->stepOnce();
            remainder -= delta;
            m_Engine->setStepSize(tempStep);
        }
        else
        {
            remainder -= m_Engine->stepOnce();
        }
        if (failed(oldRemainder, remainder, counter, true))
        {
            break;
        }
    }
    return remainder;
}

double MultistepSolver::minus()
{
    const double tol = m_Engine->tolerance().value_or(0.0);
    double remainder = m_FixedStepSize;
    if (m_Engine->stepSize() >= 0 || m_Engine->stepSize() < m_FixedStepSize || m_FixedStepSize - m_Engine->stepSize() == m_FixedStepSize)
    {
        m_Engine->setStepSize(m_FixedStepSize);
    }
    int counter = 0;
    while (remainder < tol * m_FixedStepSize)
    {
        ++counter;
        const double oldRemainder = remainder;
        if (remainder > m_Engine->stepSize())
        {
            const double tempStep = m_Engine->stepSize();
            m_Engine->setStepSize(remainder);
            const double delta = m_Engine->stepOnce();
            remainder -= delta;
            m_Engine->setStepSize(tempStep);
        }
        else
        {
            remainder -= m_Engine->stepOnce();
        }
        // Deliberate fix: OSP's minus() omits this break (plus() and RK45MultiStep have it), so
        // after a failure it keeps stepping and can loop forever once the engine step underflows.
        if (failed(oldRemainder, remainder, counter, false))
        {
            break;
        }
    }
    return remainder;
}

} // namespace motrack::science::detail
