# Motion kernels

`motrack_motion` (`src/science/motion/`, namespace `motrack::science`) ports the numerical kernels behind
Tracker's point-mass motion data: the velocity and acceleration algorithms, the polar-angle derivatives,
the three motion filters of a filtered point-mass clone, and OSP's `numerics.Derivative`. It is a Qt-free
library (slice K4 of the [implementation order](migration-plan.md#implementation-order-and-exit-gates));
the data columns, algorithm choice and filtered clones arrive with slice C2.

The reference is the Tracker 6.3.5 JAR and its source candidate `dc064f92` (`PointMass`,
`FirstDerivative`, `SecondDerivative`, `BounceDerivatives`/`BounceModel`/`BounceParameters`/`BounceMatrix`,
`MotionFilterSupport`, `MovingAverageFilter`, `ButterworthFilter`, `SavitzkyGolayFilter`,
`FilteredPointMass`, `MotionFilterDialog`, `DerivativeAlgorithmDialog`), and OSP `d56207b0` for
`numerics.Derivative`. The filters entered upstream in `197225dc` (2026-05-02) and ship in 6.3.5. On
2026-11-21 the Tracker `SwingJS` tip (`34b1b503`) had not changed these classes or `src/test/FilterSelfTest.java`
(PointMass differs by whitespace), and the OSP `swingJS` tip (`d49c6ecf`) had not changed `Derivative`.

## How Tracker derives point-mass motion

- **Samples.** PointMass gathers the world position of each mark on a clip frame (start + k × step size).
  Frames between clip steps are ignored, and `refreshData` removes a mark with a NaN world coordinate.
  The kernels work on clip-step index k; `clipStepPositions` maps marks to steps.
- **Algorithms.** Each point mass has its own algorithm, chosen with *Algorithms...* in a point-mass plot's
  menu. It is not saved in `.trk` files and a filtered clone starts with the default. Spills (stencil
  half-widths) are fixed: `vDerivSpill = 1`, `aDerivSpill = 2`, `bounceDerivsSpill = 3`.

  | Algorithm (code) | Velocity per step | Acceleration per step² |
  | --- | --- | --- |
  | Finite Difference (0, default) | `(x[k+1] − x[k−1]) / 2` | `(2x[k−2] − x[k−1] − 2x[k] − x[k+1] + 2x[k+2]) / 7` |
  | Smoother Finite Difference (2) | `(−2x[k−2] − x[k−1] + x[k+1] + 2x[k+2]) / 10` | as above |
  | Bounce Detection (1) | seven-step quadratic fits with a detected velocity step | the fit's 2c plus the step as a pulse |

  The five-point formulas are the least-squares slope and parabola of five equally spaced samples. A
  finite difference exists only where every step of its window has a mark, so the first and last steps
  have no velocity (two for the smoother velocity) and the first and last two no acceleration. A missing mark removes the derivatives of
  every window that includes it. Tracker creates a velocity or acceleration only at a marked step.
- **Bounce Detection** (Kevin Karplus, 2011) fits each step's seven-step window, shifting it past missing
  steps toward the nearer side (it can then extrapolate to a clip edge or an isolated mark). It fits both a
  quadratic and a model with an unknown velocity step, estimates the step time from each coordinate and
  from their dv²-weighted mean, and rates each candidate by how much it reduces the squared error of the
  windows within three steps. Steps are then accepted in decreasing value, at most one per window, when
  the reduction is at least 0.6 × 7 × the quadratic's error. Windows containing an accepted step use a
  quadratic fitted after removing it; velocity jumps across the step and acceleration gets the velocity
  change as a pulse at the step nearest the impact.
- **Time scaling.** `refreshData` divides by Tracker's mean step interval, for every algorithm:
  velocity by `(t[k+1] − t[k−1]) / 2` and acceleration by `((t[k+2] − t[k−2]) / 4)²`, where `t[k]` is
  `VideoPlayer.getStepTime(k) / 1000` (seconds; NaN outside the clip). So a clip-edge Bounce Detection
  estimate still has no table value. Units are length/time and length/time². The stencils assume equal
  intervals: on variable frame-rate media they report spurious acceleration for constant velocity
  (MoTrack #87).
- **Angular data.** `getRotationData` accumulates the change of `atan2(y, x)`, wrapped into [−π, π],
  across missing steps; θ is in radians (the display can convert). ω and α always use the spill-1
  difference and the spill-2 parabola with the same time scaling. At the origin (r = 0) the angle is
  undefined; like Tracker, the kernel uses `atan2(0, 0)` = 0 (±π when x is −0), so θ there and the ω
  and α of every window containing that step are not meaningful. Whether to flag such steps or treat
  them as missing is a C2 decision (MoTrack #205).
- **Vectors.** Tracker stores velocity and acceleration as image-space vectors and converts them back to
  world components for the table, which can change the last bits (see [C2](#driving-the-kernels-from-c2)).

## Motion filters

A filtered clone (*Show Filtered Clone* in the track menu, then *Edit Filter...*) applies one filter to
the source's world x and y separately, one sample per clip step, with a step valid only if it has a mark
and numeric coordinates.
Each filter works on every run of consecutive valid samples; gaps stay gaps. The clone is then
differentiated like any point mass. Its *RMS Dev* is the root-mean-square distance from the source.

| Filter (saved properties) | Dialog range, default | Normalization | Behaviour |
| --- | --- | --- | --- |
| Moving Average (`window`) | 3–99 odd, 5 | below 1 → 1; even → +1 | centred mean; at a run's ends the window is truncated to the run |
| Butterworth Lowpass (`order`, `cutoff_hz`, `sample_rate_hz`) | order 1–8, 4; cutoff 0.1–1000 Hz, 6; rate = 1000 / mean step ms | order clamped to 1–8; cutoff or rate ≤ 0 → 1 Hz | bilinear biquads with prewarping and steady-state initial conditions, applied forward then backward over odd-reflection padding of `min(n − 1, max(3(2s + 1), 6s))` samples (s sections); cutoff ≥ Nyquist or a run under 3 samples: unchanged |
| Savitzky-Golay (`window`, `poly_order`) | window 5–99 odd, 7; order 1–6, 2 | window ≥ 3 and odd; order ≥ 1 and < window | least-squares polynomial at the window centre, one-sided windows of the same length at a run's ends; a shorter run uses its longest odd window, and fewer than order + 1 samples stay unchanged |

Forward-backward filtering squares the Butterworth magnitude, so the cutoff becomes the −6 dB point and
the effective order doubles. The Savitzky-Golay coefficients come from Gauss-Jordan inversion of the
normal equations, which loses accuracy for wide, high-order windows (about 1e-11 at the edges for 99/6
on a unit-scale signal), as in Tracker.

## API

- `stepDerivatives(algorithm, clip, positions)` returns per-step derivatives; `pointMassDerivatives(...,
  stepTimesSeconds)` returns velocity and acceleration in world units per second; `pointMassAngularMotion`
  returns θ, ω and α. `firstStepDifference` and `secondStepDifference` expose the 1-D stencils.
- `MovingAverageFilter`, `ButterworthFilter` and `SavitzkyGolayFilter` normalize their parameters like
  Tracker's setters; `applyMotionFilter`, `filterPositions` and `filterRmsDeviation` apply them.
  `butterworthSampleRateHz` is the dialog's rate rule.
- `centeredDerivative`, `forwardDerivative`, `backwardDerivative`, `secondDerivative`,
  `partialDerivative` and `rombergDerivative` port `numerics.Derivative`; Romberg reports OSP's error codes
  as a status instead of throwing. Tracker 6.3.5 does not call this class; it serves later Data Tool and
  function slices.
- Missing input and non-finite results are `std::nullopt`; nothing becomes zero. An invalid clip lattice or
  a size mismatch throws `std::invalid_argument`. The functions are pure and thread-safe.

## Deliberate differences and upstream findings

- **Bounce Detection on strides above 1 (fixed, MoTrack #204).** Tracker's step search walks frames, not
  clip steps, and throws `NullPointerException` on the first frame between steps. MoTrack runs the search
  on virtual frames (first frame + step index), which is Tracker's own arithmetic for a stride of 1, so
  every stride gives Tracker's stride-1 result for the same samples, bit for bit.
- **Bounce Detection overflow (fixed, #204).** Beyond about 1e154 the dv² weights overflow and Tracker
  recurses until `StackOverflowError`; MoTrack skips the non-finite step-time estimate.
- **Evaluated range.** Tracker limits Bounce Detection to `(array length − start) / stride` steps, where
  the length is its internal frame-array capacity (at least the last marked frame + 6). For a stride of 1
  that always includes every step with a fit (up to three past the last mark); MoTrack evaluates every
  clip step.
- **Non-finite values.** Infinite coordinates are treated as missing like NaN, and a non-finite result
  (overflow, zero step interval) is missing; Tracker would show ±Infinity or NaN. `filterRmsDeviation`
  without common samples is `std::nullopt` where Tracker shows 0. A NaN Butterworth cutoff from a
  malformed file filters to NaN in Tracker and to missing samples here.
- **For C2** (reference defects, not reproduced in the kernels): the θ_p column is mass × θ_v and a
  Butterworth clone keeps its saved sample rate after the clip timing changes (#205); the *Smoother Finite
  Difference* help shows the negated velocity formula (#205); vectors keep their old components when the
  change is below 0.01 image pixel, autofill refreshes a range computed from step numbers as frames, and
  single-step edits re-run Bounce Detection on five steps only (#206). MoTrack's current measurement plot
  uses a three-point acceleration instead of Tracker's five-point default (#207).
- **Not reproduced.** Tracker's shared static derivative arrays (stale values off the clip lattice),
  `BounceDerivatives` overwriting its input with NaN, and console diagnostics.

## Floating point and the Java oracle

Each expression keeps Java's operand order and integer-to-double conversions; Double.compare (for the
stable sort of step values), Java's `(int)` truncation and `Math.round` ties, and JAMA's own `hypot` are
reproduced. The library and its tests compile with `-ffp-contract=off`, so the arithmetic kernels equal
Java bit for bit.

[`tools/reference/MotionOracle.java`](../tools/reference/MotionOracle.java) runs the pinned classes
headless the way PointMass and FilteredPointMass drive them: frame-indexed arrays with valid flags only
on marked clip frames and junk in every invalid slot. PointMass needs a TrackerPanel, so its time scaling
and angle unwrapping are transcribed in the oracle; C2's state oracle checks the full columns. Inputs use
only IEEE arithmetic (rotations from literal cosines, the FilterSelfTest generator), so they are identical
on every JVM. The cases cover free fall from a nonzero time origin, noisy motion with single, double and
edge gaps and a NaN coordinate, a bouncing ball from frame 1234, strides 2 and 3 with variable step times,
lengths 0–8, an all-missing track, isolated marks and overflowing coordinates, all for the three
algorithms; 1-D differences; two circular motions and the axes for angles; 28 filter configurations
(defaults, every Butterworth order class, normalization edge cases, cutoff at and near Nyquist, low
cutoffs, Savitzky-Golay up to 99/6) on noisy, smooth, short-segment and degenerate signals; the six
FilterSelfTest checks; and `numerics.Derivative` on OSP's `src/test/Derivative.java` function
x log x sin x cos x (including its 999 999-term benchmark sum), a cubic, a partial derivative and the
three Romberg outcomes. It also reads the algorithm codes, spills and filter defaults from the classes.
[`tests/science/motion-reference.json`](../tests/science/motion-reference.json) (175 KB, one case per line)
stores them, so `motrack_tests` needs no Java. `python tools/corpus.py motion-oracle --java <java>`
regenerates and compares it in Linux CI; `--record` replaces it for review.

Agreement with the fixture:

| Kernel | C++ against the fixture | Java against the fixture (CI) |
| --- | --- | --- |
| Finite differences, Bounce Detection (stride 1), time scaling, Moving Average, Savitzky-Golay, cubic/partial/Romberg-cubic | exact | exact |
| Butterworth | 1e-12 of the largest input: an ulp in `tan`/`sin`/`cos` moves each pole by ~1e-16 and the response's pole sensitivity stays under 1e3 for fc/fs ≥ 0.002; observed 8e-16 | 1e-12 relative (JDK intrinsics) |
| θ, ω, α | 64 ulps of the largest angle for θ, divided by dt for ω and by dt²/8 for α (fdlibm `atan2` against the C++ library); observed under 2% of that | exact (StrictMath) |
| x log x sin x cos x differences | 8 ulps of the largest function value times the stencil weight, over the stencil's step power; the benchmark sum 1e-8 (the bound if every term erred alike); observed 2e-13 | 1e-12 relative |
| Bounce Detection, stride above 1 | Tracker's stride-1 values for the same samples, exact | Tracker's exception class |

Independent tests guard against a port that is consistently wrong: Tracker's FilterSelfTest (all six
checks), exact free fall from a nonzero time origin on a stride, the analytic response of each stencil to
cubics and sinusoids, the spurious acceleration of jittered step times, Bounce Detection exact for
constant acceleration (including shifted edge windows) and recovering a velocity reversal as a single
pulse, stride invariance, uniform circular motion through ±π, the −6 dB Butterworth point, DC and Nyquist
behaviour, polynomial preservation by Savitzky-Golay up to 99/6, and the numerical derivatives' exact
results for polynomials.

## Driving the kernels from C2

C2 will build each point mass's clip-step positions with `clipStepPositions` from world marks at the
current calibration, take step times from the clip timing (VideoPlayer semantics, seconds), and call
`pointMassDerivatives` with the track's algorithm and `pointMassAngularMotion` for θ, ω and α. Speed,
direction, momentum, kinetic energy and path length follow from those columns (with θ_p = θ_v, #205).
A filtered clone applies `filterPositions` with the saved filter (a new Butterworth filter uses
`butterworthSampleRateHz` of the mean step duration), then differentiates the filtered positions and
reports `filterRmsDeviation`. C2 also decides whether to replicate Tracker's image-space vector round trip
(last-bit differences) and whether to offer time-aware derivatives for variable frame timing (#87).
Saved filter windows are unbounded (the dialog stops at 99) and Savitzky-Golay edge weights cost
O(window² × order²), as in Tracker, so C2 bounds or flags implausible loaded windows before filtering. The
kernels are pure functions of a snapshot, so C2 evaluates them off the GUI thread per document revision
and never depends on edit history (#206).
