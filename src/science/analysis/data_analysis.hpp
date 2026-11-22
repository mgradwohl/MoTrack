#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

/// Qt-free ports of the analysis that Tracker exposes through OSP's Data Tool (OSP d56207b0, the
/// source of the Tracker 6.3.5 JAR): the statistics table and its correlation status, the
/// Coordinates/Slope/Area measuring tools and the Fourier Spectrum panel. See
/// docs/data-analysis-kernels.md for the formulas, the reference classes and every deliberate
/// difference. Undefined results (OSP shows NaN or a blank cell) are empty optionals or a status;
/// a missing value is never replaced by zero. All functions are pure and thread-safe.
namespace motrack::science
{

/// Exponents of the plotted columns' units in a derived quantity: unit = (y unit)^y (x unit)^x.
/// Data Tool columns carry no unit system, so consumers format these with the column units.
struct UnitPowers
{
    int y = 0;
    int x = 0;

    friend bool operator==(const UnitPowers&, const UnitPowers&) = default;
};

/// One plotted point in the columns' own (world) units. A NaN y is a gap (a missing value).
struct PlotPoint
{
    double x;
    double y;

    friend bool operator==(const PlotPoint&, const PlotPoint&) = default;
};

/// The points that OSP's `Dataset.append` keeps for an x/y column pair, in row order.
struct PlotData
{
    std::vector<PlotPoint> points;
    std::vector<std::size_t> rows; ///< source row of each point
    std::size_t excludedRows = 0;  ///< rows without a finite x or with an infinite y
};

/// Pairs the rows of an x and a y column like Data Tool's working and selected datasets.
///
/// Rows without a finite x, or with an infinite y, are excluded; a NaN y is kept as a gap. Row i
/// pairs x[i] with y[i]; rows past the end of `y` have a missing y and rows past `x` are omitted.
/// Deliberate difference: OSP's `Dataset.append(double[], double[], int)` compacts excluded rows
/// but keeps its old point count, so it repeats the last points once per excluded row.
[[nodiscard]] PlotData plotData(std::span<const double> x, std::span<const double> y);

/// The selected rows, in the given order (Data Tool passes ascending rows). A row past the end of
/// a column has a missing value there, as in `DataToolTable.getSelectedData`.
[[nodiscard]] PlotData plotData(std::span<const double> x, std::span<const double> y, std::span<const std::size_t> rows);

/// One column of Data Tool's statistics table (`DataToolStatsTable.getStatistics`).
///
/// NaN values are missing and ignored; infinities are values. Every statistic has the column's
/// unit except the count. The sample standard deviation divides by n - 1 and the standard error is
/// sd / sqrt(n). Deliberate difference: OSP evaluates sqrt((sum d^2 - n mean^2) / (n - 1)), which
/// cancels catastrophically (seven values of 0.1 give 1.5e-9; 1e8 + {0, 1, 2} gives 0, not 1) and
/// overflows or underflows beyond about 1e154 and 1e-154 (#225). MoTrack uses a corrected two-pass sum
/// with exact power-of-two scaling, and reports Tracker's values in `tracker` for an indicator. A column whose values are all
/// -Double.MAX_VALUE or -infinity (or the positive equivalents for the minimum) has an OSP maximum of NaN; MoTrack reports the value. What
/// Tracker 6.3.5 shows for the same values, bit for bit: OSP's one-pass sd and se, a sum that may overflow, and empty (NaN in OSP) where a
/// -/+Double.MAX_VALUE sentinel survives.
struct TrackerColumnStatistics
{
    std::optional<double> maximum;
    std::optional<double> minimum;
    std::optional<double> mean;
    std::optional<double> standardDeviation;
    std::optional<double> standardError;
};

/// Relative difference above which a corrected statistic counts as differing from Tracker's: far
/// above the rounding differences of well-conditioned data (below 1e-12), and below the four
/// significant digits Tracker's table shows, so every visible correction is flagged.
inline constexpr double TRACKER_DIFFERENCE_TOLERANCE = 1e-6;

struct ColumnStatistics
{
    std::size_t count = 0;                   ///< values that are not NaN (OSP's "n")
    std::optional<double> maximum;           ///< empty without values
    std::optional<double> minimum;           ///< empty without values
    std::optional<double> mean;              ///< sum / n in OSP's order; empty if undefined (inf - inf)
    std::optional<double> standardDeviation; ///< empty with fewer than two values or an infinite value;
                                             ///< infinite only if the true value exceeds the largest double
    std::optional<double> standardError;     ///< empty whenever the standard deviation is
    TrackerColumnStatistics tracker;         ///< Tracker's values, for the correction indicator (#225)
    /// A statistic is present in only one of the two, or differs by more than
    /// TRACKER_DIFFERENCE_TOLERANCE relative: Tracker would show a different value here.
    bool correctedFromTracker = false;
};

/// Statistics of a whole column.
[[nodiscard]] ColumnStatistics columnStatistics(std::span<const double> values);

/// Statistics of the selected rows. A row past the end of the column is a missing value.
[[nodiscard]] ColumnStatistics columnStatistics(std::span<const double> values, std::span<const std::size_t> rows);

/// Data Tool's status-bar "correlation coefficient (R-squared)" from
/// `DatasetCurveFitter.doLinearRegression`: cov(x, y)^2 / (var x var y) over the points with a y
/// value. Empty with fewer than three such points, without x or y variation, or on overflow.
[[nodiscard]] std::optional<double> squaredCorrelation(std::span<const PlotPoint> points);

/// The point the Coordinates and Slope tools measure for a plot x (`DataToolTab.findNearestXIndex`).
///
/// `x` is clamped to the visible plot range [visibleMin, visibleMax]. The nearest visible point with
/// a y value wins (the first on ties), or the nearest point overall when none is visible. Empty when
/// no point has a y value or the inputs are NaN (OSP throws `ArrayIndexOutOfBoundsException`, #231).
/// Deliberate difference (#267): OSP takes the nearest point overall and moves an off-screen one to
/// the next or the last index, assuming ascending x, which can pick a point far outside the view.
[[nodiscard]] std::optional<std::size_t>
nearestPointIndex(std::span<const PlotPoint> points, double x, double visibleMin, double visibleMax);

/// Coordinates tool: the point, if it exists and has a y value.
[[nodiscard]] std::optional<PlotPoint> pointCoordinates(std::span<const PlotPoint> points, std::size_t index);

/// Slope tool on data: (y[i+1] - y[i-1]) / (x[i+1] - x[i-1]), the chord through the neighbours
/// (also for unequal spacing). Empty at the first and last index, when the point or a neighbour
/// has no y value, or when the result is NaN; neighbours with equal x give an infinite slope.
[[nodiscard]] std::optional<double> pointSlope(std::span<const PlotPoint> points, std::size_t index);

/// Slope tool on a fit: (f(x + dx) - f(x - dx)) / (2 dx). Data Tool uses one screen pixel as dx.
using ScalarFunction = std::function<double(double)>;
[[nodiscard]] std::optional<double> functionSlope(const ScalarFunction& function, double x, double dx);

inline constexpr UnitPowers SLOPE_UNIT{.y = 1, .x = -1};
inline constexpr UnitPowers AREA_UNIT{.y = 1, .x = 1};

/// Result of the Area tool.
struct AreaMeasurement
{
    std::optional<double> area;   ///< y-x units (AREA_UNIT); empty with fewer than two points
    std::size_t pointCount = 0;   ///< points used (inside the limits and with a y value)
    std::optional<double> firstX; ///< smallest x used: where Data Tool draws the lower limit line
    std::optional<double> lastX;  ///< largest x used: where Data Tool draws the upper limit line
};

/// Area tool on data (`DataToolPlotter.refreshArea`): the trapezoidal rule over the points whose x
/// lies between the two limits (in either order, widened by 1/200000 of their distance so points
/// on a limit are included) and whose y is present, in row order. Gaps are bridged by a straight
/// line; points that go back in x subtract, giving the signed path integral of y dx. Equal limits
/// give exactly 0 (a zero-width integral). Deliberate difference: OSP also reports 0 for fewer than
/// two points between distinct limits, where the area is empty here.
[[nodiscard]] AreaMeasurement dataArea(std::span<const PlotPoint> points, double firstLimit, double secondLimit);

/// Area tool on a fit: the trapezoidal rule with `intervals` equal intervals between the limits
/// (Data Tool uses the number of screen pixels between them), bridging samples where the function
/// is NaN. Zero intervals, a non-finite limit or fewer than two defined samples give an empty area;
/// equal limits give 0. Deliberate difference: OSP samples `intervals` points from the widened lower
/// limit and omits the last interval, so it misses about f(upper) (upper - lower) / intervals (#228).
[[nodiscard]] AreaMeasurement functionArea(const ScalarFunction& function, double firstLimit, double secondLimit, std::size_t intervals);

enum class FourierStatus : std::uint8_t
{
    Ok,
    TooFewPoints,   ///< fewer than two points; OSP shows no spectrum
    MissingValues,  ///< a point has no y value; every coefficient and power is NaN, as in OSP
    InvalidSpacing, ///< the assumed spacing is zero or not finite; OSP shows infinite frequencies
};

/// `maximumSpacingError` above which I4 should warn that the spacing is not uniform. Frame times
/// t0 + i dt round to about 1e-15 to 1e-12 of dt; one microsecond of jitter at 25 frames/s is 2.5e-5.
inline constexpr double UNIFORM_SPACING_TOLERANCE = 1e-6;

inline constexpr UnitPowers FREQUENCY_UNIT{.y = 0, .x = -1};
inline constexpr UnitPowers FOURIER_COEFFICIENT_UNIT{.y = 1, .x = 0};
inline constexpr UnitPowers FOURIER_POWER_UNIT{.y = 2, .x = 0};

/// Data Tool's Fourier Spectrum (`FourierPanel.createFourierData`, `FourierSinCosAnalysis`).
///
/// An odd count drops the last point. The n points are treated as uniformly spaced from x[0], with
/// the spacing (x[n-1] - x[0] + x[1] - x[0]) / n (x[1] - x[0] for uniform points); unequal spacing
/// is not corrected, only reported. Row k (k = 0..n/2) has frequency k / (n spacing) in cycles per
/// x unit (Hz for seconds), angular frequency 2 pi times that, and coefficients a_k, b_k of
/// y(x) = sum_k a_k cos(2 pi f_k x) + b_k sin(2 pi f_k x) relative to x = 0 (not to x[0]); power is
/// a_k^2 + b_k^2 (y unit squared). At the Nyquist frequency the samples fix only the amplitude
/// A = X_{n/2}/n; splitting it as a = A cos(w x[0]), b = A sin(w x[0]) (the smallest a, b that
/// reproduce the samples) is a convention. Deliberate differences: OSP stops at k = n/2 - 1 and puts
/// -X_{n/2}/n (the Nyquist term) in the k = 0 sine and power (#227); MoTrack reports b_0 = 0 and a
/// Nyquist row. MoTrack also uses the corrected real transform (#226, see realFft). For the
/// correction indicator both follow from the result: Tracker lacks the Nyquist row and its 0 Hz sine
/// and power differ whenever that row's power is nonzero, and its bins (n/2 - 1)/2 and (n/2 + 1)/2
/// are wrong whenever pointCount / 2 is odd and pointCount >= 10.
struct FourierSpectrum
{
    FourierStatus status = FourierStatus::TooFewPoints;
    std::size_t pointCount = 0;           ///< points transformed (even)
    bool droppedLastPoint = false;        ///< an odd count lost its last point, as in OSP
    double origin = 0.0;                  ///< x of the first point
    double spacing = 0.0;                 ///< the assumed uniform spacing (see above)
    double maximumSpacingError = 0.0;     ///< max |x[i+1] - x[i] - spacing| / |spacing| (UNIFORM_SPACING_TOLERANCE)
    std::vector<double> frequency;        ///< FREQUENCY_UNIT, cycles per x unit
    std::vector<double> angularFrequency; ///< radians per x unit
    std::vector<double> cosine;           ///< FOURIER_COEFFICIENT_UNIT
    std::vector<double> sine;             ///< FOURIER_COEFFICIENT_UNIT
    std::vector<double> power;            ///< FOURIER_POWER_UNIT
};

[[nodiscard]] FourierSpectrum fourierSpectrum(std::span<const PlotPoint> points);

} // namespace motrack::science
