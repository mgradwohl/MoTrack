// Fixed-step OSP solvers. Each expression keeps the Java evaluation order (left to right, compound
// assignment as `a = a + (b)`) so that results match the reference bit for bit; the target is
// compiled with -ffp-contract=off because Java never fuses a multiply and an add.

#include "science/ode/ode_solvers_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace motrack::science::detail
{
namespace
{

std::vector<double> zeros(std::size_t count)
{
    return std::vector<double>(count, 0.0);
}

/// Velocity Verlet on interleaved {x1, v1, x2, v2, ..., t} (OSP `Verlet.step`). The second rate
/// sees the advanced positions with the old velocities and the old time.
template <typename Rate>
void verletStep(Rate&& evaluate, std::span<double> state, std::span<double> rate1, std::span<double> rate2, double stepSize)
{
    const std::size_t numEqn = state.size();
    evaluate(state, rate1);
    const double dt2 = stepSize * stepSize;
    for (std::size_t i = 0; i + 1 < numEqn; i += 2)
    {
        state[i] += stepSize * rate1[i] + dt2 * rate1[i + 1] / 2;
    }
    evaluate(state, rate2);
    for (std::size_t i = 1; i < numEqn; i += 2)
    {
        state[i] += stepSize * (rate1[i] + rate2[i]) / 2.0;
    }
    if (numEqn % 2 == 1)
    {
        state[numEqn - 1] += stepSize * rate1[numEqn - 1];
    }
}

} // namespace

void AbstractSolver::initialize(double stepSize)
{
    m_StepSize = stepSize;
    m_NumEqn = state().size();
}

void AbstractSolver::setStepSize(double stepSize)
{
    m_StepSize = stepSize;
}

double AbstractSolver::stepSize() const noexcept
{
    return m_StepSize;
}

void AbstractSolver::construct()
{
    initialize(0.1);
}

EulerSolver::EulerSolver(OdeRate rate, std::span<const double> initialState)
    : AbstractSolver(OdeSolverKind::Euler, std::move(rate), initialState)
{}

void EulerSolver::initialize(double stepSize)
{
    AbstractSolver::initialize(stepSize);
    m_Rate = zeros(m_NumEqn);
}

double EulerSolver::doStep()
{
    const auto state = this->state();
    evaluateRate(state, m_Rate);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        state[i] = state[i] + m_StepSize * m_Rate[i];
    }
    return m_StepSize;
}

EulerRichardsonSolver::EulerRichardsonSolver(OdeRate rate, std::span<const double> initialState)
    : AbstractSolver(OdeSolverKind::EulerRichardson, std::move(rate), initialState)
{}

void EulerRichardsonSolver::initialize(double stepSize)
{
    AbstractSolver::initialize(stepSize);
    m_Rate = zeros(m_NumEqn);
    m_MidState = zeros(m_NumEqn);
}

double EulerRichardsonSolver::doStep()
{
    const auto state = this->state();
    evaluateRate(state, m_Rate);
    const double dt2 = m_StepSize / 2;
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        m_MidState[i] = state[i] + m_Rate[i] * dt2;
    }
    evaluateRate(m_MidState, m_Rate);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        state[i] = state[i] + m_StepSize * m_Rate[i];
    }
    return m_StepSize;
}

Ralston2Solver::Ralston2Solver(OdeRate rate, std::span<const double> initialState)
    : AbstractSolver(OdeSolverKind::Ralston2, std::move(rate), initialState)
{}

void Ralston2Solver::initialize(double stepSize)
{
    AbstractSolver::initialize(stepSize);
    m_Rate1 = zeros(m_NumEqn);
    m_Rate2 = zeros(m_NumEqn);
    m_EstimatedState = zeros(m_NumEqn);
}

double Ralston2Solver::doStep()
{
    const auto state = this->state();
    evaluateRate(state, m_Rate1);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        m_EstimatedState[i] = state[i] + 3. / 4. * m_StepSize * m_Rate1[i];
    }
    evaluateRate(m_EstimatedState, m_Rate2);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        state[i] = state[i] + m_StepSize * (m_Rate1[i] + 2. * m_Rate2[i]) / 3.0;
    }
    return m_StepSize;
}

Heun3Solver::Heun3Solver(OdeRate rate, std::span<const double> initialState)
    : AbstractSolver(OdeSolverKind::Heun3, std::move(rate), initialState)
{}

void Heun3Solver::initialize(double stepSize)
{
    AbstractSolver::initialize(stepSize);
    m_Rate1 = zeros(m_NumEqn);
    m_Rate2 = zeros(m_NumEqn);
    m_Rate3 = zeros(m_NumEqn);
    m_EstimatedState = zeros(m_NumEqn);
}

double Heun3Solver::doStep()
{
    const auto state = this->state();
    evaluateRate(state, m_Rate1);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        m_EstimatedState[i] = state[i] + m_StepSize * m_Rate1[i] / 3;
    }
    evaluateRate(m_EstimatedState, m_Rate2);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        m_EstimatedState[i] = state[i] + 2 * m_StepSize * m_Rate2[i] / 3;
    }
    evaluateRate(m_EstimatedState, m_Rate3);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        state[i] = state[i] + m_StepSize * (m_Rate1[i] + 3.0 * m_Rate3[i]) / 4.0;
    }
    return m_StepSize;
}

Rk4Solver::Rk4Solver(OdeRate rate, std::span<const double> initialState, OdeSolverKind kind)
    : AbstractSolver(kind, std::move(rate), initialState)
{}

void Rk4Solver::initialize(double stepSize)
{
    AbstractSolver::initialize(stepSize);
    m_Rate1 = zeros(m_NumEqn);
    m_Rate2 = zeros(m_NumEqn);
    m_Rate3 = zeros(m_NumEqn);
    m_Rate4 = zeros(m_NumEqn);
    m_EstimatedState = zeros(m_NumEqn);
}

double Rk4Solver::doStep()
{
    const auto state = this->state();
    evaluateRate(state, m_Rate1);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        m_EstimatedState[i] = state[i] + m_StepSize * m_Rate1[i] / 2;
    }
    evaluateRate(m_EstimatedState, m_Rate2);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        m_EstimatedState[i] = state[i] + m_StepSize * m_Rate2[i] / 2;
    }
    evaluateRate(m_EstimatedState, m_Rate3);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        m_EstimatedState[i] = state[i] + m_StepSize * m_Rate3[i];
    }
    evaluateRate(m_EstimatedState, m_Rate4);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        state[i] = state[i] + m_StepSize * (m_Rate1[i] + 2 * m_Rate2[i] + 2 * m_Rate3[i] + m_Rate4[i]) / 6.0;
    }
    return m_StepSize;
}

Butcher5Solver::Butcher5Solver(OdeRate rate, std::span<const double> initialState, OdeSolverKind kind)
    : AbstractSolver(kind, std::move(rate), initialState)
{}

void Butcher5Solver::initialize(double stepSize)
{
    AbstractSolver::initialize(stepSize);
    for (auto& rate : m_Rates)
    {
        rate = zeros(m_NumEqn);
    }
    m_EstimatedState = zeros(m_NumEqn);
}

double Butcher5Solver::doStep()
{
    const auto state = this->state();
    const double h = m_StepSize;
    auto& [r1, r2, r3, r4, r5, r6] = m_Rates;
    auto& estimated = m_EstimatedState;
    evaluateRate(state, r1);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        estimated[i] = state[i] + h * r1[i] / 4;
    }
    evaluateRate(estimated, r2);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        estimated[i] = state[i] + h * (r1[i] + r2[i]) / 8;
    }
    evaluateRate(estimated, r3);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        estimated[i] = state[i] + h * (-0.5 * r2[i] + r3[i]);
    }
    evaluateRate(estimated, r4);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        estimated[i] = state[i] + h * (3 * r1[i] + 9 * r4[i]) / 16;
    }
    evaluateRate(estimated, r5);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        estimated[i] = state[i] + h * (-3 * r1[i] + 2 * r2[i] + 12 * r3[i] - 12 * r4[i] + 8 * r5[i]) / 7;
    }
    evaluateRate(estimated, r6);
    for (std::size_t i = 0; i < m_NumEqn; ++i)
    {
        state[i] = state[i] + h * (7 * r1[i] + 32 * r3[i] + 12 * r4[i] + 32 * r5[i] + 7 * r6[i]) / 90.0;
    }
    return m_StepSize;
}

Fehlberg8Solver::Fehlberg8Solver(OdeRate rate, std::span<const double> initialState, OdeSolverKind kind)
    : AbstractSolver(kind, std::move(rate), initialState)
{}

void Fehlberg8Solver::initialize(double stepSize)
{
    AbstractSolver::initialize(stepSize);
    for (auto& rate : m_Rates)
    {
        rate = zeros(m_NumEqn);
    }
    m_EstimatedState = zeros(m_NumEqn);
}

double Fehlberg8Solver::doStep()
{
    // OSP's Fehlberg8 integrates with the eighth-order weights of the 7(8) pair and ignores the
    // embedded error estimate, so it is a fixed-step method.
    const auto state = this->state();
    const double h = m_StepSize;
    auto& [r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13] = m_Rates;
    auto& estimated = m_EstimatedState;
    const std::size_t n = m_NumEqn;
    evaluateRate(state, r1);
    for (std::size_t i = 0; i < n; ++i)
    {
        estimated[i] = state[i] + h * 2. / 27. * r1[i];
    }
    evaluateRate(estimated, r2);
    for (std::size_t i = 0; i < n; ++i)
    {
        estimated[i] = state[i] + h * (r1[i] + 3 * r2[i]) / 36;
    }
    evaluateRate(estimated, r3);
    for (std::size_t i = 0; i < n; ++i)
    {
        estimated[i] = state[i] + h * (r1[i] + 3 * r3[i]) / 24;
    }
    evaluateRate(estimated, r4);
    for (std::size_t i = 0; i < n; ++i)
    {
        estimated[i] = state[i] + h * (5. / 12. * r1[i] - 25. / 16. * r3[i] + 25. / 16. * r4[i]);
    }
    evaluateRate(estimated, r5);
    for (std::size_t i = 0; i < n; ++i)
    {
        estimated[i] = state[i] + h * (r1[i] + 5 * r4[i] + 4 * r5[i]) / 20;
    }
    evaluateRate(estimated, r6);
    for (std::size_t i = 0; i < n; ++i)
    {
        estimated[i] = state[i] + h * (-25 * r1[i] + 125 * r4[i] - 260 * r5[i] + 250 * r6[i]) / 108;
    }
    evaluateRate(estimated, r7);
    for (std::size_t i = 0; i < n; ++i)
    {
        estimated[i] = state[i] + h * (31. / 300. * r1[i] + 61. / 225. * r5[i] - 2. / 9. * r6[i] + 13. / 900. * r7[i]);
    }
    evaluateRate(estimated, r8);
    for (std::size_t i = 0; i < n; ++i)
    {
        estimated[i] =
            state[i] + h * (2 * r1[i] - 53. / 6. * r4[i] + 704. / 45. * r5[i] - 107. / 9. * r6[i] + 67. / 90. * r7[i] + 3 * r8[i]);
    }
    evaluateRate(estimated, r9);
    for (std::size_t i = 0; i < n; ++i)
    {
        estimated[i] = state[i] + h * (-91. / 108. * r1[i] + 23. / 108. * r4[i] - 976. / 135. * r5[i] + 311. / 54. * r6[i] -
                                       19. / 60. * r7[i] + 17. / 6. * r8[i] - r9[i] / 12.);
    }
    evaluateRate(estimated, r10);
    for (std::size_t i = 0; i < n; ++i)
    {
        estimated[i] = state[i] + h * (2383. / 4100. * r1[i] - 341. / 164. * r4[i] + 4496. / 1025. * r5[i] - 301. / 82. * r6[i] +
                                       2133. / 4100. * r7[i] + 45. / 82. * r8[i] + 45. / 164. * r9[i] + 18. / 41. * r10[i]);
    }
    evaluateRate(estimated, r11);
    for (std::size_t i = 0; i < n; ++i)
    {
        estimated[i] = state[i] + h * (3. / 205. * r1[i] - 6. / 41. * r6[i] - 3. / 205. * r7[i] - 3. / 41. * r8[i] + 3. / 41. * r9[i] +
                                       6. / 41. * r10[i]);
    }
    evaluateRate(estimated, r12);
    for (std::size_t i = 0; i < n; ++i)
    {
        estimated[i] = state[i] + h * (-1777. / 4100. * r1[i] - 341. / 164. * r4[i] + 4496. / 1025. * r5[i] - 289. / 82. * r6[i] +
                                       2193. / 4100. * r7[i] + 51. / 82. * r8[i] + 33. / 164. * r9[i] + 12. / 41. * r10[i] + r12[i]);
    }
    evaluateRate(estimated, r13);
    for (std::size_t i = 0; i < n; ++i)
    {
        state[i] = state[i] + h * (34. / 105. * r6[i] + 9. / 35. * r7[i] + 9. / 35. * r8[i] + 9. / 280. * r9[i] + 9. / 280. * r10[i] +
                                   41. / 840. * r12[i] + 41. / 840. * r13[i]);
    }
    return m_StepSize;
}

namespace
{

/// Shifts the Adams rate history `fn -> fn1 -> ... -> fn(k-1)`, dropping the oldest.
template <std::size_t Count> void shiftHistory(std::array<std::vector<double>, Count>& history)
{
    for (std::size_t index = Count - 1; index > 0; --index)
    {
        std::ranges::copy(history.at(index - 1), history.at(index).begin());
    }
}

} // namespace

Adams4Solver::Adams4Solver(OdeRate rate, std::span<const double> initialState)
    : Rk4Solver(std::move(rate), initialState, OdeSolverKind::Adams4)
{}

void Adams4Solver::initialize(double stepSize)
{
    Rk4Solver::initialize(stepSize);
    for (auto& rate : m_Fn)
    {
        rate = zeros(m_NumEqn);
    }
    m_TempState = zeros(m_NumEqn);
    m_TempRate = zeros(m_NumEqn);
    m_Counter = 0;
}

double Adams4Solver::doStep()
{
    const auto state = this->state();
    auto& [fn, fn1, fn2, fn3] = m_Fn;
    evaluateRate(state, fn);
    if (m_Counter < 3)
    {
        // Runge-Kutta 4 starts the method.
        m_StepSize = Rk4Solver::doStep();
        ++m_Counter;
    }
    else
    {
        const double h = m_StepSize;
        for (std::size_t i = 0; i < m_NumEqn; ++i)
        {
            m_TempState[i] = state[i] + h * (55 * fn[i] - 59 * fn1[i] + 37 * fn2[i] - 9 * fn3[i]) / 24;
        }
        evaluateRate(m_TempState, m_TempRate);
        for (std::size_t i = 0; i < m_NumEqn; ++i)
        {
            state[i] = state[i] + h * (9 * m_TempRate[i] + 19 * fn[i] - 5 * fn1[i] + fn2[i]) / 24;
        }
    }
    shiftHistory(m_Fn);
    return m_StepSize;
}

Adams5Solver::Adams5Solver(OdeRate rate, std::span<const double> initialState)
    : Butcher5Solver(std::move(rate), initialState, OdeSolverKind::Adams5)
{}

void Adams5Solver::initialize(double stepSize)
{
    Butcher5Solver::initialize(stepSize);
    for (auto& rate : m_Fn)
    {
        rate = zeros(m_NumEqn);
    }
    m_TempState = zeros(m_NumEqn);
    m_TempRate = zeros(m_NumEqn);
    m_Counter = 0;
}

double Adams5Solver::doStep()
{
    const auto state = this->state();
    auto& [fn, fn1, fn2, fn3, fn4] = m_Fn;
    evaluateRate(state, fn);
    if (m_Counter < 4)
    {
        // Butcher5 starts the method.
        m_StepSize = Butcher5Solver::doStep();
        ++m_Counter;
    }
    else
    {
        const double h = m_StepSize;
        for (std::size_t i = 0; i < m_NumEqn; ++i)
        {
            m_TempState[i] = state[i] + h * (1901 * fn[i] - 2774 * fn1[i] + 2616 * fn2[i] - 1274 * fn3[i] + 251 * fn4[i]) / 720;
        }
        evaluateRate(m_TempState, m_TempRate);
        for (std::size_t i = 0; i < m_NumEqn; ++i)
        {
            state[i] = state[i] + h * (251 * m_TempRate[i] + 646 * fn[i] - 264 * fn1[i] + 106 * fn2[i] - 19 * fn3[i]) / 720;
        }
    }
    shiftHistory(m_Fn);
    return m_StepSize;
}

Adams6Solver::Adams6Solver(OdeRate rate, std::span<const double> initialState)
    : Fehlberg8Solver(std::move(rate), initialState, OdeSolverKind::Adams6)
{}

void Adams6Solver::initialize(double stepSize)
{
    Fehlberg8Solver::initialize(stepSize);
    for (auto& rate : m_Fn)
    {
        rate = zeros(m_NumEqn);
    }
    m_TempState = zeros(m_NumEqn);
    m_TempRate = zeros(m_NumEqn);
    m_Counter = 0;
}

double Adams6Solver::doStep()
{
    const auto state = this->state();
    auto& [fn, fn1, fn2, fn3, fn4, fn5] = m_Fn;
    evaluateRate(state, fn);
    if (m_Counter < 5)
    {
        // Fehlberg8 starts the method.
        m_StepSize = Fehlberg8Solver::doStep();
        ++m_Counter;
    }
    else
    {
        const double h = m_StepSize;
        for (std::size_t i = 0; i < m_NumEqn; ++i)
        {
            // Deliberate fix: the six-step Adams-Bashforth weights sum to 1440. OSP 6.3.5 divides by
            // 720, which doubles the predicted increment and reduces the method to first order.
            m_TempState[i] =
                state[i] + h * (4277 * fn[i] - 7923 * fn1[i] + 9982 * fn2[i] - 7298 * fn3[i] + 2877 * fn4[i] - 475 * fn5[i]) / 1440;
        }
        evaluateRate(m_TempState, m_TempRate);
        for (std::size_t i = 0; i < m_NumEqn; ++i)
        {
            state[i] =
                state[i] + h * (475 * m_TempRate[i] + 1427 * fn[i] - 798 * fn1[i] + 482 * fn2[i] - 173 * fn3[i] + 27 * fn4[i]) / 1440;
        }
    }
    shiftHistory(m_Fn);
    return m_StepSize;
}

VerletSolver::VerletSolver(OdeRate rate, std::span<const double> initialState)
    : AbstractSolver(OdeSolverKind::Verlet, std::move(rate), initialState)
{}

void VerletSolver::initialize(double stepSize)
{
    AbstractSolver::initialize(stepSize);
    m_Rate1 = zeros(m_NumEqn);
    m_Rate2 = zeros(m_NumEqn);
}

double VerletSolver::doStep()
{
    verletStep([this](std::span<const double> values, std::span<double> rate) { evaluateRate(values, rate); },
               state(),
               m_Rate1,
               m_Rate2,
               m_StepSize);
    return m_StepSize;
}

LeapFrogSolver::LeapFrogSolver(OdeRate rate, std::span<const double> initialState)
    : AbstractSolver(OdeSolverKind::LeapFrog, std::move(rate), initialState)
{}

void LeapFrogSolver::initialize(double stepSize)
{
    AbstractSolver::initialize(stepSize);
    m_Rate = zeros(m_NumEqn);
    m_PriorState = zeros(m_NumEqn);
    m_CurrentState = zeros(m_NumEqn);
    estimatePreviousState();
}

void LeapFrogSolver::setStepSize(double stepSize)
{
    initialize(stepSize);
}

void LeapFrogSolver::estimatePreviousState()
{
    // OSP steps the live state back with a new Verlet solver (fresh zeroed rate arrays), copies it
    // to priorState and restores the state. Stepping a copy gives the same values.
    std::ranges::copy(state(), m_PriorState.begin());
    auto rate1 = zeros(m_NumEqn);
    auto rate2 = zeros(m_NumEqn);
    verletStep([this](std::span<const double> values, std::span<double> rate) { evaluateRate(values, rate); },
               m_PriorState,
               rate1,
               rate2,
               -m_StepSize);
}

double LeapFrogSolver::doStep()
{
    const auto state = this->state();
    std::ranges::copy(state, m_CurrentState.begin());
    evaluateRate(state, m_Rate);
    const double dtSquared = m_StepSize * m_StepSize;
    const double dt2 = 2 * m_StepSize;
    for (std::size_t i = 0; i + 1 < m_NumEqn; i += 2)
    {
        // "+= state[i]" is correct: the leapfrog position update uses 2 * state[i].
        state[i] += state[i] - m_PriorState[i] + dtSquared * m_Rate[i + 1];
        state[i + 1] = (state[i] - m_PriorState[i]) / dt2 + m_Rate[i + 1] * m_StepSize;
    }
    if (m_NumEqn % 2 == 1)
    {
        state[m_NumEqn - 1] += m_StepSize * m_Rate[m_NumEqn - 1];
    }
    std::ranges::copy(m_CurrentState, m_PriorState.begin());
    return m_StepSize;
}

} // namespace motrack::science::detail
