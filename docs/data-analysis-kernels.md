# Data analysis kernels

`motrack_analysis` (`src/science/analysis/`, namespace `motrack::science`) ports the analysis that
Tracker exposes through OSP's Data Tool: the statistics table and its correlation status, the
Coordinates, Slope and Area measuring tools, and the Fourier Spectrum panel. It is a Qt-free kernel
(slice K5 of the [implementation order](migration-plan.md#implementation-order-and-exit-gates));
the Data Tool itself, its tables, plots and saved state arrive with slice I4. Fits and their
uncertainties are slice K3.

The reference is OSP commit `d56207b0`, the source of the pinned Tracker 6.3.5 JAR. On 2026-11-21
the OSP `swingJS` tip (`d49c6ecf`) had no changes to `DataToolStatsTable`, `FourierPanel`,
`FourierSinCosAnalysis`, `FFT`, `FFTReal`, `Dataset` or `DatasetManager`; its `DataTool`,
`DataToolTab`, `DataToolTable` and `DatasetCurveFitter` changes are user-interface and number-field
refactoring only.

## What Tracker exposes

| Data Tool feature | OSP code | Kernel |
| --- | --- | --- |
| Statistics table: max, min, mean, sd, se, n per column | `DataToolStatsTable.getStatistics` | `columnStatistics` |
| Status bar with statistics shown: "Correlation coefficient (R-squared)" | `DatasetCurveFitter.doLinearRegression` | `squaredCorrelation` |
| Points under the mouse, plotted and measured data | `findNearestXIndex`, `Dataset.append`, `getSelectedData` | `nearestPointIndex`, `plotData` |
| Measure > Coordinates and Slope, on data or on a fit | `DataToolPlotter.refreshMeasurements` | `pointCoordinates`, `pointSlope`, `functionSlope` |
| Measure > Area, on data or on a fit | `DataToolPlotter.refreshArea` | `dataArea`, `functionArea` |
| Analyze > Fourier Spectrum | `FourierPanel`, `FourierSinCosAnalysis`, `FFTReal`, `FFT` | `fourierSpectrum`, `realFft`, `complexFft` |

A tab saves only whether statistics are visible (`stats_visible`) and its origin shift; measuring
tools, limits and the Fourier panel are not saved. `numerics.Integral` and `analysis.FourierAnalysis`
are not referenced by any class in the JAR, and `FourierSinCosAnalysis.repeatAnalysis` is unused by
the Data Tool, so none of them is ported.

## Input data

Every function takes values in the columns' own world units, as displayed (after any origin shift).
Statistics read a column; the plot tools and the spectrum read points of an x and a y column.

- **Missing values.** NaN is a missing value. Statistics skip it; a point with a NaN y is a gap that
  the plot tools step over and that makes a spectrum undefined. Results that OSP shows as NaN or a
  blank cell are empty optionals or a status here; no missing value becomes zero.
- **Plot points.** `plotData` keeps the rows that OSP's `Dataset.append` keeps: a finite x and a y
  that is not infinite, in row order. Data Tool measures all rows (the working dataset) and gives the
  Fourier panel and fits the selected rows (`getSelectedData`: rows selected in the x or y column, or
  all rows). A row past the end of a column is a missing value.
- **Selections.** `columnStatistics(values, rows)` and `plotData(x, y, rows)` use the given rows in
  order. The statistics table uses the selected rows only for columns that are selected; that rule
  belongs to I4.

## Statistics

For the n values that are not NaN, summed from the last row to the first as OSP does:

- max and min as `Math.max`/`Math.min` (so −0 < +0); infinities count as values;
- mean = sum / n, bit for bit as OSP;
- sd = sqrt(sum (d − mean)^2 / (n − 1)), the sample standard deviation;
- se = sd / sqrt(n), the standard error of the mean.

Fewer than one value leaves max, min and mean empty; fewer than two values or an infinite value
leaves sd and se empty, and a mean of inf − inf is empty. OSP computes
sd = sqrt((sum d^2 − n mean^2) / (n − 1)) in one pass, which cancels catastrophically: seven times
0.1 gives 1.5e-9, 1e8 + {0, 1, 2} gives 0 instead of 1, 1e9 + noise gives 0, values near 1e200
give NaN and near 1e-200 give 0. MoTrack uses the corrected two-pass sum
(sum e^2 − (sum e)^2 / n) / (n − 1) with e = d − mean, scales every deviation by one power of two
(exact) so squares cannot overflow or underflow, and returns 0 when all values are equal. A sum that
overflows for finite values is recomputed with the same scaling (OSP reports an infinite mean).
OSP starts max and min from ∓`Double.MAX_VALUE` and reports NaN if that sentinel survives, so a
column of −infinity or −`Double.MAX_VALUE` has a NaN maximum; MoTrack reports the value. An sd
is infinite only when the true value exceeds the largest double (MAX and −MAX give √2·MAX); OSP
gives infinity whenever the squares overflow. Subnormal values keep their sd (1, 2, 3 × 2^-1074
gives 2^-1074; OSP gives 0). See [MoTrack #225](https://github.com/mgradwohl/MoTrack/issues/225).

For the quiet correction indicator, `ColumnStatistics::tracker` holds what Tracker shows, bit for
bit (OSP's sums, its one-pass sd and its sentinels, empty where Tracker shows NaN), and
`correctedFromTracker` is true when any statistic is present in only one of them or differs by
more than `TRACKER_DIFFERENCE_TOLERANCE` (10^-6 relative): far above the rounding differences of
well-conditioned data (below 10^-12) and below the four significant digits of Tracker's table.

The correlation status is `doLinearRegression` on the selected points with a y value:
r^2 = cov(x, y)^2 / (var x var y) with population (1/n) moments from a two-pass sum. It is empty
with fewer than three points, without x or y variation, or when it overflows. It is dimensionless.

## Measuring tools

- **Nearest point.** The plot x is clamped to the visible range; the nearest visible point with a
  y value wins, the first on ties, or the nearest point overall when none is visible. OSP takes the
  nearest point overall and moves an off-screen one to the next index (left of the view) or to the
  last index (right of it), assuming ascending x, so it can measure a point far outside the view: x
  = 0 … 5 with the view [0, 2.6] and the cursor at 2.6 measures x = 5 instead of 2
  ([MoTrack #267](https://github.com/mgradwohl/MoTrack/issues/267); OSP's earlier sorted version,
  still commented out in `DataToolTab`, chose the visible neighbour). When no point has a y value OSP
  throws `ArrayIndexOutOfBoundsException` from the mouse handler; `nearestPointIndex` is empty
  ([MoTrack #231](https://github.com/mgradwohl/MoTrack/issues/231)).
- **Coordinates.** The point itself, if it has a y value. OSP keeps showing the previous point over a
  gap; I4 may do the same.
- **Slope on data.** (y[i+1] − y[i−1]) / (x[i+1] − x[i−1]), the chord through the neighbours, also
  for unequal spacing. Undefined at the ends, at a gap or next to one; equal neighbour x gives an
  infinite slope. Unit y/x.
- **Slope on a fit.** (f(x + dx) − f(x − dx)) / (2 dx) with dx one screen pixel in x units.
- **Area on data.** The trapezoidal rule over the points between the two limits (either order,
  widened by 1/200000 of their distance so that a point on a limit counts) that have a y value, in
  row order: area = (y0 (x1 − x0) + y_{n−1} (x_{n−1} − x_{n−2}) + sum y_i (x_{i+1} − x_{i−1})) / 2,
  bit for bit as OSP. Gaps are bridged by straight lines; points that go back in x subtract, so a
  path gives the signed integral of y dx. Data Tool draws the limit lines at the first and last x
  used (`firstX`, `lastX`). Equal limits give exactly 0, a zero-width integral, as in OSP. Unit y·x.
- **Area on a fit.** The trapezoidal rule with `intervals` equal intervals between the limits,
  bridging samples where the function is NaN. Data Tool uses the number of screen pixels between the
  limits. OSP takes that many samples from the widened lower limit and so omits the last interval,
  missing about f(upper) (upper − lower) / intervals (0.4% at 400 pixels). Zero intervals, a
  non-finite limit or fewer than two defined samples give no area. OSP also shows 0 for an area
  with fewer than two points between distinct limits, where MoTrack reports none
  ([MoTrack #228](https://github.com/mgradwohl/MoTrack/issues/228)).

## Fourier spectrum

`fourierSpectrum` follows `FourierPanel.createFourierData`, `FourierSinCosAnalysis.doAnalysis` with
no gutter and `getDatasets`:

1. Fewer than two points give no spectrum (`TooFewPoints`). An odd count drops the last point.
2. The n points are treated as uniformly spaced from x0 = x[0] with the spacing
   delta = (x[n−1] − x[0] + x[1] − x[0]) / n, which is x[1] − x[0] for uniform data. Unequal
   spacing is not corrected; `maximumSpacingError` reports max |x[i+1] − x[i] − delta| / |delta|.
   It is not exactly 0 for frame times t0 + i dt (about 1.5e-15 for dt = 0.1): I4 should warn above
   `UNIFORM_SPACING_TOLERANCE` (10^-6, while one microsecond of jitter at 25 frames/s is 2.5e-5). A zero or non-finite delta is `InvalidSpacing` (OSP lists infinite
   frequencies with NaN coefficients). Descending x gives negative frequencies, as in OSP.
3. The y values are transformed with `realFft` (no window, no padding, no detrending), giving
   X_k = sum_j y_j exp(−2 pi i j k / n).
4. Row k, k = 0..n/2, has frequency f_k = k / (n delta) in cycles per x unit (Hz for seconds),
   accumulated by repeated addition as OSP does, and angular frequency w_k = 2 pi f_k (radians per x
   unit). The coefficients describe y(x) = sum_k a_k cos(w_k x) + b_k sin(w_k x) relative to x = 0,
   not to x0: with c = (2/n) cos(w_k x0) and s = (2/n) sin(w_k x0),
   a_k = Re X_k c + Im X_k s and b_k = −Im X_k c + Re X_k s, both halved for k = 0 and k = n/2. Power
   is a_k^2 + b_k^2: the squared amplitude in y units squared, not a spectral density. a_0 is the mean
   and b_0 = 0. At the Nyquist frequency the samples determine only the amplitude A = X_{n/2}/n;
   a = A cos(w x0), b = A sin(w x0) is the smallest pair that reproduces them, a convention.
5. A missing y value makes every coefficient and power NaN, as in OSP, with the status
   `MissingValues`.

Deliberate differences:

- **Real transform.** `FFTReal.shuffle` unpacks the half-length complex transform with a loop that
  stops at n/4 (2 for n = 6). When n/2 is odd and n > 6 (n = 10, 14, 18, ...: half of all even
  lengths) it skips the middle pair, leaving bins (n/2 − 1)/2 and (n/2 + 1)/2 as raw intermediate
  values; a 30-point cosine at bin 7 shows power 0.30 there and 0.95 at bin 8. `realFft` runs the
  loop to (n/2 + 1)/2; all other outputs keep OSP's operations
  ([MoTrack #226](https://github.com/mgradwohl/MoTrack/issues/226)).
- **Nyquist row.** OSP lists rows k = 0..n/2 − 1 only and reads the packed X_{n/2} as the imaginary
  part of X_0, so the zero-frequency sine is −X_{n/2}/n and its power includes the Nyquist energy.
  MoTrack reports b_0 = 0 and a Nyquist row
  ([MoTrack #227](https://github.com/mgradwohl/MoTrack/issues/227)). OSP's `createFourierData` also
  drops the last point of an odd count from the selected dataset it shares with the curve fitter, so
  later fits silently use one point fewer while the spectrum is open; the kernel does not modify its
  input ([MoTrack #230](https://github.com/mgradwohl/MoTrack/issues/230)).

Both differences follow from the result, which is all the correction indicator needs: Tracker has
no Nyquist row and its 0 Hz sine and power differ whenever that row's power is nonzero; its bins
(n/2 − 1)/2 and (n/2 + 1)/2 are wrong whenever `pointCount` / 2 is odd and `pointCount` ≥ 10.

`complexFft` ports `FFT.transform`, GSL's mixed-radix algorithm with passes for 2, 3, 4, 5, 6 and 7
and a general pass for other primes, bit for bit apart from the twiddle factors.

## Units

The kernel returns plain doubles; `UnitPowers{y, x}` states each result's unit as a power of the
plotted columns' units, which I4 formats.

| Result | Unit |
| --- | --- |
| Statistics except n | the column's unit |
| n, r^2 | dimensionless |
| Coordinates | the x and y units |
| Slope (`SLOPE_UNIT`) | y / x |
| Area (`AREA_UNIT`) | y · x |
| Frequency (`FREQUENCY_UNIT`) | cycles per x unit (Hz when x is in seconds); angular frequency in rad per x unit |
| Cosine and sine coefficients (`FOURIER_COEFFICIENT_UNIT`) | y |
| Power (`FOURIER_POWER_UNIT`) | y² |

## Reference defects outside the kernel

`Dataset.append(double[], double[], int)` compacts the rows it rejects but keeps its old point count,
so the last points repeat once per rejected row (a stale NaN-x row stays at the end). Data Tool's
working and selected datasets inherit this for rows without an x, which adds repeated points to
measurements, spectra and fits. `DataColumn.setPoints` goes through the same code, so a column with
an infinite value loses it, moves every later value up one row (misaligning it with the other columns)
and repeats its last value. `plotData` keeps rows aligned and never repeats a point; the column
model (I4/C2) must not reproduce the shift
([MoTrack #230](https://github.com/mgradwohl/MoTrack/issues/230)).

## Floating point and the Java oracle

Each expression keeps Java's evaluation order and uses `Math.max`/`Math.min` from the shared`nmotrack_java_math` library (`science/java_math.hpp`, also used by K1 and K2; #263 tracks moving the
other kernels' Java helpers there). Java never fuses multiply-add, so the library and its reference
test are compiled with `MOTRACK_JAVA_FP_OPTIONS` (`-ffp-contract=off`).
Max, min, mean, n and the `tracker` statistics, plot data, correlation, nearest points (apart from
#267), coordinates, slopes, data areas and frequencies are therefore identical to Java; sd and se
are corrected (#225) and compared within OSP's rounding bound below. FFT outputs and Fourier coefficients also use `Math.sin`/`Math.cos`
(twiddle and phase factors), whose last bit differs between HotSpot's x86-64 intrinsics, its arm64
path and C++ libraries.

[`tools/reference/AnalysisOracle.java`](../tools/reference/AnalysisOracle.java) runs the pinned
classes headless. The table and plot code lives in Swing classes whose constructors need a display,
so the oracle allocates the owning objects without constructors, sets only the fields the
computation reads, and calls the real `getStatistics`, `refreshArea`, `refreshMeasurements`,
`findNearestXIndex`, `doLinearRegression`, `createFourierData`/`getDatasets`, `FFTReal` and `FFT`.
Columns, working and selected datasets are built by `DataColumn.setPoints`,
`WorkingDataset.setXSource` and `Dataset.append`; only the statistics table's and `getSelectedData`'s
row gathering (plain array copies of a JTable selection) is restated. Fit-mode cases use
`UncertainFunctionDrawer` with a `KnownPolynomial` and a pixel transform set on the plot.
Inputs come from a 64-bit LCG and `StrictMath` (fdlibm on every JVM): 29 columns (textbook,
gaps, frame times, noise, constants, large offsets, extremes, ±MAX, subnormals, infinities) and 32
point series
(parabola, line, unequal spacing, gaps in x and y, an infinity, a reversing path, repeated x, tiny
sizes, sinusoids at known bins with phase origins, Nyquist terms, power-of-two and other lengths
including radix-7 and prime factors, degenerate spacing). The 336 cases cover statistics, plot data,
correlation, 23 area cases (two with equal limits) and 20 nearest-point cases (two at the view
edge), coordinates and slope at every index, fit-mode area
and slope, 27 spectra, 23 real and 25 complex transforms, and column storage.
[`tests/science/analysis-reference.json`](../tests/science/analysis-reference.json) stores them so
`motrack_tests` needs no Java. `python tools/corpus.py analysis-oracle --java <java>` regenerates
it in Linux CI and compares; `--record` replaces it for review.

Tolerances: all values are compared exactly, except FFT outputs, Fourier coefficients and power.
Those allow 16 × (1 + log2 n) units of 2^-52 times the sum of input magnitudes (times 2/n for
coefficients; power allows (4 scale + 2 t) t for coefficient tolerance t and scale (2/n) sum |y|).
A last-bit twiddle difference perturbs each of the log2 n butterfly stages by at most a few units of
2^-52 of the input scale; a wrong twiddle, sign or branch shows at the size of the input. The C++
test compares statistics' sd and se with OSP within the one-pass rounding bound
4 (n + 4) u ((sum d^2 + n mean^2) / ((n − 1) sd) + sd), u = 2^-53 (Chan, Golub and LeVeque 1983),
which is exact enough to catch a wrong formula yet admits OSP's cancellation. The fixture was
recorded on Windows x64 with Temurin 17.0.21; there every exact value of the C++ port matched and
every transform value was within its tolerance.

For every documented difference the reference test asserts both the corrected value and that the
Java value still deviates. Independent tests guard against a consistently wrong port: textbook
statistics, exact constant and offset deviations, the analytic trapezoidal error of x^2, exact
integrals of lines and polynomials within the trapezoidal bound, the signed semicircle path integral,
every complex length 1..64 and selected lengths to 1024 and every even real length to 130 against
the O(n^2) definition, Parseval's theorem, and spectra of signals with known amplitudes, phases and
Nyquist terms reconstructed at every sample.

## Using the kernel from I4

The Data Tool view builds `plotData` from its x and y columns (all rows for the measuring tools,
the selection for the spectrum and fits), passes the visible x range and the pixel scale for
`nearestPointIndex`, `functionSlope` and `functionArea`, and formats results with the column units
and `UnitPowers`. It shows an empty result as blank or "undefined" rather than 0, reports
`maximumSpacingError` (above `UNIFORM_SPACING_TOLERANCE`) and the Fourier status to the user,
names the applicable corrections in a quiet indicator (`correctedFromTracker`, and the Fourier and
area rules above), and keeps the Tracker workflow of a statistics row per column with the
correlation in the status bar. `squaredCorrelation` is the shared R² for K3 and the existing
position-plot fit ([MoTrack #268](https://github.com/mgradwohl/MoTrack/issues/268)). All functions are pure; I4 runs
them on immutable snapshots, off the GUI thread for large spectra.
