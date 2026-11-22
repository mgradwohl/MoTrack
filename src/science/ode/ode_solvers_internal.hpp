#pragma once

// Internal OSP solver classes behind motrack::science::OdeSolver. Each class mirrors the Java class
// of the same name (fields, inheritance and floating-point operation order); see ode_solver.hpp.

#include "science/ode/ode_solver.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace motrack::science::detail
{

/// Java `Float.MIN_VALUE` widened to double, used by OSP as a "negligible" threshold.
inline constexpr double FLOAT_MIN_VALUE = static_cast<double>(std::numeric_limits<float>::denorm_min());

/// Java `Math.max`: NaN if either argument is NaN, and +0.0 is greater than -0.0.
[[nodiscard]] double javaMax(double a, double b) noexcept;

/// Java `Math.min`: NaN if either argument is NaN, and -0.0 is less than +0.0.
[[nodiscard]] double javaMin(double a, double b) noexcept;

/// `AbstractODESolver`: a fixed step size (default 0.1) and the equation count.
class AbstractSolver : public OdeSolver
{
  public:
    void initialize(double stepSize) override;
    void setStepSize(double stepSize) override;
    [[nodiscard]] double stepSize() const noexcept override;

  protected:
    using OdeSolver::OdeSolver;
    void construct() override;

    double m_StepSize = 0.1;
    std::size_t m_NumEqn = 0;
};

class EulerSolver final : public AbstractSolver
{
  public:
    EulerSolver(OdeRate rate, std::span<const double> initialState);
    void initialize(double stepSize) override;

  private:
    double doStep() override;

    std::vector<double> m_Rate;
};

class EulerRichardsonSolver final : public AbstractSolver
{
  public:
    EulerRichardsonSolver(OdeRate rate, std::span<const double> initialState);
    void initialize(double stepSize) override;

  private:
    double doStep() override;

    std::vector<double> m_Rate;
    std::vector<double> m_MidState;
};

class Ralston2Solver final : public AbstractSolver
{
  public:
    Ralston2Solver(OdeRate rate, std::span<const double> initialState);
    void initialize(double stepSize) override;

  private:
    double doStep() override;

    std::vector<double> m_Rate1;
    std::vector<double> m_Rate2;
    std::vector<double> m_EstimatedState;
};

class Heun3Solver final : public AbstractSolver
{
  public:
    Heun3Solver(OdeRate rate, std::span<const double> initialState);
    void initialize(double stepSize) override;

  private:
    double doStep() override;

    std::vector<double> m_Rate1;
    std::vector<double> m_Rate2;
    std::vector<double> m_Rate3;
    std::vector<double> m_EstimatedState;
};

class Rk4Solver : public AbstractSolver
{
  public:
    Rk4Solver(OdeRate rate, std::span<const double> initialState, OdeSolverKind kind = OdeSolverKind::Rk4);
    void initialize(double stepSize) override;

  protected:
    double doStep() override;

  private:
    std::vector<double> m_Rate1;
    std::vector<double> m_Rate2;
    std::vector<double> m_Rate3;
    std::vector<double> m_Rate4;
    std::vector<double> m_EstimatedState;
};

class Butcher5Solver : public AbstractSolver
{
  public:
    Butcher5Solver(OdeRate rate, std::span<const double> initialState, OdeSolverKind kind = OdeSolverKind::Butcher5);
    void initialize(double stepSize) override;

  protected:
    double doStep() override;

  private:
    std::array<std::vector<double>, 6> m_Rates;
    std::vector<double> m_EstimatedState;
};

class Fehlberg8Solver : public AbstractSolver
{
  public:
    Fehlberg8Solver(OdeRate rate, std::span<const double> initialState, OdeSolverKind kind = OdeSolverKind::Fehlberg8);
    void initialize(double stepSize) override;

  protected:
    double doStep() override;

  private:
    std::array<std::vector<double>, 13> m_Rates;
    std::vector<double> m_EstimatedState;
};

/// `Adams4 extends RK4`: RK4 for the first three steps, then a 4-step predictor-corrector.
class Adams4Solver final : public Rk4Solver
{
  public:
    Adams4Solver(OdeRate rate, std::span<const double> initialState);
    void initialize(double stepSize) override;

  private:
    double doStep() override;

    std::array<std::vector<double>, 4> m_Fn; // fn, fn1, fn2, fn3
    std::vector<double> m_TempState;
    std::vector<double> m_TempRate;
    int m_Counter = 0;
};

/// `Adams5 extends Butcher5`: Butcher5 for the first four steps.
class Adams5Solver final : public Butcher5Solver
{
  public:
    Adams5Solver(OdeRate rate, std::span<const double> initialState);
    void initialize(double stepSize) override;

  private:
    double doStep() override;

    std::array<std::vector<double>, 5> m_Fn;
    std::vector<double> m_TempState;
    std::vector<double> m_TempRate;
    int m_Counter = 0;
};

/// `Adams6 extends Fehlberg8`: Fehlberg8 for the first five steps. The predictor uses the
/// Adams-Bashforth weights over 1440; OSP divides them by 720 (docs/ode-solvers.md).
class Adams6Solver final : public Fehlberg8Solver
{
  public:
    Adams6Solver(OdeRate rate, std::span<const double> initialState);
    void initialize(double stepSize) override;

  private:
    double doStep() override;

    std::array<std::vector<double>, 6> m_Fn;
    std::vector<double> m_TempState;
    std::vector<double> m_TempRate;
    int m_Counter = 0;
};

class VerletSolver final : public AbstractSolver
{
  public:
    VerletSolver(OdeRate rate, std::span<const double> initialState);
    void initialize(double stepSize) override;

  private:
    double doStep() override;

    std::vector<double> m_Rate1;
    std::vector<double> m_Rate2;
};

/// `LeapFrog`: not self-starting; `initialize` (and therefore `setStepSize`) estimates the prior
/// state with a backward velocity-Verlet step, which evaluates the rate twice.
class LeapFrogSolver final : public AbstractSolver
{
  public:
    LeapFrogSolver(OdeRate rate, std::span<const double> initialState);
    void initialize(double stepSize) override;
    void setStepSize(double stepSize) override;

  private:
    double doStep() override;
    void estimatePreviousState();

    std::vector<double> m_Rate;
    std::vector<double> m_PriorState;
    std::vector<double> m_CurrentState;
};

/// Butcher tableau of a six-stage embedded pair as written in OSP: `a` rows (stage s uses the
/// first s entries of row s - 1), the fifth-order weights and the error weights (b5 - b4).
struct EmbeddedPair
{
    std::array<std::array<double, 5>, 5> a;
    std::array<double, 6> b5;
    std::array<double, 6> er;
};

/// `CashKarp45` and `DormandPrince45` share one implementation; only the tableau and the
/// tolerance floor differ (Dormand-Prince clamps tolerances below 1e-12, Cash-Karp only warns).
class EmbeddedPairSolver : public OdeSolver
{
  public:
    void initialize(double stepSize) override;
    void setStepSize(double stepSize) override;
    [[nodiscard]] double stepSize() const noexcept override;
    bool setTolerance(double tolerance) override;
    [[nodiscard]] std::optional<double> tolerance() const noexcept override;
    [[nodiscard]] int ospErrorCode() const noexcept override;
    [[nodiscard]] OdeConvergenceFailure convergenceFailure() const noexcept override;

    /// The OSP `step()` body, for wrappers that drive this solver directly.
    double stepOnce();

  protected:
    EmbeddedPairSolver(
        OdeSolverKind kind, const EmbeddedPair& pair, bool clampsTolerance, OdeRate rate, std::span<const double> initialState);
    void construct() override;
    double doStep() override;

    double m_StepSize = 0.01;
    double m_Tol = 1.0e-6;
    int m_ErrorCode = OSP_NO_ERROR;
    OdeConvergenceFailure m_Failure = OdeConvergenceFailure::None;

  private:
    const EmbeddedPair* m_Pair;
    bool m_ClampsTolerance;
    std::size_t m_NumEqn = 0;
    std::vector<double> m_TempState;
    std::array<std::vector<double>, 6> m_K;
};

class CashKarp45Solver final : public EmbeddedPairSolver
{
  public:
    CashKarp45Solver(OdeRate rate, std::span<const double> initialState);
};

/// `DormandPrince45`, and `RK45`, which extends it without changes.
class DormandPrince45Solver : public EmbeddedPairSolver
{
  public:
    DormandPrince45Solver(OdeRate rate, std::span<const double> initialState, OdeSolverKind kind = OdeSolverKind::DormandPrince45);
};

/// `RK45MultiStep extends RK45`: repeats Dormand-Prince steps on the solver's own state until a
/// fixed interval is covered.
class Rk45MultiStepSolver final : public DormandPrince45Solver
{
  public:
    Rk45MultiStepSolver(OdeRate rate, std::span<const double> initialState);
    void initialize(double stepSize) override;
    void setStepSize(double stepSize) override;
    [[nodiscard]] double stepSize() const noexcept override;
    [[nodiscard]] OdeConvergenceFailure convergenceFailure() const noexcept override;

  private:
    void construct() override;
    double doStep() override;
    double plus();
    double minus();
    bool failed(double oldRemainder, double remainder, int counter, bool forward);

    double m_FixedStepSize = 0.1;
    OdeConvergenceFailure m_MultistepFailure = OdeConvergenceFailure::None;
};

/// `ODEMultistepSolver`: a Dormand-Prince engine that advances a private copy of the state
/// (`InternalODE`) and copies it back after each fixed interval.
class MultistepSolver final : public OdeSolver
{
  public:
    MultistepSolver(OdeRate rate, std::span<const double> initialState);
    void initialize(double stepSize) override;
    void setStepSize(double stepSize) override;
    [[nodiscard]] double stepSize() const noexcept override;
    bool setTolerance(double tolerance) override;
    [[nodiscard]] std::optional<double> tolerance() const noexcept override;
    [[nodiscard]] int ospErrorCode() const noexcept override;
    [[nodiscard]] OdeConvergenceFailure convergenceFailure() const noexcept override;

  private:
    void construct() override;
    double doStep() override;
    double plus();
    double minus();
    bool failed(double oldRemainder, double remainder, int counter, bool forward);
    void setInitialConditions();

    std::unique_ptr<DormandPrince45Solver> m_Engine;
    double m_FixedStepSize = 0.1;
    int m_ErrCode = OSP_NO_ERROR;
    OdeConvergenceFailure m_Failure = OdeConvergenceFailure::None;
};

} // namespace motrack::science::detail
