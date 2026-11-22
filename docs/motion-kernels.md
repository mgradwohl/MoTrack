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
- `measuredTimeDerivatives(clip, positions, stepTimesSeconds, timeResolutionSeconds)` is MoTrack's
  extension for measured (decoded) step times; `evenlySpacedWindow`, `quadraticSlope` and
  `leastSquaresCurvature` are its parts (see [workspace measurements](#workspace-measurements)).
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
  single-step edits re-run Bounce Detection on five steps only (#206).
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

## Workspace measurements

The workspace plot and its selected-series CSV export (`measurementPlot` in `src/measurements.cpp`) take
velocity and acceleration from `pointMassDerivatives` with Tracker's default *Finite Difference* algorithm
(#207). Speed is the magnitude of that velocity, momentum is mass × velocity, and kinetic energy is ½ m v².
Inputs are the world positions of the marks on clip steps (image pixels when uncalibrated). Each run of
consecutive marked steps is evaluated on its own, so a value needs every step of its window: none at the first
and last step for velocity, none at the first and last two steps for acceleration, and none in a window that
contains a missing step. A missing value is left out of the plot and the export, never written as zero.

Time scaling depends on where the times come from:

- **Constant timing** (a saved or default frame duration: media-free projects and image sequences). The step
  time of clip step k is k × duration × stride / 1000 s, in the operand order of Tracker's media-free
  `StepperClipControl.getStepTime`. That order matters. With 1000/30 ms frames, (k × stride) × duration
  differs in the last bit at 34% of the first 1000 steps for a stride of 3, 44% for 5 and 12% for 7. It never
  differs for strides of 1, 2 or 4, where scaling by a power of two is exact. Tracker then adds the clip
  start time and subtracts absolute step times. The intervals are the same as MoTrack's, bit for bit when the
  start is 0. Otherwise they agree within the rounding of those absolute times, which MoTrack avoids so that
  a large time origin cannot distort them. Image sequences are timed through the video in Tracker, which
  agrees within rounding. Plotted times still include the start time.
- **Decoded movie frame times** (`measuredTimeDerivatives`, #331). These are seconds from the movie's first
  frame, per original frame number. The decoder takes them from the stream's time base (the tick of its
  timestamps, for example 1/1000 s in Matroska/WebM or 1/600 s in many phone recordings) and rescales them to
  whole nanoseconds. The index records that time base, and the native document saves it with the times.
  - **Evenly spaced windows** get Tracker's stencils above, bit for bit with Tracker on the same times.
    A window counts as evenly spaced when every interior time lies within the timestamp resolution of the
    evenly spaced grid between its end times: max(time base, 1 ns), plus 1 ns for the rescaling and 8 ulps.
    That is the most that rounding the timestamps of a constant rate can move a time, so constant-rate video
    is never mistaken for variable timing. For velocity's three steps this says the two intervals agree within
    2 × max(time base, 1 ns) (+2 ns). For five steps it checks each time against the grid rather than only the
    spread of the intervals. The interval spread would leave a frame two ticks late exactly on the boundary,
    and nanosecond rounding would then decide.
  - **Other windows** use time-aware estimates. Velocity is the slope at the step's time of the quadratic
    through the step and its two neighbours (`quadraticSlope`). Acceleration is 2c of the least-squares
    parabola x ≈ a + b (t − tₖ) + c (t − tₖ)² over steps k−2..k+2 (`leastSquaresCurvature`). Both are exact
    for constant acceleration at any spacing.
  - **Continuity:** at equal spacing the parabola is Tracker's (2, −1, −2, −1, 2) / (7Δ²); the derivation is
    in `motion_derivatives.cpp`. So acceleration is a five-point fit everywhere, and its noise does not change
    where the estimator does. Windows, edges and gaps are Tracker's, so values are blank exactly where Tracker's
    table is.
  - **What the timestamps mean decides the remaining difference from Tracker** (#87):
    - *Constant-rate capture with coarse timestamps:* positions follow the true, evenly spaced times. With
      their 1/1000 s time base the 33/34 ms intervals of 30 fps (and the intervals at 29.97 and 60 fps) are
      evenly spaced. The values are therefore Tracker's: no acceleration for steady motion (|a| < 1e-9; Tracker's
      stencil gives about 1e-13). Before #331 the time-aware three-point estimates reported up to 1.77, 1.78
      and 7.43 m/s² there for 2 m/s. Velocity is Tracker's secant, which keeps the rounding of its two
      timestamps: about 1% here, as in Tracker.
    - *True variable frame rate:* positions follow the recorded times. Beyond one tick from the grid the
      time-aware estimates are exact, where Tracker's stencils invent acceleration for steady motion, for
      example −0.26 m/s² at 2 m/s on 0, 33, 67, 100, 133 ms with a nanosecond time base. Within one tick the
      timestamps cannot tell jitter from rounding, so Tracker's stencils apply. A sample displaced by up to one
      tick moves the acceleration by at most (2/7) v × tick / Δ², the resolution such timestamps allow. That
      is 1.4 m/s² at 3.3 m/s for 1/600 s ticks at 30 fps, and 0.008 m/s² for 1/90000 s.
    - *Unknown time base* (a document saved before #331, until its movie is indexed again, which happens on
      opening): nanosecond resolution is assumed. Constant-rate video with coarse timestamps then gets the
      five-point fit on its rounded times: up to 0.26 m/s² at 30 fps and 1.0 m/s² at 60 fps for 2 m/s.
  - **Tests:** `MeasurementPlot.ConstantRateCaptureWithMillisecondTimestampsUsesTrackersStencils` and
    `MeasurementPlot.AMixedClipOnACoarseTimeBaseHasNoEstimatorDiscontinuity`. The mixed clip is 30 fps on
    1/600 s with free fall and ±1 mm noise, one frame one to three ticks late. It stays within the five-point
    noise band (±0.51 m/s²) when the frame is two or three ticks late, including where the estimator changes;
    the three-point estimates swung by ±3.6 m/s² there. One tick late, it stays within the one-tick bound
    above.

`MotionReference.MeasurementPlotMatchesTrackersDefaultDerivatives` checks `measurementPlot` against every
Finite Difference case of the fixture. With decoded times it equals Tracker bit for bit, and with constant
timing too when the clip starts at 0 ms (within 1e-12 relative otherwise). In the variable-time case (steps 89
to 107 ms apart) it gives the analytic derivatives through the time-aware estimates, where Tracker does not. The fixture's uniform cases use strides 1 and 2, where both
operand orders agree, so `MeasurementPlot.ConstantTimingOnAStrideUsesTrackersStepTimeOperandOrder` covers a
stride of 3.

## Driving the kernels from C2

C2 will build each point mass's clip-step positions with `clipStepPositions` from world marks at the
current calibration, take step times from the clip timing (VideoPlayer semantics, seconds), and call
`pointMassDerivatives` with the track's algorithm and `pointMassAngularMotion` for θ, ω and α. Speed,
direction, momentum, kinetic energy and path length follow from those columns (with θ_p = θ_v, #205).
A filtered clone applies `filterPositions` with the saved filter (a new Butterworth filter uses
`butterworthSampleRateHz` of the mean step duration), then differentiates the filtered positions and
reports `filterRmsDeviation`. C2 also decides whether to replicate Tracker's image-space vector round trip
(last-bit differences), and whether to offer Tracker's uniform stencils for unequal frame intervals as an
option beside the time-aware estimates described under [workspace measurements](#workspace-measurements) (#87).
Saved filter windows are unbounded (the dialog stops at 99) and Savitzky-Golay edge weights cost
O(window² × order²), as in Tracker, so C2 bounds or flags implausible loaded windows before filtering. The
kernels are pure functions of a snapshot, so C2 evaluates them off the GUI thread per document revision
and never depends on edit history (#206).
