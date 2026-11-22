#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace motrack::science
{

/// Curve fitting as Tracker 6.3.5's Data Tool performs it (OSP `DatasetCurveFitter`, OSP d56207b).
///
/// `Tracker` reproduces the reference exactly, including its defects; `Corrected`, the default, fixes the
/// reproduced defects listed in docs/curve-fitting.md and otherwise runs the same algorithms.
enum class FitDialect : std::uint8_t
{
    Tracker,
    Corrected,
};

/// Evaluates a fit function f(x; parameters), parameters in declaration order.
///
/// The callback runs synchronously on the fitting thread, many times per fit. It reports an undefined
/// value with NaN rather than throwing. Like OSP's `UserFunction`, the fitter treats a NaN value as 0 while
/// minimizing and as a "non-real value" when measuring the deviation. An exception propagates out of
/// `fitCurve`, which has no side effects. Slice I4 binds user-defined fits to the K1 expression engine by
/// compiling the expression over the variables {x, parameters...} and evaluating it here.
using FitModel = std::function<double(double x, std::span<const double> parameters)>;

/// The fits every Data Tool offers, in OSP's `DatasetCurveFitter.defaultFits` order.
enum class BuiltInFit : std::uint8_t
{
    Line,
    Parabola,
    Cubic,
    Gaussian,
    Exponential,
    Sinusoid,
    DampedSine,
    Power,
    Log,
};

inline constexpr std::array ALL_BUILT_IN_FITS = {
    BuiltInFit::Line,
    BuiltInFit::Parabola,
    BuiltInFit::Cubic,
    BuiltInFit::Gaussian,
    BuiltInFit::Exponential,
    BuiltInFit::Sinusoid,
    BuiltInFit::DampedSine,
    BuiltInFit::Power,
    BuiltInFit::Log,
};

/// The fit menu in an English locale: Line and Parabola first, then the rest sorted by display name,
/// ignoring case. Line is selected by default.
inline constexpr std::array TRACKER_FIT_MENU_ORDER = {
    BuiltInFit::Line,
    BuiltInFit::Parabola,
    BuiltInFit::Cubic,
    BuiltInFit::DampedSine,
    BuiltInFit::Exponential,
    BuiltInFit::Gaussian,
    BuiltInFit::Log,
    BuiltInFit::Power,
    BuiltInFit::Sinusoid,
};

struct FitParameter
{
    std::string name;
    double value = 0.0;
    /// Untranslated English description, as Tracker's tooltip shows it.
    std::string description;
};

/// A fit function and its current parameter values.
struct FitFunction
{
    /// OSP's function name: `Poly1`, `Poly2`, `Poly3`, `Gaussian`, `Exponential`, `Sinusoid`,
    /// `DampedSine`, `Power`, `Log`, or the user's name.
    std::string name;
    /// Untranslated English description.
    std::string description;
    /// The equation Tracker shows after "y =", in the independent variable x and without spaces.
    std::string expression;
    std::vector<FitParameter> parameters;
    /// Evaluates `expression` with SuryonoParser's operation order. For polynomials it is used only for
    /// fits with fixed parameters, which Tracker evaluates through the expression.
    FitModel model;
    /// Selects Tracker's from-scratch initial-guess heuristic.
    std::optional<BuiltInFit> builtIn;
    /// Above 0 for a `KnownPolynomial`: fitted in closed form unless a parameter is fixed, evaluated by
    /// Horner's rule, and unable to hold NaN parameter values.
    int polynomialDegree = 0;
};

/// The built-in fit with OSP's names, expression, parameter names, descriptions and default values.
[[nodiscard]] FitFunction builtInFit(BuiltInFit kind);

/// English display name in Tracker's menu: Line, Parabola, Cubic, Gaussian, Exponential, Sine,
/// Damped Sine, Power, Log.
[[nodiscard]] std::string_view builtInFitDisplayName(BuiltInFit kind) noexcept;

/// A `KnownPolynomial` of degree 1 to 5 with parameters A (highest power) to F, all zero. Nothing for
/// another degree. The built-in Line, Parabola and Cubic are degrees 1-3.
[[nodiscard]] std::optional<FitFunction> polynomialFit(int degree);

enum class FitStart : std::uint8_t
{
    /// Replace the free parameters with the built-in fit's initial guess (Tracker does this the first
    /// time a fit is selected and whenever Autofit is switched on).
    FromScratch,
    /// Start from the current parameter values (Tracker's refit after the data or a parameter changes).
    FromCurrent,
};

struct FitOptions
{
    FitDialect dialect = FitDialect::Corrected;
    /// Tracker's Autofit check box. Off, the parameters are only evaluated (a manual fit).
    bool autofit = true;
    FitStart start = FitStart::FromScratch;
    /// One flag per parameter (empty: none fixed). A fixed parameter keeps its current value.
    std::vector<bool> fixed;
    /// Values the initial guess keeps for parameters it does not estimate (only DampedSine's E). Tracker
    /// uses the values the fit had at its first from-scratch fit in the session (OSP shares them between
    /// Data Tool tabs); empty means the built-in defaults.
    std::vector<double> scratchFallback;
    /// Polled between minimizations; returning true stops the fit with `FitStatus::Cancelled`.
    std::function<bool()> cancelled;
};

enum class FitStatus : std::uint8_t
{
    /// Minimized (or solved in closed form) and kept.
    Fitted,
    /// Neither minimizer reduced the deviation, so the starting values were kept, as in Tracker.
    NotImproved,
    /// Autofit is off: the given parameters were evaluated.
    Manual,
    /// Every parameter is fixed (or there are none).
    NothingToFit,
    /// Fewer valid points than free parameters: the values are unchanged.
    TooFewPoints,
    /// The function is undefined (NaN) at a valid point for the starting values, so Tracker skips the fit.
    UndefinedAtStart,
    /// A polynomial without a unique fit. Corrected: fewer distinct x values than coefficients (the values
    /// are unchanged). Tracker: the normal equations were singular, keeping or giving non-finite values.
    DegenerateData,
    Cancelled,
    /// The arguments are inconsistent (lengths, missing model); nothing was computed.
    InvalidRequest,
};

/// The algorithm of the main fit (the uncertainty refits always minimize).
enum class FitMethod : std::uint8_t
{
    None,
    /// `PolynomialLeastSquareFit` normal equations (Corrected: scaled normal equations).
    ClosedForm,
    /// `HessianMinimize`, kept because it did not increase the deviation.
    Hessian,
    /// `LevenbergMarquardt` from the Hessian result; with `FitStatus::NotImproved` it failed too.
    LevenbergMarquardt,
};

enum class UncertaintyStatus : std::uint8_t
{
    Estimated,
    /// The parameter is fixed (Tracker's NaN).
    Fixed,
    /// The chi-squared curvature was not positive (Tracker's 0, shown without an uncertainty).
    Undetermined,
};

struct FitResult
{
    FitStatus status = FitStatus::InvalidRequest;
    FitMethod method = FitMethod::None;
    std::vector<double> parameters;
    /// Standard errors from the curvature of chi squared, one per parameter, when Tracker shows them:
    /// autofit on, more valid points than free parameters, and neither x nor y constant (three or more
    /// points). Empty otherwise. Tracker's raw values are kept: NaN when fixed, 0 when undetermined.
    std::vector<double> uncertainties;
    std::vector<UncertaintyStatus> uncertaintyStatus;
    /// Parameter sets whose curves, with the fit itself, bound the shaded uncertainty band: for each
    /// parameter with an uncertainty, its value minus and plus sigma. Tracker keeps drawing the band when
    /// the uncertainties are discarded for constant x or y.
    std::vector<std::vector<double>> uncertaintyBand;
    /// sqrt(sum of squared deviations / points), Tracker's "rms dev"; NaN without points or when autofit
    /// has more parameters than points.
    double rmsDeviation = std::numeric_limits<double>::quiet_NaN();
    /// Square of the Pearson correlation of the valid data, whatever the fit: Tracker's status-bar
    /// "Correlation coefficient (R-squared)". NaN for fewer than three points or constant x or y.
    double correlation = std::numeric_limits<double>::quiet_NaN();
    /// Valid points used: Tracker drops NaN y (and, through `Dataset.append`, NaN or infinite x and
    /// infinite y while repeating its last points); Corrected drops every point with a non-finite value.
    std::size_t points = 0;
    /// Tracker's "The function returns non-real values over this range" warning.
    bool nonRealValues = false;
};

/// Fits `function` to the paired columns `x` and `y` (equal lengths), as Tracker's Data Tool does.
/// Deterministic, single-threaded and allocation-bounded by the data size; no exceptions are thrown
/// except from the model callback.
[[nodiscard]] FitResult
fitCurve(const FitFunction& function, std::span<const double> x, std::span<const double> y, const FitOptions& options = {});

/// `DatasetCurveFitter.formatUncertainParameter`: "value PM sigma", PM being U+00B1 in UTF-8, with sigma rounded
/// to one significant digit (plus `extraPlaces`) and both scaled by a shared power of ten, e.g. "(7.6 PM 0.4) E1". Nothing when
/// sigma is NaN or not positive, or either value is not finite. Uses '.' as decimal separator; Tracker
/// substitutes the locale's.
[[nodiscard]] std::optional<std::string> formatUncertainParameter(double value, double sigma, int extraPlaces = 0);

/// Parameters for which Tracker computes a different fit from the same expression: Tracker fixes a
/// parameter, and estimates every uncertainty, by replacing the parameter's name in the expression text
/// with its value, so a name that also occurs inside another name or number is corrupted (for example `c`
/// in `cos`, or `E` once a fixed value is written as 1.0E-4). MoTrack substitutes values structurally, so
/// both dialects compute the intended fit; this lets editors warn when an opened experiment is affected.
[[nodiscard]] std::vector<std::string> trackerSubstitutionCollisions(const FitFunction& function, const std::vector<bool>& fixed = {});

} // namespace motrack::science
