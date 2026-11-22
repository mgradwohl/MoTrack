#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace motrack::science
{

/// OSP ODE solver classes that Tracker 6.3.5 can instantiate for a dynamic particle model.
///
/// Every kind ports the corresponding `org.opensourcephysics.numerics` class at OSP commit
/// d56207b0 (the source of the Tracker 6.3.5 JAR). See docs/ode-solvers.md for the deliberate
/// differences: the Adams6 predictor uses the correct 1/1440 weights, and a backward
/// `ODEMultistepSolver` step stops at its first convergence failure instead of looping.
enum class OdeSolverKind : std::uint8_t
{
    Euler,
    EulerRichardson,
    Ralston2,
    Heun3,
    Rk4,
    Butcher5,
    Fehlberg8,
    Adams4,
    Adams5,
    Adams6,
    Verlet,
    LeapFrog,
    CashKarp45,
    DormandPrince45,
    Rk45,
    Rk45MultiStep,
    Multistep,
};

inline constexpr std::array ALL_ODE_SOLVER_KINDS = {
    OdeSolverKind::Euler,
    OdeSolverKind::EulerRichardson,
    OdeSolverKind::Ralston2,
    OdeSolverKind::Heun3,
    OdeSolverKind::Rk4,
    OdeSolverKind::Butcher5,
    OdeSolverKind::Fehlberg8,
    OdeSolverKind::Adams4,
    OdeSolverKind::Adams5,
    OdeSolverKind::Adams6,
    OdeSolverKind::Verlet,
    OdeSolverKind::LeapFrog,
    OdeSolverKind::CashKarp45,
    OdeSolverKind::DormandPrince45,
    OdeSolverKind::Rk45,
    OdeSolverKind::Rk45MultiStep,
    OdeSolverKind::Multistep,
};

/// `DynamicParticle` constructs `new RK4(this)`; Tracker 6.3.5 neither saves nor offers another solver.
inline constexpr OdeSolverKind TRACKER_DEFAULT_ODE_SOLVER = OdeSolverKind::Rk4;

/// Solvers listed by Tracker's (hidden) Model Builder solver dropdown, in its order.
inline constexpr std::array TRACKER_MODEL_BUILDER_SOLVERS = {OdeSolverKind::Rk4, OdeSolverKind::Euler, OdeSolverKind::Ralston2};

/// `ODEAdaptiveSolver` error codes, as returned by `ospErrorCode()`.
inline constexpr int OSP_NO_ERROR = 0;
inline constexpr int OSP_DID_NOT_CONVERGE = 1;

/// Fully qualified Java class name, e.g. `org.opensourcephysics.numerics.RK4` (empty for a value
/// outside the enumeration; the name and flag lookups below never throw).
[[nodiscard]] std::string_view odeSolverClassName(OdeSolverKind kind) noexcept;

/// `ODESolverFactory.createODESolver` name, or nothing for classes the factory does not create
/// (RK45, RK45MultiStep and LeapFrog are reachable only through Tracker's class-name loader).
[[nodiscard]] std::optional<std::string_view> odeSolverFactoryName(OdeSolverKind kind) noexcept;

/// Resolves a factory name like OSP: surrounding whitespace is ignored and ASCII letters are
/// compared case-insensitively. Unknown names return nothing (the factory returns null).
[[nodiscard]] std::optional<OdeSolverKind> odeSolverFromFactoryName(std::string_view name) noexcept;

/// Resolves the exact class name used by Tracker's legacy `solver` property (`Class.forName`).
[[nodiscard]] std::optional<OdeSolverKind> odeSolverFromClassName(std::string_view className) noexcept;

/// Whether the class implements `ODEAdaptiveSolver` (tolerance and error code).
[[nodiscard]] bool isAdaptiveOdeSolver(OdeSolverKind kind) noexcept;

/// Computes the rate of change `rate` (same length) of the state vector `state`.
///
/// Tracker states end with the independent variable, whose rate is 1, for example
/// `{x, vx, y, vy, t}`. The callback runs synchronously on the stepping thread, must write every
/// rate it needs (unwritten entries keep the solver's previous scratch values, as in OSP) and
/// should report an undefined result with NaN rather than throwing. An exception thrown by the
/// callback propagates out of `step()` and leaves the state partially advanced.
using OdeRate = std::function<void(std::span<const double> state, std::span<double> rate)>;

/// Outcome of one `step()`. The solver itself never throws while stepping.
enum class OdeStepStatus : std::uint8_t
{
    Ok,
    /// The OSP error code is `DID_NOT_CONVERGE`; the state was still advanced as in OSP.
    DidNotConverge,
    /// The state or the step taken contains NaN or infinity (for example an unstable step size or
    /// a NaN rate). OSP does not detect this; MoTrack reports it without changing the numbers.
    NonFinite,
};

/// Why an adaptive solver last reported `DID_NOT_CONVERGE` (MoTrack diagnostic detail).
enum class OdeConvergenceFailure : std::uint8_t
{
    None,
    /// An embedded 4(5) pair spent its ten attempts. OSP also reports this when the tenth attempt
    /// was accepted, and advances the state with the last attempted step either way.
    StepRejected,
    /// A multistep wrapper's adaptive substep reported `StepRejected`.
    SubstepRejected,
    /// A multistep substep left the remaining interval unchanged (round-off).
    NoProgress,
    /// The adaptive substep fell below `tolerance * fixed step / 10`, which usually means stiffness.
    StepTooSmall,
    /// A multistep step needed more than 200 substeps, which usually means stiffness.
    TooManySubsteps,
};

struct OdeStepResult
{
    /// The value OSP's `step()` returns: the interval actually advanced.
    double stepTaken = 0.0;
    OdeStepStatus status = OdeStepStatus::Ok;
};

struct OdeAdvanceResult
{
    std::size_t steps = 0;
    /// Most severe status seen (`NonFinite` over `DidNotConverge` over `Ok`).
    OdeStepStatus status = OdeStepStatus::Ok;
    /// Zero-based index of the first step whose status was not `Ok`.
    std::optional<std::size_t> firstFailure;
    /// For `advanceTo`: whether the time component ended within half a step of the target.
    bool reachedTarget = true;
};

/// A single-threaded OSP ODE solver that owns the state vector it advances.
///
/// The object has no shared or global state; separate instances may run on different threads.
/// Results are deterministic for a deterministic rate callback. The library is compiled without
/// floating-point contraction so that results match Java's strict IEEE arithmetic.
class OdeSolver
{
  public:
    OdeSolver(const OdeSolver&) = delete;
    OdeSolver& operator=(const OdeSolver&) = delete;
    OdeSolver(OdeSolver&&) = delete;
    OdeSolver& operator=(OdeSolver&&) = delete;
    virtual ~OdeSolver();

    [[nodiscard]] OdeSolverKind kind() const noexcept;

    /// The current state, like the array OSP obtains from `ODE.getState()`. Writing values in
    /// place (Tracker's `resetState`/`restoreState`) keeps solver history such as Adams
    /// predictor rates or the LeapFrog prior state; call `initialize` to restart it.
    [[nodiscard]] std::span<double> state() noexcept;
    [[nodiscard]] std::span<const double> state() const noexcept;

    /// Copies new state values. A different length reinitializes the solver with `stepSize()`.
    /// OSP's RK4, Ralston2, Heun3, Butcher5, Fehlberg8, Adams, Verlet and LeapFrog do the same
    /// when `step()` sees a new length; its other solvers keep the old equation count and either
    /// ignore the new components or fail with an index error.
    void setState(std::span<const double> values);

    /// OSP `initialize(stepSize)`: sets the (fixed or initial) step and restarts solver history.
    virtual void initialize(double stepSize) = 0;

    /// Advances the state once, exactly as the OSP class does.
    OdeStepResult step();

    /// Calls `step()` `count` times without stopping on a failure, matching Tracker's loops.
    OdeAdvanceResult advance(std::size_t count);

    /// Steps until the time component (the last state value) is within half of `stepSize()` of
    /// `targetTime`, in the stepping direction. Steps are never shortened, so fixed-step results
    /// equal some whole-step sequence; use `Multistep` or `Rk45MultiStep` to land on an exact
    /// interval. This is a convenience, not Tracker's sequence: Tracker computes step counts with
    /// integer division, so reproducing a model uses `advance(count)`. Stops without reaching the
    /// target when a step does not move time toward it, or after `maxSteps` steps.
    OdeAdvanceResult advanceTo(double targetTime, std::size_t maxSteps = 10'000'000);

    /// OSP `setStepSize`; adaptive solvers treat it as the next attempted step.
    virtual void setStepSize(double stepSize) = 0;

    /// OSP `getStepSize`: the fixed step, the next adaptive attempt, or a multistep wrapper's
    /// fixed interval.
    [[nodiscard]] virtual double stepSize() const noexcept = 0;

    /// Sets the tolerance of an adaptive solver (OSP stores its absolute value). Returns false,
    /// changing nothing, for a fixed-step solver.
    virtual bool setTolerance(double tolerance);

    /// Tolerance of an adaptive solver, or nothing for a fixed-step solver.
    [[nodiscard]] virtual std::optional<double> tolerance() const noexcept;

    /// The OSP `getErrorCode()` value after the last step (always `OSP_NO_ERROR` for fixed-step).
    [[nodiscard]] virtual int ospErrorCode() const noexcept;

    /// Detail for the last `DID_NOT_CONVERGE`, or `None`.
    [[nodiscard]] virtual OdeConvergenceFailure convergenceFailure() const noexcept;

    /// Number of rate evaluations since construction, including construction-time work such as
    /// the LeapFrog prior-state estimate.
    [[nodiscard]] std::uint64_t rateEvaluations() const noexcept;

  protected:
    OdeSolver(OdeSolverKind kind, OdeRate rate, std::span<const double> initialState);

    /// Invokes the rate callback and counts the evaluation.
    void evaluateRate(std::span<const double> state, std::span<double> rate);

    /// Replaces values (and length) without the reinitialization `setState` performs.
    void assignState(std::span<const double> values);

    /// Reproduces the work of the Java constructor, which calls `initialize` with the class's
    /// default step size. Called once by the factory.
    virtual void construct() = 0;

  private:
    friend std::unique_ptr<OdeSolver> makeOdeSolver(OdeSolverKind kind, OdeRate rate, std::span<const double> initialState);

    /// The OSP `step()` body; returns the step taken.
    virtual double doStep() = 0;

    OdeSolverKind m_Kind;
    OdeRate m_Rate;
    std::vector<double> m_State;
    std::uint64_t m_RateEvaluations = 0;
};

/// Creates a solver in the state its Java constructor leaves: the default step size (0.1, or
/// 0.01 for the embedded pairs) and default tolerance 1e-6. Tracker then calls `initialize` with
/// the step of docs/ode-solvers.md. Returns null if `rate` is empty or `kind` is not an enumerator.
[[nodiscard]] std::unique_ptr<OdeSolver> makeOdeSolver(OdeSolverKind kind, OdeRate rate, std::span<const double> initialState);

/// `ODESolverFactory.createODESolver`: null for an unknown name (or an empty `rate`).
[[nodiscard]] std::unique_ptr<OdeSolver> createOdeSolver(std::string_view factoryName, OdeRate rate, std::span<const double> initialState);

} // namespace motrack::science
