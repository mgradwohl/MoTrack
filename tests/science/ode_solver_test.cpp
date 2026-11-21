#include "science/ode/ode_solver.hpp"

#include "science/ode/ode_solvers_internal.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{

using motrack::science::OdeConvergenceFailure;
using motrack::science::OdeRate;
using motrack::science::OdeSolver;
using motrack::science::OdeSolverKind;
using motrack::science::OdeStepStatus;

// ---------------------------------------------------------------------------------------------
// Test systems. Each rate function repeats tools/reference/OdeOracle.java operation by operation
// (this file is compiled with -ffp-contract=off), so fixed-step results must match bit for bit.

constexpr double OSCILLATOR_OMEGA_SQUARED = 4.0;
constexpr double DRAG_GRAVITY = 9.8;
constexpr double DRAG_COEFFICIENT = 0.1;
constexpr double KEPLER_G = std::numbers::pi * std::numbers::pi / 4.0;
constexpr double KEPLER_M1 = 3.0;
constexpr double KEPLER_M2 = 1.0;
// Java computes Math.PI * Math.sqrt(3.0); sqrt is correctly rounded, so sqrt3 is the same double.
constexpr double KEPLER_PERIAPSIS_SPEED = std::numbers::pi * std::numbers::sqrt3;
constexpr double FORCED_OMEGA_SQUARED = 9.0;
constexpr double FORCED_LINEAR = 2.0;
constexpr double FORCED_QUADRATIC = -1.0;
constexpr double NAN_RATE_START = 0.25;
constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();
constexpr double INFINITE = std::numeric_limits<double>::infinity();

void oscillatorRate(std::span<const double> s, std::span<double> r)
{
    r[0] = s[1];
    r[1] = -OSCILLATOR_OMEGA_SQUARED * s[0];
    r[2] = 1.0;
}

void dragRate(std::span<const double> s, std::span<double> r)
{
    const double speed = std::sqrt(s[1] * s[1] + s[3] * s[3]);
    r[0] = s[1];
    r[1] = -DRAG_COEFFICIENT * speed * s[1];
    r[2] = s[3];
    r[3] = -DRAG_GRAVITY - DRAG_COEFFICIENT * speed * s[3];
    r[4] = 1.0;
}

void keplerRate(std::span<const double> s, std::span<double> r)
{
    const double dx = s[0] - s[4];
    const double dy = s[2] - s[6];
    const double r2 = dx * dx + dy * dy;
    const double r3 = r2 * std::sqrt(r2);
    const double f = KEPLER_G * KEPLER_M1 * KEPLER_M2 / r3;
    const double fx = -f * dx;
    const double fy = -f * dy;
    r[0] = s[1];
    r[1] = fx / KEPLER_M1;
    r[2] = s[3];
    r[3] = fy / KEPLER_M1;
    r[4] = s[5];
    r[5] = -fx / KEPLER_M2;
    r[6] = s[7];
    r[7] = -fy / KEPLER_M2;
    r[8] = 1.0;
}

void stiffRate(std::span<const double> s, std::span<double> r)
{
    r[0] = -50.5 * s[0] + 49.5 * s[1];
    r[1] = 49.5 * s[0] - 50.5 * s[1];
    r[2] = 1.0;
}

void forcedRate(std::span<const double> s, std::span<double> r)
{
    r[0] = s[1];
    r[1] = -FORCED_OMEGA_SQUARED * s[0] + FORCED_LINEAR * s[2] + FORCED_QUADRATIC * s[2] * s[2];
    r[2] = 1.0;
}

void nanRate(std::span<const double> s, std::span<double> r)
{
    r[0] = s[1];
    r[1] = s[2] < NAN_RATE_START ? -OSCILLATOR_OMEGA_SQUARED * s[0] : NOT_A_NUMBER;
    r[2] = 1.0;
}

struct TestSystem
{
    std::vector<double> initial;
    OdeRate rate;
};

TestSystem testSystem(std::string_view id)
{
    if (id == "oscillator")
    {
        return {.initial = {1.0, 0.0, 0.0}, .rate = oscillatorRate};
    }
    if (id == "drag")
    {
        return {.initial = {0.0, 10.0, 0.0, 10.0, 0.0}, .rate = dragRate};
    }
    if (id == "kepler")
    {
        return {.initial = {0.125, 0.0, 0.0, 0.25 * KEPLER_PERIAPSIS_SPEED, -0.375, 0.0, 0.0, -0.75 * KEPLER_PERIAPSIS_SPEED, 0.0},
                .rate = keplerRate};
    }
    if (id == "stiff")
    {
        return {.initial = {2.0, 0.0, 0.0}, .rate = stiffRate};
    }
    if (id == "forced")
    {
        return {.initial = {0.5, 0.0, -0.5}, .rate = forcedRate};
    }
    if (id == "nan-rate")
    {
        return {.initial = {1.0, 0.0, 0.0}, .rate = nanRate};
    }
    return {};
}

std::unique_ptr<OdeSolver> makeSolver(OdeSolverKind kind, std::string_view system)
{
    auto test = testSystem(system);
    return motrack::science::makeOdeSolver(kind, std::move(test.rate), test.initial);
}

// Exact solutions used as independent expectations.
double oscillatorX(double t)
{
    return std::cos(2.0 * t);
}

double oscillatorV(double t)
{
    return -2.0 * std::sin(2.0 * t);
}

struct ForcedSolution
{
    double x;
    double v;
};

ForcedSolution forcedExact(double t)
{
    // Particular solution of x'' + 9x = 2t - t^2 plus the homogeneous part from x(-0.5) = 0.5, v = 0.
    const auto particular = [](double time)
    {
        return (2.0 * time - time * time) / 9.0 + 2.0 / 81.0;
    };
    const auto particularRate = [](double time)
    {
        return (2.0 - 2.0 * time) / 9.0;
    };
    constexpr double START_TIME = -0.5;
    constexpr double OMEGA = 3.0;
    const double c = 0.5 - particular(START_TIME);
    const double d = (0.0 - particularRate(START_TIME)) / OMEGA;
    const double phase = OMEGA * (t - START_TIME);
    return {.x = particular(t) + c * std::cos(phase) + d * std::sin(phase),
            .v = particularRate(t) - c * OMEGA * std::sin(phase) + d * OMEGA * std::cos(phase)};
}

double maxStateError(std::span<const double> actual, std::span<const double> expected)
{
    double error = 0;
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        error = std::max(error, std::fabs(actual[index] - expected[index]));
    }
    return error;
}

// ---------------------------------------------------------------------------------------------
// Java oracle fixture (tests/science/ode-reference.json, regenerated by `corpus.py ode-oracle`).

const QJsonObject& reference()
{
    static const QJsonObject document = []
    {
        QFile file(QStringLiteral(MOTRACK_ODE_REFERENCE));
        if (!file.open(QIODevice::ReadOnly))
        {
            return QJsonObject{};
        }
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return document;
}

std::string text(const QJsonValue& value)
{
    return value.toString().toStdString();
}

double number(const QJsonValue& value)
{
    if (value.isString())
    {
        const auto name = value.toString();
        if (name == QStringLiteral("NaN"))
        {
            return NOT_A_NUMBER;
        }
        if (name == QStringLiteral("Infinity"))
        {
            return INFINITE;
        }
        if (name == QStringLiteral("-Infinity"))
        {
            return -INFINITE;
        }
        ADD_FAILURE() << "Unexpected numeric string " << name.toStdString();
    }
    return value.toDouble();
}

std::vector<double> numbers(const QJsonValue& value)
{
    std::vector<double> result;
    for (const auto& item : value.toArray())
    {
        result.push_back(number(item));
    }
    return result;
}

std::map<std::string, OdeSolverKind> solverIds()
{
    std::map<std::string, OdeSolverKind> ids;
    for (const auto& solver : reference()[QStringLiteral("solvers")].toArray())
    {
        const auto object = solver.toObject();
        const auto kind = motrack::science::odeSolverFromClassName(text(object[QStringLiteral("class")]));
        if (kind)
        {
            ids.emplace(text(object[QStringLiteral("id")]), *kind);
        }
    }
    return ids;
}

/// How closely a solver must reproduce the Java values (the same classes as corpus.py ode-oracle).
///
/// Fixed-step solvers use only +, -, *, / on rates that are themselves arithmetic and sqrt, so
/// IEEE 754 makes every value identical: they must match exactly. The adaptive solvers also call
/// Math.pow for the step-size factor. Neither HotSpot's x86-64 intrinsic nor the C++ libraries
/// round it correctly in every case (about 0.07% of inputs differ by one ulp between Java and the
/// Windows runtime), and HotSpot on arm64 uses fdlibm instead, so no implementation can match
/// Java's last bit everywhere. On a non-stiff system that perturbation stays near 1e-16; a relative
/// 1e-12 tolerance (absolute 1e-13 near zero, against O(1) trajectories) leaves four orders of
/// margin and still rejects a wrong coefficient, formula or branch, whose effects appear at the
/// truncation-error level (>= 1e-10 here). On the stiff system the controller holds h at the
/// stability limit, where the error estimate varies like (h lambda)^5 and feeds back into h; on
/// Windows it amplified one-ulp differences to 7e-9 relative in a step size (1e-13 absolute on a
/// decayed component), so that class allows 1e-6 relative (1e-10 absolute). Integer data (step counts, rate evaluations and error
/// codes, i.e. the accept/reject sequence) must match exactly in every case.
struct Agreement
{
    double relative = 0;
    double absolute = 0;
};

Agreement agreement(OdeSolverKind kind, std::string_view system)
{
    if (!motrack::science::isAdaptiveOdeSolver(kind))
    {
        return {};
    }
    return system == "stiff" ? Agreement{.relative = 1e-6, .absolute = 1e-10} : Agreement{.relative = 1e-12, .absolute = 1e-13};
}

bool agrees(double expected, double actual, Agreement tolerance)
{
    if (std::isnan(expected) || std::isnan(actual))
    {
        return std::isnan(expected) && std::isnan(actual);
    }
    if (expected == actual)
    {
        return true;
    }
    if (std::isinf(expected) || std::isinf(actual))
    {
        return false;
    }
    return std::fabs(expected - actual) <=
           std::max(tolerance.relative * std::max(std::fabs(expected), std::fabs(actual)), tolerance.absolute);
}

/// Adams6 differs from Java after its five Fehlberg8 starting steps because MoTrack corrects the
/// predictor weights (docs/ode-solvers.md); its state is compared only for the starting steps.
constexpr int ADAMS6_STARTING_STEPS = 5;

TEST(OdeOracle, FixtureDescribesEverySolverAndTrackerDefaults)
{
    const auto& document = reference();
    ASSERT_FALSE(document.isEmpty()) << "Missing or malformed " << MOTRACK_ODE_REFERENCE;
    EXPECT_EQ(document[QStringLiteral("schema_version")].toInt(), 1);
    EXPECT_EQ(text(document[QStringLiteral("reference_version")]), "6.3.5");

    const auto solvers = document[QStringLiteral("solvers")].toArray();
    ASSERT_EQ(static_cast<std::size_t>(solvers.size()), motrack::science::ALL_ODE_SOLVER_KINDS.size());
    for (const auto& value : solvers)
    {
        const auto solver = value.toObject();
        const auto className = text(solver[QStringLiteral("class")]);
        const auto kind = motrack::science::odeSolverFromClassName(className);
        ASSERT_TRUE(kind) << className;
        EXPECT_EQ(motrack::science::odeSolverClassName(*kind), className);
        EXPECT_EQ(motrack::science::isAdaptiveOdeSolver(*kind), solver[QStringLiteral("adaptive")].toBool()) << className;
        const auto factory = solver[QStringLiteral("factory")];
        const auto expectedFactory = motrack::science::odeSolverFactoryName(*kind);
        if (factory.isNull())
        {
            EXPECT_FALSE(expectedFactory) << className;
        }
        else
        {
            ASSERT_TRUE(expectedFactory) << className;
            EXPECT_EQ(*expectedFactory, text(factory));
            EXPECT_EQ(motrack::science::odeSolverFromFactoryName(text(factory)), kind);
        }
    }

    // Read from the pinned ModelBuilder and ParticleModel classes by the oracle.
    const auto tracker = document[QStringLiteral("tracker")].toObject();
    const auto builder = tracker[QStringLiteral("model_builder_solvers")].toArray();
    ASSERT_EQ(static_cast<std::size_t>(builder.size()), motrack::science::TRACKER_MODEL_BUILDER_SOLVERS.size());
    for (std::size_t index = 0; index < motrack::science::TRACKER_MODEL_BUILDER_SOLVERS.size(); ++index)
    {
        EXPECT_EQ(text(builder.at(static_cast<qsizetype>(index))),
                  motrack::science::odeSolverClassName(motrack::science::TRACKER_MODEL_BUILDER_SOLVERS.at(index)));
    }
    EXPECT_EQ(motrack::science::TRACKER_MODEL_BUILDER_SOLVERS.front(), motrack::science::TRACKER_DEFAULT_ODE_SOLVER);
    // Tracker steps the solver 10 times per trace point and draws 10 trace points per clip step.
    EXPECT_EQ(tracker[QStringLiteral("trace_points_per_step")].toInt(), 10);

    for (const auto& value : document[QStringLiteral("systems")].toArray())
    {
        const auto system = value.toObject();
        const auto id = text(system[QStringLiteral("id")]);
        const auto test = testSystem(id);
        ASSERT_TRUE(test.rate) << id;
        const auto initial = numbers(system[QStringLiteral("initial_state")]);
        ASSERT_EQ(initial.size(), test.initial.size()) << id;
        for (std::size_t index = 0; index < initial.size(); ++index)
        {
            EXPECT_EQ(initial[index], test.initial[index]) << id << " initial state " << index;
        }
    }
}

TEST(OdeOracle, EverySolverReproducesTheJavaTrajectories)
{
    const auto ids = solverIds();
    const auto cases = reference()[QStringLiteral("cases")].toArray();
    ASSERT_GE(cases.size(), 200);
    std::map<OdeSolverKind, int> casesPerSolver;
    for (const auto& value : cases)
    {
        const auto test = value.toObject();
        const auto id = text(test[QStringLiteral("id")]);
        SCOPED_TRACE(id);
        const auto found = ids.find(text(test[QStringLiteral("solver")]));
        ASSERT_NE(found, ids.end());
        const auto kind = found->second;
        ++casesPerSolver[kind];
        const auto system = text(test[QStringLiteral("system")]);
        const auto tolerance = agreement(kind, system);
        auto solver = makeSolver(kind, system);
        ASSERT_TRUE(solver);

        const auto setup = text(test[QStringLiteral("setup")]);
        if (setup == "initialize")
        {
            solver->initialize(number(test[QStringLiteral("step_size")]));
        }
        else if (setup == "set_step")
        {
            solver->setStepSize(number(test[QStringLiteral("step_size")]));
        }
        else
        {
            ASSERT_EQ(setup, "construct");
        }
        if (!test[QStringLiteral("tolerance")].isNull())
        {
            ASSERT_TRUE(solver->setTolerance(number(test[QStringLiteral("tolerance")])));
        }
        const auto effectiveTolerance = test[QStringLiteral("effective_tolerance")];
        ASSERT_EQ(solver->tolerance().has_value(), !effectiveTolerance.isNull());
        if (!effectiveTolerance.isNull())
        {
            EXPECT_EQ(solver->tolerance(), number(effectiveTolerance));
        }

        const auto steps = test[QStringLiteral("steps")].toInt();
        int stepIndex = 0;
        motrack::science::OdeStepResult last;
        for (const auto& checkpointValue : test[QStringLiteral("checkpoints")].toArray())
        {
            const auto checkpoint = checkpointValue.toObject();
            const int target = checkpoint[QStringLiteral("n")].toInt();
            ASSERT_LE(target, steps);
            for (; stepIndex < target; ++stepIndex)
            {
                last = solver->step();
            }
            SCOPED_TRACE("after step " + std::to_string(target));
            if (target > 0)
            {
                EXPECT_TRUE(agrees(number(checkpoint[QStringLiteral("taken")]), last.stepTaken, tolerance))
                    << number(checkpoint[QStringLiteral("taken")]) << " vs " << last.stepTaken;
            }
            EXPECT_TRUE(agrees(number(checkpoint[QStringLiteral("step_size")]), solver->stepSize(), tolerance))
                << number(checkpoint[QStringLiteral("step_size")]) << " vs " << solver->stepSize();
            const auto errorCode = checkpoint[QStringLiteral("error_code")];
            EXPECT_EQ(solver->ospErrorCode(), errorCode.isNull() ? motrack::science::OSP_NO_ERROR : errorCode.toInt());
            EXPECT_EQ(solver->rateEvaluations(), static_cast<std::uint64_t>(checkpoint[QStringLiteral("rates")].toInteger()));
            if (kind == OdeSolverKind::Adams6 && target > ADAMS6_STARTING_STEPS)
            {
                continue;
            }
            const auto expected = numbers(checkpoint[QStringLiteral("state")]);
            const auto actual = solver->state();
            ASSERT_EQ(expected.size(), actual.size());
            for (std::size_t index = 0; index < expected.size(); ++index)
            {
                EXPECT_TRUE(agrees(expected[index], actual[index], tolerance))
                    << "state[" << index << "] expected " << expected[index] << " got " << actual[index] << " (difference "
                    << actual[index] - expected[index] << ")";
            }
        }
    }
    EXPECT_EQ(casesPerSolver.size(), motrack::science::ALL_ODE_SOLVER_KINDS.size());
}

TEST(OdeOracle, OspAdams6PredictorDefectIsReplacedBySixthOrderAccuracy)
{
    // Independent check of the deliberate difference: OSP's Adams6 is far less accurate than its
    // own Fehlberg8 starter, while MoTrack's corrected predictor is accurate to roundoff here.
    std::optional<QJsonObject> javaCase;
    for (const auto& value : reference()[QStringLiteral("cases")].toArray())
    {
        if (text(value.toObject()[QStringLiteral("id")]) == "oscillator/adams6/h=0.01")
        {
            javaCase = value.toObject();
        }
    }
    ASSERT_TRUE(javaCase);
    const auto last = (*javaCase)[QStringLiteral("checkpoints")].toArray().last().toObject();
    const auto javaState = numbers(last[QStringLiteral("state")]);
    const std::vector exact{oscillatorX(2.0), oscillatorV(2.0)};
    const double javaError = maxStateError(std::span(javaState).first(2), exact);

    auto solver = makeSolver(OdeSolverKind::Adams6, "oscillator");
    solver->initialize(0.01);
    solver->advance(200);
    const double nativeError = maxStateError(solver->state().first(2), exact);
    EXPECT_GT(javaError, 1e-6);
    EXPECT_LT(nativeError, 1e-11);
    EXPECT_GT(javaError, 1e4 * nativeError);
}

// ---------------------------------------------------------------------------------------------
// Independent expectations: convergence orders and exact solutions.

struct OrderCase
{
    OdeSolverKind kind;
    double order;
    int coarseSteps;
};

TEST(OdeSolverAccuracy, EachMethodConvergesAtItsDesignOrder)
{
    // Halving h divides the global error by 2^p. A wrong tableau, weight or history shift lowers
    // the observed order, so a port that is consistently wrong cannot pass this test.
    const std::vector<OrderCase> cases{
        {.kind = OdeSolverKind::Euler, .order = 1, .coarseSteps = 200},
        {.kind = OdeSolverKind::EulerRichardson, .order = 2, .coarseSteps = 100},
        {.kind = OdeSolverKind::Ralston2, .order = 2, .coarseSteps = 100},
        {.kind = OdeSolverKind::Heun3, .order = 3, .coarseSteps = 50},
        {.kind = OdeSolverKind::Rk4, .order = 4, .coarseSteps = 20},
        {.kind = OdeSolverKind::Butcher5, .order = 5, .coarseSteps = 10},
        {.kind = OdeSolverKind::Fehlberg8, .order = 8, .coarseSteps = 4},
        {.kind = OdeSolverKind::Adams4, .order = 4, .coarseSteps = 40},
        {.kind = OdeSolverKind::Adams5, .order = 5, .coarseSteps = 40},
        {.kind = OdeSolverKind::Adams6, .order = 6, .coarseSteps = 40},
        {.kind = OdeSolverKind::Verlet, .order = 2, .coarseSteps = 100},
        {.kind = OdeSolverKind::LeapFrog, .order = 2, .coarseSteps = 100},
    };
    constexpr double DURATION = 1.0;
    const std::vector exact{oscillatorX(DURATION), oscillatorV(DURATION)};
    for (const auto& test : cases)
    {
        SCOPED_TRACE(std::string(motrack::science::odeSolverClassName(test.kind)));
        std::vector<double> errors;
        for (const int steps : {test.coarseSteps, 2 * test.coarseSteps})
        {
            auto solver = makeSolver(test.kind, "oscillator");
            solver->initialize(DURATION / steps);
            EXPECT_EQ(solver->advance(static_cast<std::size_t>(steps)).status, OdeStepStatus::Ok);
            // Positions only: the LeapFrog velocity is a differenced estimate, not an integrated state.
            const std::size_t components = test.kind == OdeSolverKind::LeapFrog ? 1 : 2;
            errors.push_back(maxStateError(solver->state().first(components), std::span(exact).first(components)));
        }
        const double observed = std::log2(errors[0] / errors[1]);
        EXPECT_NEAR(observed, test.order, 0.35) << "errors " << errors[0] << ", " << errors[1];
    }
}

TEST(OdeSolverAccuracy, RungeKuttaMatchesExactOscillatorAndForcedSolutions)
{
    auto oscillator = makeSolver(OdeSolverKind::Rk4, "oscillator");
    oscillator->initialize(0.001);
    oscillator->advance(2000);
    // Global RK4 error is about N (omega h)^5 / 120 = 5e-13 here.
    EXPECT_NEAR(oscillator->state()[0], oscillatorX(2.0), 2e-12);
    EXPECT_NEAR(oscillator->state()[1], oscillatorV(2.0), 4e-12);
    EXPECT_NEAR(oscillator->state()[2], 2.0, 1e-12);

    // Explicit time dependence from a negative start time, as for a model starting before t = 0.
    for (const auto kind : {OdeSolverKind::Rk4, OdeSolverKind::Butcher5, OdeSolverKind::Fehlberg8})
    {
        auto forced = makeSolver(kind, "forced");
        forced->initialize(0.001);
        forced->advance(2000);
        const auto exact = forcedExact(forced->state()[2]);
        EXPECT_NEAR(forced->state()[2], 1.5, 1e-12);
        EXPECT_NEAR(forced->state()[0], exact.x, 1e-11);
        EXPECT_NEAR(forced->state()[1], exact.v, 3e-11);
    }
}

TEST(OdeSolverAccuracy, VerletUsesTheStepStartTimeForItsSecondRate)
{
    // OSP's velocity Verlet evaluates the second rate before advancing t, so explicit time
    // dependence is integrated to first order only. MoTrack keeps this documented OSP behavior.
    std::vector<double> errors;
    for (const int steps : {4000, 8000})
    {
        auto solver = makeSolver(OdeSolverKind::Verlet, "forced");
        solver->initialize(2.0 / steps);
        solver->advance(static_cast<std::size_t>(steps));
        const auto exact = forcedExact(solver->state()[2]);
        errors.push_back(std::fabs(solver->state()[1] - exact.v));
    }
    EXPECT_NEAR(std::log2(errors[0] / errors[1]), 1.0, 0.15);
}

TEST(OdeSolverAccuracy, KeplerOrbitReturnsAfterOnePeriodAndConservesInvariants)
{
    const auto initial = testSystem("kepler").initial;
    const auto energy = [](std::span<const double> s)
    {
        const double dx = s[0] - s[4];
        const double dy = s[2] - s[6];
        const double kinetic = 0.5 * KEPLER_M1 * (s[1] * s[1] + s[3] * s[3]) + 0.5 * KEPLER_M2 * (s[5] * s[5] + s[7] * s[7]);
        return kinetic - KEPLER_G * KEPLER_M1 * KEPLER_M2 / std::sqrt(dx * dx + dy * dy);
    };
    const auto angularMomentum = [](std::span<const double> s)
    {
        return KEPLER_M1 * (s[0] * s[3] - s[2] * s[1]) + KEPLER_M2 * (s[4] * s[7] - s[6] * s[5]);
    };
    for (const auto kind : {OdeSolverKind::Rk4, OdeSolverKind::Fehlberg8, OdeSolverKind::Multistep, OdeSolverKind::Rk45MultiStep})
    {
        SCOPED_TRACE(std::string(motrack::science::odeSolverClassName(kind)));
        auto solver = makeSolver(kind, "kepler");
        const bool multistep = kind == OdeSolverKind::Multistep || kind == OdeSolverKind::Rk45MultiStep;
        solver->initialize(multistep ? 0.1 : 0.001);
        if (multistep)
        {
            solver->setTolerance(1e-10);
        }
        EXPECT_EQ(solver->advance(multistep ? 20 : 2000).status, OdeStepStatus::Ok);
        const auto state = solver->state();
        // Linear momentum is a linear invariant, which every Runge-Kutta step preserves exactly.
        EXPECT_NEAR(KEPLER_M1 * state[1] + KEPLER_M2 * state[5], 0.0, 1e-12);
        EXPECT_NEAR(KEPLER_M1 * state[3] + KEPLER_M2 * state[7], 0.0, 1e-12);
        EXPECT_NEAR(energy(state), energy(initial), 1e-7 * std::fabs(energy(initial)));
        EXPECT_NEAR(angularMomentum(state), angularMomentum(initial), 1e-7 * std::fabs(angularMomentum(initial)));
        EXPECT_NEAR(state[8], 2.0, 1e-12);
        // The period of the a = 1 relative orbit is exactly 2 in these units.
        EXPECT_LT(maxStateError(state.first(8), std::span(initial).first(8)), 1e-6);
    }
}

TEST(OdeSolverAccuracy, DragFallFromRestMatchesTheTerminalVelocitySolution)
{
    auto solver = motrack::science::makeOdeSolver(OdeSolverKind::Rk4, dragRate, std::vector{0.0, 0.0, 0.0, 0.0, 0.0});
    solver->initialize(0.001);
    solver->advance(1500);
    const double terminal = std::sqrt(DRAG_GRAVITY / DRAG_COEFFICIENT);
    const double t = 1.5;
    EXPECT_EQ(solver->state()[0], 0.0);
    EXPECT_EQ(solver->state()[1], 0.0);
    EXPECT_NEAR(solver->state()[3], -terminal * std::tanh(DRAG_GRAVITY * t / terminal), 1e-11);
    EXPECT_NEAR(solver->state()[2], -(terminal * terminal / DRAG_GRAVITY) * std::log(std::cosh(DRAG_GRAVITY * t / terminal)), 1e-11);
}

TEST(OdeSolverAccuracy, StiffSystemSeparatesStableAdaptiveAndUnstableSteps)
{
    const auto exact = [](double t)
    {
        return std::vector{std::exp(-t) + std::exp(-100 * t), std::exp(-t) - std::exp(-100 * t)};
    };
    auto stable = makeSolver(OdeSolverKind::Rk4, "stiff");
    stable->initialize(0.01);
    EXPECT_EQ(stable->advance(100).status, OdeStepStatus::Ok);
    EXPECT_LT(maxStateError(stable->state().first(2), exact(1.0)), 1e-9);

    // h * lambda = -2.5 is outside Euler's stability interval: the fast mode grows by 1.5 per step.
    auto unstable = makeSolver(OdeSolverKind::Euler, "stiff");
    unstable->initialize(0.025);
    unstable->advance(40);
    EXPECT_GT(std::fabs(unstable->state()[0] - unstable->state()[1]), 1e6);

    // The embedded pair keeps its steps near its stability boundary (h * lambda of about -3.3),
    // so stiffness rather than accuracy sets the step count, and it stays accurate. Without the
    // fast mode the e^-t component would need about ten steps at this tolerance.
    auto adaptive = makeSolver(OdeSolverKind::DormandPrince45, "stiff");
    adaptive->initialize(0.01);
    adaptive->setTolerance(1e-8);
    double largest = 0;
    int steps = 0;
    while (adaptive->state()[2] < 1.0)
    {
        const auto result = adaptive->step();
        ASSERT_EQ(result.status, OdeStepStatus::Ok);
        largest = std::max(largest, result.stepTaken);
        ++steps;
    }
    EXPECT_LT(largest, 2 * 3.3 / 100);
    EXPECT_GT(steps, 25);
    EXPECT_LT(maxStateError(adaptive->state().first(2), exact(adaptive->state()[2])), 1e-6);

    // h * lambda = -10 overflows every explicit method; the solver reports instead of throwing.
    auto overflow = makeSolver(OdeSolverKind::Euler, "stiff");
    overflow->initialize(0.1);
    const auto result = overflow->advance(400);
    EXPECT_EQ(result.status, OdeStepStatus::NonFinite);
    ASSERT_TRUE(result.firstFailure);
    EXPECT_GT(*result.firstFailure, 300u);
    EXPECT_EQ(result.steps, 400u);
}

TEST(OdeSolverAccuracy, AdaptiveSolversMeetTheirToleranceAndMultistepHitsFrameTimes)
{
    for (const auto kind : {OdeSolverKind::CashKarp45, OdeSolverKind::DormandPrince45, OdeSolverKind::Rk45})
    {
        SCOPED_TRACE(std::string(motrack::science::odeSolverClassName(kind)));
        auto solver = makeSolver(kind, "oscillator");
        solver->initialize(0.01);
        solver->setTolerance(1e-10);
        const auto result = solver->advanceTo(2.0);
        EXPECT_EQ(result.status, OdeStepStatus::Ok);
        EXPECT_TRUE(result.reachedTarget);
        const double t = solver->state()[2];
        EXPECT_LE(std::fabs(t - 2.0), 0.5 * solver->stepSize());
        EXPECT_LT(std::fabs(solver->state()[0] - oscillatorX(t)), 1e-8);
    }
    for (const auto kind : {OdeSolverKind::Multistep, OdeSolverKind::Rk45MultiStep})
    {
        SCOPED_TRACE(std::string(motrack::science::odeSolverClassName(kind)));
        auto solver = makeSolver(kind, "forced");
        solver->initialize(0.1);
        solver->setTolerance(1e-10);
        for (int frame = 1; frame <= 20; ++frame)
        {
            const auto result = solver->step();
            ASSERT_EQ(result.status, OdeStepStatus::Ok);
            EXPECT_NEAR(result.stepTaken, 0.1, 1e-10 * 0.1);
            const auto exact = forcedExact(solver->state()[2]);
            EXPECT_NEAR(solver->state()[2], -0.5 + 0.1 * frame, 1e-12);
            EXPECT_NEAR(solver->state()[0], exact.x, 1e-8);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Public API and edge cases.

TEST(OdeSolverApi, NamesFollowTheFactoryAndTrackerClassNames)
{
    using motrack::science::odeSolverFromClassName;
    using motrack::science::odeSolverFromFactoryName;
    EXPECT_EQ(odeSolverFromFactoryName("rk4"), OdeSolverKind::Rk4);
    EXPECT_EQ(odeSolverFromFactoryName(" \tMultiStep\n"), OdeSolverKind::Multistep);
    EXPECT_EQ(odeSolverFromFactoryName("DORMANDPRINCE45"), OdeSolverKind::DormandPrince45);
    EXPECT_FALSE(odeSolverFromFactoryName("rk45"));
    EXPECT_FALSE(odeSolverFromFactoryName("leapfrog"));
    EXPECT_FALSE(odeSolverFromFactoryName(""));
    EXPECT_FALSE(odeSolverFromFactoryName("rk4x"));
    EXPECT_EQ(odeSolverFromClassName("org.opensourcephysics.numerics.RK45MultiStep"), OdeSolverKind::Rk45MultiStep);
    EXPECT_EQ(odeSolverFromClassName("org.opensourcephysics.numerics.LeapFrog"), OdeSolverKind::LeapFrog);
    EXPECT_FALSE(odeSolverFromClassName("org.opensourcephysics.numerics.rk4"));
    EXPECT_FALSE(odeSolverFromClassName("RK4"));
    for (const auto kind : motrack::science::ALL_ODE_SOLVER_KINDS)
    {
        EXPECT_EQ(odeSolverFromClassName(motrack::science::odeSolverClassName(kind)), kind);
        auto solver = makeSolver(kind, "oscillator");
        ASSERT_TRUE(solver);
        EXPECT_EQ(solver->kind(), kind);
    }

    auto created = motrack::science::createOdeSolver("  Verlet ", oscillatorRate, std::vector{1.0, 0.0, 0.0});
    ASSERT_TRUE(created);
    EXPECT_EQ(created->kind(), OdeSolverKind::Verlet);
    EXPECT_FALSE(motrack::science::createOdeSolver("unknown", oscillatorRate, std::vector{1.0}));
    EXPECT_FALSE(motrack::science::makeOdeSolver(OdeSolverKind::Rk4, OdeRate{}, std::vector{1.0}));
}

TEST(OdeSolverApi, ConstructionMatchesJavaDefaults)
{
    for (const auto kind : motrack::science::ALL_ODE_SOLVER_KINDS)
    {
        SCOPED_TRACE(std::string(motrack::science::odeSolverClassName(kind)));
        auto solver = makeSolver(kind, "oscillator");
        const bool embedded = kind == OdeSolverKind::CashKarp45 || kind == OdeSolverKind::DormandPrince45 || kind == OdeSolverKind::Rk45;
        EXPECT_EQ(solver->stepSize(), embedded ? 0.01 : 0.1);
        EXPECT_EQ(solver->ospErrorCode(), motrack::science::OSP_NO_ERROR);
        EXPECT_EQ(solver->convergenceFailure(), OdeConvergenceFailure::None);
        // Only LeapFrog evaluates rates during construction (its backward Verlet start).
        EXPECT_EQ(solver->rateEvaluations(), kind == OdeSolverKind::LeapFrog ? 2u : 0u);
        if (motrack::science::isAdaptiveOdeSolver(kind))
        {
            EXPECT_EQ(solver->tolerance(), 1e-6);
        }
        else
        {
            EXPECT_FALSE(solver->tolerance());
            EXPECT_FALSE(solver->setTolerance(1e-3));
            EXPECT_FALSE(solver->tolerance());
        }
    }
}

TEST(OdeSolverApi, RateEvaluationsFollowEachMethodsStageCount)
{
    struct Count
    {
        OdeSolverKind kind;
        std::uint64_t perStep;
    };
    for (const auto& [kind, perStep] : {Count{.kind = OdeSolverKind::Euler, .perStep = 1},
                                        Count{.kind = OdeSolverKind::EulerRichardson, .perStep = 2},
                                        Count{.kind = OdeSolverKind::Ralston2, .perStep = 2},
                                        Count{.kind = OdeSolverKind::Heun3, .perStep = 3},
                                        Count{.kind = OdeSolverKind::Rk4, .perStep = 4},
                                        Count{.kind = OdeSolverKind::Butcher5, .perStep = 6},
                                        Count{.kind = OdeSolverKind::Fehlberg8, .perStep = 13},
                                        Count{.kind = OdeSolverKind::Verlet, .perStep = 2}})
    {
        auto solver = makeSolver(kind, "oscillator");
        solver->initialize(0.01);
        solver->advance(3);
        EXPECT_EQ(solver->rateEvaluations(), 3 * perStep) << motrack::science::odeSolverClassName(kind);
    }
    // Adams methods: one history rate plus the starter's stages, then predictor-corrector (2).
    for (const auto& [kind, starting, starterRates] :
         {std::tuple{OdeSolverKind::Adams4, 3, 4}, std::tuple{OdeSolverKind::Adams5, 4, 6}, std::tuple{OdeSolverKind::Adams6, 5, 13}})
    {
        auto solver = makeSolver(kind, "oscillator");
        solver->initialize(0.01);
        solver->advance(static_cast<std::size_t>(starting));
        EXPECT_EQ(solver->rateEvaluations(), static_cast<std::uint64_t>(starting * (1 + starterRates)));
        solver->advance(2);
        EXPECT_EQ(solver->rateEvaluations(), static_cast<std::uint64_t>(starting * (1 + starterRates) + 4));
    }
    // LeapFrog: one rate per step, plus two for every (re)initialization including setStepSize.
    auto leapFrog = makeSolver(OdeSolverKind::LeapFrog, "oscillator");
    leapFrog->initialize(0.01);
    leapFrog->advance(3);
    EXPECT_EQ(leapFrog->rateEvaluations(), 7u);
    leapFrog->setStepSize(0.02);
    EXPECT_EQ(leapFrog->stepSize(), 0.02);
    EXPECT_EQ(leapFrog->rateEvaluations(), 9u);
}

TEST(OdeSolverApi, StateWritesKeepHistoryAndResizingReinitializes)
{
    auto solver = makeSolver(OdeSolverKind::Adams4, "oscillator");
    solver->initialize(0.01);
    solver->advance(3);
    // Same-length values, like Tracker's resetState/restoreState, keep the predictor history:
    // the next step is a predictor-corrector step (two rates), not an RK4 starting step (five).
    auto before = solver->rateEvaluations();
    solver->setState(std::vector{1.0, 0.0, 0.0});
    solver->state()[0] = 0.5;
    solver->step();
    EXPECT_EQ(solver->rateEvaluations() - before, 2u);
    EXPECT_NEAR(solver->state()[2], 0.01, 1e-15);

    // A different length is a new system: the solver reinitializes with its step size and the
    // starter runs again. The oscillator rate leaves the two extra rates at their zero scratch.
    solver->setState(std::vector{1.0, 0.0, 0.0, 7.0, 8.0});
    EXPECT_EQ(solver->stepSize(), 0.01);
    before = solver->rateEvaluations();
    solver->step();
    EXPECT_EQ(solver->rateEvaluations() - before, 5u);
    const auto& constSolver = *solver;
    ASSERT_EQ(constSolver.state().size(), 5u);
    EXPECT_EQ(constSolver.state()[3], 7.0);
    EXPECT_EQ(constSolver.state()[4], 8.0);

    // Every solver, including the multistep engine's private copy, follows a resized state.
    for (const auto kind : motrack::science::ALL_ODE_SOLVER_KINDS)
    {
        SCOPED_TRACE(std::string(motrack::science::odeSolverClassName(kind)));
        auto resized = makeSolver(kind, "oscillator");
        resized->initialize(0.1);
        resized->step();
        resized->setState(std::vector{1.0, 0.0, 0.0, 7.0, 8.0});
        const auto result = resized->step();
        EXPECT_EQ(result.status, OdeStepStatus::Ok);
        ASSERT_EQ(resized->state().size(), 5u);
        EXPECT_NEAR(resized->state()[2], result.stepTaken, 1e-12);
        if (kind != OdeSolverKind::LeapFrog)
        {
            // LeapFrog rederives every odd ("velocity") slot from the positions.
            EXPECT_EQ(resized->state()[3], 7.0);
        }
        resized->setState(std::vector{1.0, 0.0, 0.0});
        EXPECT_EQ(resized->step().status, OdeStepStatus::Ok);
        EXPECT_NEAR(resized->state()[0], oscillatorX(resized->state()[2]), 0.05);
    }
}

TEST(OdeSolverApi, ZeroNegativeAndNonFiniteStepsNeverHang)
{
    for (const auto kind : motrack::science::ALL_ODE_SOLVER_KINDS)
    {
        SCOPED_TRACE(std::string(motrack::science::odeSolverClassName(kind)));
        auto zero = makeSolver(kind, "oscillator");
        zero->initialize(0.0);
        const auto result = zero->step();
        EXPECT_EQ(result.stepTaken, 0.0);
        if (kind == OdeSolverKind::LeapFrog)
        {
            // The leapfrog velocity divides by 2h.
            EXPECT_EQ(result.status, OdeStepStatus::NonFinite);
        }
        else
        {
            EXPECT_EQ(result.status, OdeStepStatus::Ok);
            EXPECT_EQ(zero->state()[0], 1.0);
            EXPECT_EQ(zero->state()[2], 0.0);
        }

        auto backward = makeSolver(kind, "oscillator");
        backward->initialize(kind == OdeSolverKind::Multistep || kind == OdeSolverKind::Rk45MultiStep ? -0.1 : -0.01);
        backward->advance(10);
        EXPECT_LT(backward->state()[2], 0.0);
        EXPECT_NEAR(backward->state()[0], oscillatorX(backward->state()[2]), 0.05);

        auto notANumber = makeSolver(kind, "oscillator");
        notANumber->initialize(NOT_A_NUMBER);
        EXPECT_EQ(notANumber->step().status, OdeStepStatus::NonFinite);

        auto infinite = makeSolver(kind, "oscillator");
        infinite->initialize(INFINITE);
        EXPECT_EQ(infinite->step().status, OdeStepStatus::NonFinite);
    }
}

TEST(OdeSolverApi, EmptyAndUntimedStatesAreDefined)
{
    const OdeRate none = [](std::span<const double>, std::span<double>) {
    };
    for (const auto kind : motrack::science::ALL_ODE_SOLVER_KINDS)
    {
        SCOPED_TRACE(std::string(motrack::science::odeSolverClassName(kind)));
        auto solver = motrack::science::makeOdeSolver(kind, none, std::vector<double>{});
        ASSERT_TRUE(solver);
        solver->initialize(0.1);
        EXPECT_EQ(solver->advance(3).steps, 3u);
        EXPECT_TRUE(solver->state().empty());
        const auto target = solver->advanceTo(1.0);
        EXPECT_FALSE(target.reachedTarget);
        EXPECT_EQ(target.steps, 0u);
    }

    // An even-length Verlet state has no independent variable to advance.
    auto verlet = motrack::science::makeOdeSolver(
        OdeSolverKind::Verlet,
        [](std::span<const double> s, std::span<double> r)
        {
            r[0] = s[1];
            r[1] = -s[0];
        },
        std::vector{1.0, 0.0});
    verlet->initialize(0.1);
    verlet->step();
    EXPECT_DOUBLE_EQ(verlet->state()[0], 1.0 - 0.005);
    EXPECT_DOUBLE_EQ(verlet->state()[1], 0.1 * (-1.0 - 0.995) / 2.0);
}

TEST(OdeSolverApi, NanRatesAreReportedWithoutChangingOspResults)
{
    for (const auto kind : motrack::science::ALL_ODE_SOLVER_KINDS)
    {
        SCOPED_TRACE(std::string(motrack::science::odeSolverClassName(kind)));
        auto solver = makeSolver(kind, "nan-rate");
        solver->initialize(0.1);
        const auto result = solver->advance(5);
        EXPECT_EQ(result.status, OdeStepStatus::NonFinite);
        ASSERT_TRUE(result.firstFailure);
        // Adaptive and multistep solvers do not report DID_NOT_CONVERGE for NaN (as in OSP).
        EXPECT_EQ(solver->ospErrorCode(), motrack::science::OSP_NO_ERROR);
        EXPECT_TRUE(std::isnan(solver->state()[1]));
    }
}

TEST(OdeSolverApi, EmbeddedPairsReportRejectedStepsAndToleranceFloors)
{
    // A decay rate of 1e12 cannot be resolved in ten attempts from h = 1 (each shrinks at most 10x).
    const OdeRate decay = [](std::span<const double> s, std::span<double> r)
    {
        r[0] = -1e12 * s[0];
        r[1] = 1.0;
    };
    for (const auto kind : {OdeSolverKind::CashKarp45, OdeSolverKind::DormandPrince45})
    {
        SCOPED_TRACE(std::string(motrack::science::odeSolverClassName(kind)));
        auto solver = motrack::science::makeOdeSolver(kind, decay, std::vector{1.0, 0.0});
        solver->initialize(1.0);
        const auto result = solver->step();
        EXPECT_EQ(result.status, OdeStepStatus::DidNotConverge);
        EXPECT_EQ(solver->ospErrorCode(), motrack::science::OSP_DID_NOT_CONVERGE);
        EXPECT_EQ(solver->convergenceFailure(), OdeConvergenceFailure::StepRejected);
        // OSP still advances with the last attempted step: 1 shrunk nine times by 10.
        EXPECT_NEAR(result.stepTaken, 1e-9, 1e-15);
        EXPECT_EQ(solver->rateEvaluations(), 1u + 10u * 5u);
        // A resolvable step (h * lambda = -0.1) clears the failure.
        solver->setState(std::vector{1.0, 0.0});
        solver->setStepSize(1e-13);
        EXPECT_EQ(solver->step().status, OdeStepStatus::Ok);
        EXPECT_EQ(solver->convergenceFailure(), OdeConvergenceFailure::None);

        // Tolerances are stored as magnitudes; only Dormand-Prince raises them to 1e-12.
        EXPECT_TRUE(solver->setTolerance(-1e-8));
        EXPECT_EQ(solver->tolerance(), 1e-8);
        EXPECT_TRUE(solver->setTolerance(1e-14));
        EXPECT_EQ(solver->tolerance(), kind == OdeSolverKind::DormandPrince45 ? 1e-12 : 1e-14);
        solver->setStepSize(0.5);
        EXPECT_EQ(solver->stepSize(), 0.5);
    }
}

TEST(OdeSolverApi, MultistepWrappersReportWhyAnIntervalFailed)
{
    for (const auto kind : {OdeSolverKind::Multistep, OdeSolverKind::Rk45MultiStep})
    {
        SCOPED_TRACE(std::string(motrack::science::odeSolverClassName(kind)));
        // Stiff interval: more than 200 substeps.
        auto stiff = makeSolver(kind, "stiff");
        stiff->initialize(10.0);
        auto result = stiff->step();
        EXPECT_EQ(result.status, OdeStepStatus::DidNotConverge);
        EXPECT_EQ(stiff->convergenceFailure(), OdeConvergenceFailure::TooManySubsteps);
        EXPECT_GT(result.stepTaken, 0.0);
        EXPECT_LT(result.stepTaken, 10.0);
        EXPECT_EQ(stiff->stepSize(), 10.0);

        // An interval below Float.MIN_VALUE never changes the remainder by more than that.
        auto tiny = makeSolver(kind, "oscillator");
        tiny->initialize(1e-50);
        EXPECT_EQ(tiny->step().status, OdeStepStatus::DidNotConverge);
        EXPECT_EQ(tiny->convergenceFailure(), OdeConvergenceFailure::NoProgress);

        // A decay rate too fast for ten attempts makes the engine itself give up.
        const OdeRate decay = [](std::span<const double> s, std::span<double> r)
        {
            r[0] = -1e12 * s[0];
            r[1] = 1.0;
        };
        auto rejected = motrack::science::makeOdeSolver(kind, decay, std::vector{1.0, 0.0});
        rejected->initialize(1.0);
        EXPECT_EQ(rejected->step().status, OdeStepStatus::DidNotConverge);
        EXPECT_EQ(rejected->convergenceFailure(), OdeConvergenceFailure::SubstepRejected);

        // Backward into the same stiffness: OSP's ODEMultistepSolver.minus() would keep looping
        // after this failure; both wrappers now stop at the first one.
        const OdeRate growth = [](std::span<const double> s, std::span<double> r)
        {
            r[0] = 1e12 * s[0];
            r[1] = 1.0;
        };
        auto backward = motrack::science::makeOdeSolver(kind, growth, std::vector{1.0, 0.0});
        backward->initialize(-1.0);
        result = backward->step();
        EXPECT_EQ(result.status, OdeStepStatus::DidNotConverge);
        EXPECT_EQ(backward->convergenceFailure(), OdeConvergenceFailure::SubstepRejected);
        EXPECT_LT(result.stepTaken, 0.0);
        EXPECT_GT(result.stepTaken, -1.0);

        // The step after a failure starts cleanly.
        auto recovered = makeSolver(kind, "oscillator");
        recovered->initialize(0.1);
        EXPECT_EQ(recovered->step().status, OdeStepStatus::Ok);
        EXPECT_EQ(recovered->convergenceFailure(), OdeConvergenceFailure::None);

        // setStepSize changes the fixed interval only.
        recovered->setStepSize(0.05);
        EXPECT_EQ(recovered->stepSize(), 0.05);
        EXPECT_NEAR(recovered->step().stepTaken, 0.05, 1e-12);
        recovered->setStepSize(-0.05);
        EXPECT_NEAR(recovered->step().stepTaken, -0.05, 1e-12);
        EXPECT_NEAR(recovered->state()[2], 0.1, 1e-12);
        EXPECT_TRUE(recovered->setTolerance(-1e-9));
        EXPECT_EQ(recovered->tolerance(), 1e-9);
    }
}

TEST(OdeSolverApi, MultistepStopsWhenTheEngineStepFallsBelowTheToleranceFloor)
{
    // A fixed interval of 1 with tolerance 1e-6 requires engine steps above 1e-7. Rates of order
    // 1e8 force the stable step below that floor after a successful (non-rejected) substep.
    const OdeRate stiff = [](std::span<const double> s, std::span<double> r)
    {
        r[0] = -1e8 * (s[0] - std::cos(s[1]));
        r[1] = 1.0;
    };
    for (const auto kind : {OdeSolverKind::Multistep, OdeSolverKind::Rk45MultiStep})
    {
        SCOPED_TRACE(std::string(motrack::science::odeSolverClassName(kind)));
        auto solver = motrack::science::makeOdeSolver(kind, stiff, std::vector{1.0, 0.0});
        solver->initialize(1.0);
        solver->setStepSize(1.0);
        const auto result = solver->step();
        EXPECT_EQ(result.status, OdeStepStatus::DidNotConverge);
        EXPECT_EQ(solver->convergenceFailure(), OdeConvergenceFailure::StepTooSmall);
    }
}

TEST(OdeSolverApi, AdvanceToTakesWholeStepsUntilTheTargetTime)
{
    auto solver = makeSolver(OdeSolverKind::Rk4, "oscillator");
    solver->initialize(0.01);
    auto result = solver->advanceTo(1.0);
    EXPECT_TRUE(result.reachedTarget);
    EXPECT_EQ(result.steps, 100u);
    EXPECT_EQ(result.status, OdeStepStatus::Ok);
    EXPECT_FALSE(result.firstFailure);
    EXPECT_NEAR(solver->state()[2], 1.0, 1e-12);

    // Already there, behind, or unreachable.
    EXPECT_EQ(solver->advanceTo(1.004).steps, 0u);
    result = solver->advanceTo(0.5);
    EXPECT_EQ(result.steps, 0u);
    EXPECT_FALSE(result.reachedTarget);
    EXPECT_FALSE(solver->advanceTo(NOT_A_NUMBER).reachedTarget);
    result = solver->advanceTo(2.0, 10);
    EXPECT_EQ(result.steps, 10u);
    EXPECT_FALSE(result.reachedTarget);

    // Backward with a negative step.
    solver->setStepSize(-0.01);
    result = solver->advanceTo(0.0);
    EXPECT_TRUE(result.reachedTarget);
    EXPECT_NEAR(solver->state()[2], 0.0, 1e-12);
    EXPECT_NEAR(solver->state()[0], 1.0, 1e-8);

    // A zero step, or a state whose last component is not time, makes no progress.
    solver->setStepSize(0.0);
    EXPECT_FALSE(solver->advanceTo(1.0).reachedTarget);
    auto untimed = motrack::science::makeOdeSolver(
        OdeSolverKind::Euler, [](std::span<const double>, std::span<double> r) { r[0] = 0.0; }, std::vector{0.0});
    untimed->initialize(0.1);
    result = untimed->advanceTo(1.0);
    EXPECT_EQ(result.steps, 1u);
    EXPECT_FALSE(result.reachedTarget);

    // Failures are reported with the index of the first failing step.
    auto unstable = makeSolver(OdeSolverKind::Euler, "stiff");
    unstable->initialize(0.1);
    result = unstable->advanceTo(40.0);
    EXPECT_EQ(result.status, OdeStepStatus::NonFinite);
    ASSERT_TRUE(result.firstFailure);
    EXPECT_LT(*result.firstFailure, result.steps);

    // Multistep wrappers land on every interval exactly.
    auto multistep = makeSolver(OdeSolverKind::Multistep, "oscillator");
    multistep->initialize(0.1);
    result = multistep->advanceTo(1.0);
    EXPECT_EQ(result.steps, 10u);
    EXPECT_NEAR(multistep->state()[2], 1.0, 1e-12);
}

TEST(OdeSolverApi, JavaMinAndMaxKeepNanAndSignedZeroSemantics)
{
    using motrack::science::detail::javaMax;
    using motrack::science::detail::javaMin;
    EXPECT_TRUE(std::isnan(javaMax(NOT_A_NUMBER, 1.0)));
    EXPECT_TRUE(std::isnan(javaMax(1.0, NOT_A_NUMBER)));
    EXPECT_TRUE(std::isnan(javaMin(NOT_A_NUMBER, 1.0)));
    EXPECT_TRUE(std::isnan(javaMin(1.0, NOT_A_NUMBER)));
    EXPECT_FALSE(std::signbit(javaMax(-0.0, 0.0)));
    EXPECT_FALSE(std::signbit(javaMax(0.0, -0.0)));
    EXPECT_TRUE(std::signbit(javaMin(0.0, -0.0)));
    EXPECT_TRUE(std::signbit(javaMin(-0.0, 0.0)));
    EXPECT_EQ(javaMax(2.0, 3.0), 3.0);
    EXPECT_EQ(javaMin(2.0, 3.0), 2.0);
    EXPECT_EQ(motrack::science::detail::FLOAT_MIN_VALUE, 1.401298464324817e-45);
}

} // namespace
