#include "science/ode/ode_solver.hpp"

#include "science/ode/ode_solvers_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace motrack::science
{
namespace
{

struct SolverName
{
    OdeSolverKind kind;
    std::string_view className;
    std::string_view factoryName; // empty when ODESolverFactory cannot create the class
    bool adaptive;
};

// ODESolverFactory.createODESolver names and the classes Tracker can load by name.
constexpr std::array SOLVER_NAMES = {
    SolverName{
        .kind = OdeSolverKind::Euler, .className = "org.opensourcephysics.numerics.Euler", .factoryName = "euler", .adaptive = false},
    SolverName{.kind = OdeSolverKind::EulerRichardson,
               .className = "org.opensourcephysics.numerics.EulerRichardson",
               .factoryName = "eulerrichardson",
               .adaptive = false},
    SolverName{.kind = OdeSolverKind::Ralston2,
               .className = "org.opensourcephysics.numerics.Ralston2",
               .factoryName = "ralston2",
               .adaptive = false},
    SolverName{
        .kind = OdeSolverKind::Heun3, .className = "org.opensourcephysics.numerics.Heun3", .factoryName = "heun3", .adaptive = false},
    SolverName{.kind = OdeSolverKind::Rk4, .className = "org.opensourcephysics.numerics.RK4", .factoryName = "rk4", .adaptive = false},
    SolverName{.kind = OdeSolverKind::Butcher5,
               .className = "org.opensourcephysics.numerics.Butcher5",
               .factoryName = "butcher5",
               .adaptive = false},
    SolverName{.kind = OdeSolverKind::Fehlberg8,
               .className = "org.opensourcephysics.numerics.Fehlberg8",
               .factoryName = "fehlberg8",
               .adaptive = false},
    SolverName{
        .kind = OdeSolverKind::Adams4, .className = "org.opensourcephysics.numerics.Adams4", .factoryName = "adams4", .adaptive = false},
    SolverName{
        .kind = OdeSolverKind::Adams5, .className = "org.opensourcephysics.numerics.Adams5", .factoryName = "adams5", .adaptive = false},
    SolverName{
        .kind = OdeSolverKind::Adams6, .className = "org.opensourcephysics.numerics.Adams6", .factoryName = "adams6", .adaptive = false},
    SolverName{
        .kind = OdeSolverKind::Verlet, .className = "org.opensourcephysics.numerics.Verlet", .factoryName = "verlet", .adaptive = false},
    SolverName{
        .kind = OdeSolverKind::LeapFrog, .className = "org.opensourcephysics.numerics.LeapFrog", .factoryName = "", .adaptive = false},
    SolverName{.kind = OdeSolverKind::CashKarp45,
               .className = "org.opensourcephysics.numerics.CashKarp45",
               .factoryName = "cashkarp45",
               .adaptive = true},
    SolverName{.kind = OdeSolverKind::DormandPrince45,
               .className = "org.opensourcephysics.numerics.DormandPrince45",
               .factoryName = "dormandprince45",
               .adaptive = true},
    SolverName{.kind = OdeSolverKind::Rk45, .className = "org.opensourcephysics.numerics.RK45", .factoryName = "", .adaptive = true},
    SolverName{.kind = OdeSolverKind::Rk45MultiStep,
               .className = "org.opensourcephysics.numerics.RK45MultiStep",
               .factoryName = "",
               .adaptive = true},
    SolverName{.kind = OdeSolverKind::Multistep,
               .className = "org.opensourcephysics.numerics.ODEMultistepSolver",
               .factoryName = "multistep",
               .adaptive = true},
};
/// solverName() indexes the table by enumerator value, so entry i must describe enumerator i.
consteval bool namesFollowEnumeratorOrder()
{
    if (SOLVER_NAMES.size() != ALL_ODE_SOLVER_KINDS.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < SOLVER_NAMES.size(); ++index)
    {
        const auto kind = SOLVER_NAMES.at(index).kind;
        if (kind != ALL_ODE_SOLVER_KINDS.at(index) || static_cast<std::size_t>(kind) != index)
        {
            return false;
        }
    }
    return true;
}
static_assert(namesFollowEnumeratorOrder(), "The name table must list every solver kind in enumerator order");

/// The table entry, or null for a value outside the enumeration (for example a corrupt cast).
const SolverName* solverName(OdeSolverKind kind) noexcept
{
    const auto index = static_cast<std::size_t>(kind);
    return index < SOLVER_NAMES.size() ? &SOLVER_NAMES[index] : nullptr;
}

bool isJavaWhitespace(char character) noexcept
{
    // String.trim() removes every character up to U+0020.
    return static_cast<unsigned char>(character) <= 0x20;
}

char asciiLower(char character) noexcept
{
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
}

bool equalsLowercase(std::string_view text, std::string_view lowercase) noexcept
{
    return text.size() == lowercase.size() &&
           std::ranges::equal(text, lowercase, [](char left, char right) { return asciiLower(left) == right; });
}

OdeStepStatus worse(OdeStepStatus left, OdeStepStatus right) noexcept
{
    return static_cast<std::uint8_t>(left) >= static_cast<std::uint8_t>(right) ? left : right;
}

void record(OdeAdvanceResult& result, OdeStepStatus status) noexcept
{
    if (status != OdeStepStatus::Ok && !result.firstFailure)
    {
        result.firstFailure = result.steps;
    }
    result.status = worse(result.status, status);
    ++result.steps;
}

} // namespace

std::string_view odeSolverClassName(OdeSolverKind kind) noexcept
{
    const auto* entry = solverName(kind);
    return entry == nullptr ? std::string_view{} : entry->className;
}

std::optional<std::string_view> odeSolverFactoryName(OdeSolverKind kind) noexcept
{
    const auto* entry = solverName(kind);
    if (entry == nullptr || entry->factoryName.empty())
    {
        return std::nullopt;
    }
    return entry->factoryName;
}

std::optional<OdeSolverKind> odeSolverFromFactoryName(std::string_view name) noexcept
{
    while (!name.empty() && isJavaWhitespace(name.front()))
    {
        name.remove_prefix(1);
    }
    while (!name.empty() && isJavaWhitespace(name.back()))
    {
        name.remove_suffix(1);
    }
    const auto match = std::ranges::find_if(
        SOLVER_NAMES, [name](const SolverName& entry) { return !entry.factoryName.empty() && equalsLowercase(name, entry.factoryName); });
    return match == SOLVER_NAMES.end() ? std::nullopt : std::optional{match->kind};
}

std::optional<OdeSolverKind> odeSolverFromClassName(std::string_view className) noexcept
{
    const auto match = std::ranges::find(SOLVER_NAMES, className, &SolverName::className);
    return match == SOLVER_NAMES.end() ? std::nullopt : std::optional{match->kind};
}

bool isAdaptiveOdeSolver(OdeSolverKind kind) noexcept
{
    const auto* entry = solverName(kind);
    return entry != nullptr && entry->adaptive;
}

OdeSolver::OdeSolver(OdeSolverKind kind, OdeRate rate, std::span<const double> initialState)
    : m_Kind(kind), m_Rate(std::move(rate)), m_State(initialState.begin(), initialState.end())
{}

OdeSolver::~OdeSolver() = default;

OdeSolverKind OdeSolver::kind() const noexcept
{
    return m_Kind;
}

std::span<double> OdeSolver::state() noexcept
{
    return m_State;
}

std::span<const double> OdeSolver::state() const noexcept
{
    return m_State;
}

void OdeSolver::setState(std::span<const double> values)
{
    const bool resized = values.size() != m_State.size();
    assignState(values);
    if (resized)
    {
        initialize(stepSize());
    }
}

void OdeSolver::assignState(std::span<const double> values)
{
    m_State.assign(values.begin(), values.end());
}

OdeStepResult OdeSolver::step()
{
    const double taken = doStep();
    auto status = OdeStepStatus::Ok;
    if (!std::isfinite(taken) || !std::ranges::all_of(m_State, [](double value) { return std::isfinite(value); }))
    {
        status = OdeStepStatus::NonFinite;
    }
    else if (ospErrorCode() != OSP_NO_ERROR)
    {
        status = OdeStepStatus::DidNotConverge;
    }
    return {.stepTaken = taken, .status = status};
}

OdeAdvanceResult OdeSolver::advance(std::size_t count)
{
    OdeAdvanceResult result;
    for (std::size_t index = 0; index < count; ++index)
    {
        record(result, step().status);
    }
    return result;
}

OdeAdvanceResult OdeSolver::advanceTo(double targetTime, std::size_t maxSteps)
{
    OdeAdvanceResult result;
    if (m_State.empty() || !std::isfinite(targetTime))
    {
        result.reachedTarget = false;
        return result;
    }
    const auto remaining = [this, targetTime]
    {
        return targetTime - m_State.back();
    };
    while (true)
    {
        const double nominal = stepSize();
        const double before = remaining();
        // Positive when the target lies ahead in the stepping direction; false for NaN.
        const double ahead = std::signbit(nominal) ? -before : before;
        if (!(ahead > 0.5 * std::fabs(nominal)))
        {
            result.reachedTarget = std::fabs(before) <= 0.5 * std::fabs(nominal);
            return result;
        }
        if (result.steps == maxSteps)
        {
            result.reachedTarget = false;
            return result;
        }
        record(result, step().status);
        if (!(std::fabs(remaining()) < std::fabs(before)))
        {
            // The step did not move time toward the target: a zero, NaN or reversed step, or a
            // state whose last component is not the independent variable.
            result.reachedTarget = std::fabs(remaining()) <= 0.5 * std::fabs(stepSize());
            return result;
        }
    }
}

bool OdeSolver::setTolerance(double /*tolerance*/)
{
    return false;
}

std::optional<double> OdeSolver::tolerance() const noexcept
{
    return std::nullopt;
}

int OdeSolver::ospErrorCode() const noexcept
{
    return OSP_NO_ERROR;
}

OdeConvergenceFailure OdeSolver::convergenceFailure() const noexcept
{
    return OdeConvergenceFailure::None;
}

std::uint64_t OdeSolver::rateEvaluations() const noexcept
{
    return m_RateEvaluations;
}

void OdeSolver::evaluateRate(std::span<const double> state, std::span<double> rate)
{
    ++m_RateEvaluations;
    m_Rate(state, rate);
}

std::unique_ptr<OdeSolver> makeOdeSolver(OdeSolverKind kind, OdeRate rate, std::span<const double> initialState)
{
    if (!rate)
    {
        return nullptr;
    }
    std::unique_ptr<OdeSolver> solver;
    switch (kind)
    {
    case OdeSolverKind::Euler:
        solver = std::make_unique<detail::EulerSolver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::EulerRichardson:
        solver = std::make_unique<detail::EulerRichardsonSolver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::Ralston2:
        solver = std::make_unique<detail::Ralston2Solver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::Heun3:
        solver = std::make_unique<detail::Heun3Solver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::Rk4:
        solver = std::make_unique<detail::Rk4Solver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::Butcher5:
        solver = std::make_unique<detail::Butcher5Solver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::Fehlberg8:
        solver = std::make_unique<detail::Fehlberg8Solver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::Adams4:
        solver = std::make_unique<detail::Adams4Solver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::Adams5:
        solver = std::make_unique<detail::Adams5Solver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::Adams6:
        solver = std::make_unique<detail::Adams6Solver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::Verlet:
        solver = std::make_unique<detail::VerletSolver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::LeapFrog:
        solver = std::make_unique<detail::LeapFrogSolver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::CashKarp45:
        solver = std::make_unique<detail::CashKarp45Solver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::DormandPrince45:
    case OdeSolverKind::Rk45:
        solver = std::make_unique<detail::DormandPrince45Solver>(std::move(rate), initialState, kind);
        break;
    case OdeSolverKind::Rk45MultiStep:
        solver = std::make_unique<detail::Rk45MultiStepSolver>(std::move(rate), initialState);
        break;
    case OdeSolverKind::Multistep:
        solver = std::make_unique<detail::MultistepSolver>(std::move(rate), initialState);
        break;
    }
    if (solver)
    {
        solver->construct();
    }
    return solver;
}

std::unique_ptr<OdeSolver> createOdeSolver(std::string_view factoryName, OdeRate rate, std::span<const double> initialState)
{
    const auto kind = odeSolverFromFactoryName(factoryName);
    return kind ? makeOdeSolver(*kind, std::move(rate), initialState) : nullptr;
}

} // namespace motrack::science
