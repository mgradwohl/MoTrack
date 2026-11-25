# Curve fitting

`motrack_fit` (`src/science/fit/curve_fit.hpp`, `minimizers.hpp`) reproduces the curve fitting of Tracker's
Data Tool: the built-in fits, user-defined fits, fixed parameters, Autofit, manual fits, parameter
uncertainties, the uncertainty band, rms deviation and the status-bar R². It is a Qt-free kernel (slice K3
of the [full-port plan](migration-plan.md)); the Data Tool and Fit Builder views, persistence and the
binding of user expressions belong to slice I4. Everything is a port of OSP's
`org.opensourcephysics.tools.DatasetCurveFitter` and the numerics it calls (`HessianMinimize`,
`LevenbergMarquardt`, `PolynomialLeastSquareFit`, `LUPDecomposition`) at OSP `d56207b`, the source of the
Tracker 6.3.5 JAR. The current OSP `swingJS` branch differs only in Swing number fields and added `hashCode`
methods.

## Using the kernel

```cpp
using namespace motrack::science;
FitFunction gaussian = builtInFit(BuiltInFit::Gaussian);
FitOptions options;                       // Corrected dialect, Autofit, from scratch, nothing fixed
const FitResult fit = fitCurve(gaussian, x, y, options);
// fit.parameters, fit.uncertainties, fit.uncertaintyBand, fit.rmsDeviation, fit.correlation, fit.status
const auto text = formatUncertainParameter(fit.parameters[0], fit.uncertainties[0]); // e.g. "3.01 ± 0.02"
```

- `fitCurve` is a pure function of its arguments: deterministic, single-threaded, no shared state, no
  exceptions except from the model callback. Run it on a worker: a Damped Sine fit of 10 000 points takes
  tens of seconds. `FitOptions::cancelled` is called on the fitting thread every 4096 model evaluations
  and between uncertainty refits, so it must be safe to call while another thread requests cancellation
  (read an `std::atomic<bool>` or `std::stop_token`); a cancelled fit returns `FitStatus::Cancelled` with
  the input values.
- `x` and `y` are the raw paired columns, including missing values. A `FitFunction` owns its parameter
  values; the caller keeps them between fits (Tracker refits from the current values whenever the data
  change, `FitStart::FromCurrent`, and from scratch when a fit is selected or Autofit is switched on).
- A user-defined fit is a `FitFunction` with names, starting values, the expression text and a `FitModel`
  callback `f(x, parameters)`. Slice I4 implements the callback with the K1 expression engine (see
  [Binding](#binding-for-slice-i4)).
- `FitResult::method` and `status` say how the parameters were obtained; Tracker shows no such status, so
  I4 can add it as an explanation (for example when a fit was skipped or did not improve).

## Fits

The menu shows Line and Parabola first, then the rest by display name (`TRACKER_FIT_MENU_ORDER`, English).
Line is the default. Parameters are listed in order with their default values.

| Menu name (OSP name) | Expression | Parameters (defaults) | Method | Initial guess from scratch |
| --- | --- | --- | --- | --- |
| Line (`Poly1`) | `A*x+B` | A slope, B intercept (0, 0) | normal equations | none |
| Parabola (`Poly2`) | `A*x^2+B*x+C` | (0, 0, 0) | normal equations | none |
| Cubic (`Poly3`) | `A*x^3+B*x^2+C*x+D` | (0, 0, 0, 0) | normal equations | none |
| Gaussian | `A*exp(-(x-B)^2/(2*C^2))` | peak height 1, position 0, standard deviation 1 | Hessian, then LM | Jacquelin's integral regression |
| Exponential | `A*exp(B*x)+C` | magnitude 1, rate −1, offset 0 | Hessian, then LM | offset from three equally spaced points, rate and magnitude through the ends |
| Sine (`Sinusoid`) | `A*sin(B*x+C)+D` | amplitude 1, ω 1, phase 0, offset 0 | Hessian, then LM | offset and amplitude from the y range; ω and phase from zero crossings |
| Damped Sine (`DampedSine`) | `A*exp(E*x)*sin(B*x+C)+D` | 1, 1, 0, 0, rate 0 | Hessian, then LM | as Sine; E keeps its fallback value (0) |
| Power | `A*x^B` | coefficient 1, power 1 | Hessian, then LM | line through (ln x, ln y), needs x, y > 0 |
| Log | `A*ln(x)+B` | scale 1, offset 0 | Hessian, then LM | line through (ln x, y), needs x > 0 |

`polynomialFit(degree)` also provides the degree-4 and degree-5 `KnownPolynomial`s OSP supports (names A–F).
OSP's descriptions are kept as untranslated English strings, including three inaccurate ones
([#246](https://github.com/mgradwohl/MoTrack/issues/246)). Each model repeats SuryonoParser's operation
order; `x^2` is evaluated as `x*x`, which is what HotSpot's `Math.pow(x, 2.0)` returns.

## How Tracker fits

**Data.** Tracker fits the rows `Dataset.append` keeps, minus rows whose y is NaN. `append` drops rows with
NaN or infinite x or infinite y by compacting its arrays but keeps their length, so the last rows are counted
again ([#230](https://github.com/mgradwohl/MoTrack/issues/230)), and it keeps only the last 524 288 rows: `Dataset.defaultMaxPoints` (16 384)
times the `maxPointsMultiplier` of 32 that `Tracker.start` sets, a deliberate capacity limit
([#247](https://github.com/mgradwohl/MoTrack/issues/247)). There are no weights or error bars in a fit, and the Data Tool of 6.3.5 shows no residuals.

**Fixed parameters and from-scratch starts.** From scratch, the built-in heuristic replaces every free
parameter; polynomials and user fits keep their values. Fixed parameters keep their values; Tracker fits a
"test function" without them. With every parameter fixed (or none defined) nothing is fitted.

**Autofit.** A polynomial without fixed parameters is solved in closed form. Every other fit (including a
polynomial with a fixed coefficient, evaluated through its expression) is minimized when the number of free
parameters is between 1 and the number of points, and only when the function is defined at every point for
the starting values (otherwise the fit is skipped and Tracker warns "The function returns non-real values
over this range"). Tracker runs `HessianMinimize` (20 iterations, tolerance 1e−6); if the deviation then
exceeds the starting one it restores the start and runs `LevenbergMarquardt` from the Hessian's best point;
if that fails or is worse too, the starting values are kept (`FitStatus::NotImproved`). While minimizing, an
undefined (NaN) function value counts as 0, as OSP's `UserFunction` returns, and a NaN deviation never counts
as worse ([#307](https://github.com/mgradwohl/MoTrack/issues/307)). An infinite value is an ordinary value to
OSP: Log on data with x = 0 (`log/nonpositive-x`) minimizes from an infinite deviation and reports a fit with an
infinite rms and no warning ([#311](https://github.com/mgradwohl/MoTrack/issues/311)). The fitted function then
holds the *last parameter vector the objective
evaluated*, often one the minimizer rejected, and that vector's deviation decides whether the fit improved
([#306](https://github.com/mgradwohl/MoTrack/issues/306)); the Tracker dialect carries that state exactly.

**Minimizers.** `HessianMinimize` first makes five damped (factor 0.35) one-parameter Newton cycles, then full
Newton steps with central-difference Hessian and gradient (steps (|p|+1)/1e5), solved by LU decomposition
with partial pivoting, until the step length, or the step divided by |p|² + tolerance, falls to 1e−12. It
keeps the better of start and result and never a result containing NaN. `LevenbergMarquardt` adds λ to the
Hessian diagonal (λ = 0.001, ÷10 after an improvement, ×10 otherwise) and also stops once λ is no longer above
1e−6, that is after three accepted steps; it reports failure unless its last step met the tolerance. Allowing
more steps did not make the non-converging oracle cases (damped sine from its initial guess) converge, so
MoTrack keeps this behaviour in both dialects.
LU decomposition never fails for doubles; a singular system yields infinities or NaN.

**rms deviation and R².** "rms dev" is √(Σ deviation² / N), NaN without points or when Autofit is on with more
parameters than points. The status bar's "Correlation coefficient (R-squared)" is the squared Pearson
correlation of the data themselves, whatever the fit, and is undefined below three points or for constant x
or y. The kernel keeps its own `doLinearRegression` port rather than K5's `squaredCorrelation`: the fitter
also needs to know that x or y is constant, which discards the uncertainties, and must see the points the
fit used, whereas `squaredCorrelation` drops NaN y itself and answers "undefined" for both of those cases.

**Uncertainties.** χ² is calibrated so that its minimum equals N − free parameters (σ_y² = Σ deviation² /
(N − free)). For each free parameter, Tracker fixes it at p ± δ, refits the others from the best fit, and
takes σ = δ √(2 / Δ) with Δ = χ²(p − δ) + χ²(p + δ) − 2χ²_min (Bevington, eq. 8.13). δ starts at (|p|+1)/1e5
and is multiplied or divided by 10 (at most ten tries) until Δ lies in [0.001, 2]. For a model linear in its
parameters this is the exact least-squares standard error. A non-positive curvature gives 0 (shown without
an uncertainty), a fixed parameter NaN. Tracker shows none with Autofit off, with no more points than free
parameters, or (three or more points) for constant x or y. `FitResult::uncertaintyStatus` names these cases.

**Band and display.** The shaded band is the envelope of the fit and, for each parameter with an uncertainty,
the curves at p − σ and p + σ. `formatUncertainParameter` reproduces the table text: σ rounded to one
significant digit (plus optional extra places) with the value at the same decimal place and a shared power of
ten, `(7.6 ± 0.4) E1`, using Java's `String.format` half-up rounding of `Double.toString` digits. The port
rounds the shortest round-trip digits (`std::to_chars`) and scales with `std::pow(10, n)`. JDK 17's
`Double.toString` sometimes prints one more digit than the shortest (fixed in JDK 19), so a value just at a
half-up boundary can, rarely, round differently; none of the 36 probes or the oracle's displayed values does.

## Dialects and deliberate differences

`FitDialect::Tracker` reproduces the reference; `FitDialect::Corrected` (the default) fixes the reproduced
defects below and otherwise runs the same algorithms. Neither dialect reproduces the text substitution, which
is a defect of how OSP builds functions rather than of the fit.

Corrected minimizes in four steps:

1. It keeps Newton's best vector.
2. It runs Levenberg-Marquardt from that vector unless Newton reached its tolerance with its last iterate
   kept and a lower deviation. A singular Hessian, for example, leaves Newton at the start. A rejected
   final iterate means the crude guess was kept, which is not a converged point.
3. Each Levenberg-Marquardt run stops after about three accepted steps, so Corrected restarts it from its
   best point while it keeps lowering the deviation without reaching its tolerance, at most 100 times. The
   damped-sine fits that Tracker abandons converge this way (`dampedsine/outlier`: deviation 0.393, where
   one run stops at 17.06).
4. It reports `Fitted` only when the minimizer whose result it keeps reached its tolerance: either the
   deviation decreased, or Levenberg-Marquardt's shrinking steps found nothing lower around the start (a
   minimum already).

Otherwise Corrected keeps the start (`FitStatus::NotImproved`), or the lower values reached without
convergence (`FitStatus::NotConverged`, such as a Gaussian fitted to a valley, whose best width is
unbounded). In both cases it estimates no uncertainty. The finite-difference derivatives (steps
(|p|+1)/1e5) still limit both dialects in valleys whose curvatures differ by more than about 10⁸.

| Tracker 6.3.5 | Corrected | Issue |
| --- | --- | --- |
| Fixed parameters and uncertainty refits replace the parameter's name in the expression text, so `c` in `cos`, or `E` after a fixed value written `1.0E-4`, corrupts the function (uncertainties ≈ 1e−16, fixed fits that do not move) | Values are passed to the model structurally in both dialects; `trackerSubstitutionCollisions()` reports where Tracker computes something else | [#240](https://github.com/mgradwohl/MoTrack/issues/240) |
| The band keeps the other parameters at their original values; with fixed parameters these are the values from before the fit | Band sets hold the refitted parameters | [#241](https://github.com/mgradwohl/MoTrack/issues/241) |
| Initial guesses sort through a TreeMap that keeps one y per x and leaves zero-padded arrays, and can be NaN (constant data, inverted Gaussian), which skips the fit | Every point sorted stably; a non-finite guess or failed inner line fit keeps the current values | [#242](https://github.com/mgradwohl/MoTrack/issues/242) |
| Polynomial normal equations in powers of x: digits lost for offset or narrow x; arbitrary or NaN results for repeated x | Normal equations in x scaled to [−1, 1]; covariance in closed form; `DegenerateData` for fewer distinct x than coefficients | [#244](https://github.com/mgradwohl/MoTrack/issues/244) |
| The last vector each minimizer evaluated, often a rejected one, is kept and decides whether the fit improved, so a worse vector is reported or an improvement is reverted | Each minimizer's best vector is kept, with the Levenberg-Marquardt fallback described above | [#306](https://github.com/mgradwohl/MoTrack/issues/306) |
| An undefined model value counts as zero deviation and a NaN deviation as no worse, so a fit can end where the function is undefined on the data | A vector undefined at any valid point is rejected; a fit is never reported for an undefined function | [#307](https://github.com/mgradwohl/MoTrack/issues/307) |
| An infinite model value (log 0, an overflowing exp) is a value: the fit runs from an infinite deviation and reports an infinite rms without the non-real-values warning | ±∞ is undefined like NaN: such a start is `UndefinedAtStart` with the warning and an undefined (NaN) rms, and the minimizers reject such vectors. Squared residuals that overflow on finite model values still give an infinite rms ([#372](https://github.com/mgradwohl/MoTrack/issues/372), not fixed yet) | [#311](https://github.com/mgradwohl/MoTrack/issues/311) |
| A singular Newton step keeps the start as the fit, and an unconverged improvement is reported as a fit; any positive χ² curvature, rounding noise included, gives an uncertainty, also around a start that was not improved | Levenberg-Marquardt continues, restarting while it improves, and an improvement without convergence is `NotConverged`; σ is undetermined below a curvature of 0.001 after ten tries and for `NotImproved` or `NotConverged` | [#307](https://github.com/mgradwohl/MoTrack/issues/307) |
| NaN or infinite x and infinite y rows repeat the last rows; only the last 524 288 rows are fitted (a deliberate capacity limit) | Rows with any non-finite value are dropped; no row limit | [#230](https://github.com/mgradwohl/MoTrack/issues/230), [#247](https://github.com/mgradwohl/MoTrack/issues/247) |
| The band stays drawn when constant x or y discards the uncertainties | The band is dropped with them | — |
| The warning reflects the last deviation computed, often an uncertainty refit | It reflects the final parameters | — |

Tracker's fits also share their parameter arrays between Data Tool tabs, so the Damped Sine rate guess comes
from the session's last such fit ([#245](https://github.com/mgradwohl/MoTrack/issues/245)); the kernel takes
that value explicitly (`FitOptions::scratchFallback`, default 0). The Tracker dialect assumes a fresh Data
Tool where state matters: Log and Power fall back to a zero Line fit, and a skipped fit returns no
uncertainties instead of the previous fit's. Formatting returns nothing for non-finite values, where Java
prints `NaN ± 0`.

## Kernel oracle

[`tools/reference/FitOracle.java`](../tools/reference/FitOracle.java) runs OSP's real `DatasetCurveFitter.fit`
headless with the pinned JAR. The fitter is a Swing panel whose constructor needs the Fit Builder dialog, so
the oracle allocates it without a constructor and installs the plain objects `fit()` uses (the real
`Dataset`, default fits, minimizers, drawer and parameter table, and a tab whose plot ignores messages);
every decision is OSP's own code. Like `Tracker.start`, it sets `Dataset.maxPointsMultiplier` to 32. It records
[`tests/science/fit-reference.json`](../tests/science/fit-reference.json):

- 155 cases: for every built-in fit exact data, noisy data, an outlier, shuffled order, repeated x, exactly as
  many points as parameters and one fewer, a fixed parameter, a refit from current values, a manual fit and
  four seeded random data sets; missing and infinite values, the 524 288-row limit (evaluated with Autofit
  off), constant and repeated data, undefined starting values, negative fixed values, two fixed cubic
  coefficients, offset x, the substitution defects with the intended fit computed from renamed parameters,
  and user-defined fits (`m*x+b`, `A*(1-exp(-x/T))`, a Lorentzian from two starts, one of which falls back to
  a converging Levenberg-Marquardt, `a*cos(b*x)+c`, a parameterless function, one undefined at the start);
- 14 direct minimizer runs on arithmetic objectives (Rosenbrock, a bowl, a singular flat valley, a quartic,
  an objective that becomes NaN) and 36 formatting probes.

Each case stores the inputs, Java's parameters, uncertainties, band, the refitted band OSP computes before
overwriting it, rms, R², the warning flag, the display text, which minimizer produced the result, the vector
and deviation the minimization started from, and whether the fit is *tied* (ended at its starting deviation,
to 1e−12). 76 cases
also carry an independent expectation computed without OSP: synthetic parameters for exact data, exact
80-digit least squares and standard errors for models linear in their parameters, and a Gauss-Newton minimum
with linearized standard errors for the others. Data and expectations use `StrictMath` (fdlibm), so every JVM
generates identical inputs; only the fits run through OSP's `Math` calls.

### Tolerances

`CurveFitReference.*` in `motrack_tests` checks the Tracker dialect against Java, Java and the Corrected
dialect against the independent expectations, the corrected band against OSP's refits, and the minimizers
and formatting bit for bit. `python tools/corpus.py fit-oracle --java <jdk>/bin/java` regenerates the
fixture with Java in a temporary home and applies the same rules; CI runs it with x86-64 Java on Linux and
with arm64 Java, whose `Math` functions are fdlibm, on macOS. `--record` rewrites it.

- **Exact** (Line, Parabola, `m*x+b`, the Lorentzian, minimizers): only + − × ÷ and √ (`x^2` is `x*x`), so
  results, including the Levenberg-Marquardt path of `user-lorentz/levenberg-marquardt`, are bitwise identical.
- **Polynomial** (Cubic): closed-form coefficients are bitwise; the uncertainty refits evaluate x³ with `pow`.
- **Transcendental** (everything using `exp`, `sin`, `cos`, `ln`, `pow`): Java's `Math` functions differ from
  each C++ library in the last bit, and the finite-difference Hessian amplifies that by about 1e6 per Newton
  step before the 1e−12 step tolerance stops it. Parameters and rms must agree within 1e−7 relative + 1e−9,
  uncertainties and band within 1e−4 relative + 1e−9 (second differences of χ², refitted). Measured on
  Windows (MSVC STL) the largest differences were 3.1e−12 for parameters and 3.7e−4 relative for the
  rounding-level uncertainties of exact data, which the absolute term covers; Linux (glibc) and macOS agree
  within the same bounds outside the chaotic cases. The main-fit path must match too, except for the 11 tied
  fits: from a start already at the minimum (Log's line-fit guess, or exact data) Newton's result, the
  Levenberg-Marquardt fallback and the restored start differ in the last bits, so any of them may reach the
  compared values.
- **Starts.** The starting vector and deviation compare bitwise for exact fits and within the parameter
  tolerance otherwise; `TrackerScratchGuessesMatchJava` checks every from-scratch guess (Sinusoid's, which is
  arithmetic, bitwise). Any fit reported as `Fitted` must end at or below Java's starting deviation.
- **Sensitivity.** The oracle reruns each such case with every y moved by one ulp. Where a quantity moves,
  the tolerance grows to 100 times that movement; a quantity that moves by more than 1e−3 is decided by last
  bits (uncertainties around a non-minimum, for example) and is not compared. Of the 101 such cases, 2 skip
  parameters and 2 skip uncertainties.
- **Chaotic** (11 cases): where Tracker's Newton iteration diverged (its result is more than 1e−6 relative,
  plus 1e−12 Σy², worse than the start) the outcome of the three-step Levenberg-Marquardt fallback depends on
  last bits: `dampedsine/shuffled` converges with HotSpot's `exp`/`sin` and fails with glibc's and Apple's.
  The oracle marks such cases automatically, and the overflowing default start of `A*(1-exp(-x/T))` by hand.
  Another JVM may classify a transcendental case as chaotic or the reverse (`dampedsine/outlier` with fdlibm),
  so the regeneration check then applies the chaotic rule. Data handling always compares, and parameters
  compare (within the transcendental tolerance) when the port took Java's path.

Independent expectations use 1e−6 relative (1e−7 absolute) for synthetic parameters, 1e−6 (1e−8) for least
squares, 1e−4 for linear standard errors and 5 % for nonlinear ones, which the curvature method estimates at a
finite δ rather than by linearization. Chaotic cases carry no expectation, nor do the damped-sine outlier and
fixed-offset fits, which do not converge from Tracker's own initial guess.

## Binding for slice I4

- **User fits.** Compile the Fit Builder expression with K1 over the variables `{x, parameters…}` (after
  `UserFunction`'s space removal and name padding) and evaluate it in the `FitModel`, returning NaN for an
  undefined value; the fitter applies OSP's NaN-as-0 rule (Tracker) or rejects the vector (Corrected, which
  treats ±∞ the same way). The expression dialect (K1 `Corrected` or `Tracker`) decides operator precedence;
  K3 does not depend on it. Clones of the built-in polynomials stay
  `KnownPolynomial`s (closed form) under their own names.
- **State.** Keep each tab's fits, fixed flags, Autofit and the scratch fallback per tab
  ([#245](https://github.com/mgradwohl/MoTrack/issues/245)); refit from current values when data change and
  from scratch on selection or when Autofit is switched on; clear uncertainties when a fit is skipped.
- **Warnings.** Show Tracker's "non-real values" warning from `nonRealValues`, `trackerSubstitutionCollisions()`
  for an opened experiment, and that rows outside the fit selection or with missing values are not fitted.
  Explain `FitStatus::NotImproved` (the start was kept because no defined parameters lowered the
  deviation) and `FitStatus::NotConverged` (lower values, but not a minimum, so no uncertainties).
- **Threads.** Run fits on a worker and cancel through an atomic flag or `std::stop_token` read by
  `FitOptions::cancelled`; discard a result whose inputs changed meanwhile.
- **Display.** Use `formatUncertainParameter` with the locale's decimal separator, "rms dev" with pattern
  `0.000E0`, translated fit names and descriptions, and draw the band from `uncertaintyBand`.
