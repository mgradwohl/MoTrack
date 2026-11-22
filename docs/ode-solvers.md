# ODE solvers

`motrack_ode` (`src/science/ode/`, namespace `motrack::science`) ports the OSP ODE solvers that
Tracker's dynamic particle models and two-body systems integrate with. It is a Qt-free kernel (slice
K2 of the [implementation order](migration-plan.md#implementation-order-and-exit-gates)); models,
UI and file integration arrive with slice C5. The reference is OSP commit `d56207b0`, the source of
the pinned Tracker 6.3.5 JAR. On 2026-11-21 the OSP `swingJS` tip (`d49c6ecf`) had no `numerics`
changes since then, and the Tracker `SwingJS` tip (`34b1b503`) changed only a mass-field refresh and
a beep in `DynamicSystem`/`ParticleModel`.

## How Tracker uses a solver

- `DynamicParticle` constructs `new RK4(this)`. The Model Builder builds a solver dropdown (RK4,
  Euler, Ralston2), but 6.3.5 never adds it to the toolbar and files do not save a solver. Only the
  legacy-XML fallback of `DynamicParticle.Loader` reads a `solver` class name and instantiates any
  class with an `(ODE)` constructor by reflection; `odeSolverFromClassName` resolves those names.
- The state ends with time, whose rate is 1: `{x, vx, y, vy, t}` for Cartesian and polar particles
  (polar forces are converted to Cartesian), `{x1, vx1, y1, vy1, x2, vx2, y2, vy2, t}` for a
  two-body `DynamicSystem` (`{t}` when empty). Time accumulates by repeated addition of the step.
- The step is computed with two roundings, `dt = meanStepDurationMs / (1000 * 10)` then
  `dt /= 10` (`tracePtsPerStep` and `iterationsPerStep`, both 10): each of 10 trace points per clip
  step takes 10 solver steps. `reset()` restores the initial state (t0 = start-frame time), calls
  `initialize(dt)` and takes `(firstFrameInClip - startFrame) * 10 * 10 / clipStepSize` steps
  with Java integer division (`DynamicParticle.java:208`) to the first frame in the clip.
- `refreshSteps()` only advances, from the last computed frame to the current one, taking
  `10 * (end - start) / clipStepSize` trace points (integer division) and saving a copy of the
  state at every tenth. Moving back reuses computed frames; trimming the end frame restores that
  frame's saved state into the array without reinitializing the solver. Parameter edits reset the
  model.
- Tracker ignores solver error codes and NaN. Positions beyond ±8000 × 6000 image pixels become NaN
  (invalid steps) with a one-time warning.

## Solvers

| Kind | Class (factory name) | Method | Rates per step |
| --- | --- | --- | --- |
| `Euler` | `Euler` (`euler`) | first order | 1 |
| `EulerRichardson` | `EulerRichardson` (`eulerrichardson`) | midpoint, second order | 2 |
| `Ralston2` | `Ralston2` (`ralston2`) | second order | 2 |
| `Heun3` | `Heun3` (`heun3`) | third order | 3 |
| `Rk4` | `RK4` (`rk4`) | classical fourth order — **Tracker's default** | 4 |
| `Butcher5` | `Butcher5` (`butcher5`) | fifth order | 6 |
| `Fehlberg8` | `Fehlberg8` (`fehlberg8`) | eighth-order weights of the 7(8) pair, fixed step | 13 |
| `Adams4/5/6` | `Adams4/5/6` (`adams4/5/6`) | predictor-corrector started by RK4, Butcher5 or Fehlberg8 for 3, 4 or 5 steps | 1 + starter, then 2 |
| `Verlet` | `Verlet` (`verlet`) | velocity Verlet on `{x, v, ..., t}` pairs | 2 |
| `LeapFrog` | `LeapFrog` (class name only) | Störmer positions; `initialize`/`setStepSize` estimate the prior state with a backward Verlet step | 1 (+2 per initialize) |
| `CashKarp45` | `CashKarp45` (`cashkarp45`) | adaptive embedded 4(5) pair | 6 per attempt |
| `DormandPrince45`, `Rk45` | `DormandPrince45` (`dormandprince45`), `RK45` (class name only) | adaptive six-stage Dormand-Prince pair | 6 per attempt |
| `Rk45MultiStep` | `RK45MultiStep` (class name only) | Dormand-Prince substeps on the state itself to cover a fixed interval | substeps |
| `Multistep` | `ODEMultistepSolver` (`multistep`) | Dormand-Prince engine on a private state copy, copied back per interval | substeps |

Construction reproduces the Java defaults: step 0.1 (0.01 for the embedded pairs; the multistep
engines start at 0.01 or 0.005), tolerance 1e-6. The embedded pairs use the maximum absolute
error over all components, make up to ten attempts, shrink by `0.9 (err/tol)^-1/4` (at most 10×),
grow by `0.9 (err/tol)^-1/5` (at most 10×) when the error is below tol/10, and treat an error at
or below `Float.MIN_VALUE` as tol/1e5. Dormand-Prince raises tolerances below 1e-12; Cash-Karp only
warns. The multistep wrappers repeat substeps until the remainder is at most `tol × h`, shorten
the last substep to the remainder (then restore the adaptive step) and fail on an engine failure,
a remainder change of at most `Float.MIN_VALUE`, an engine step below `tol × h / 10`, or more than
200 substeps. `step()` returns the interval actually advanced.

## API

`makeOdeSolver(kind, rate, state)` or `createOdeSolver(factoryName, …)` returns a solver that
owns its state. The rate callback `(std::span<const double> state, std::span<double> rate)`
should signal an undefined result with NaN instead of throwing. `step()`, `advance(count)` and
`advanceTo(time)` report `Ok`, `DidNotConverge` (the OSP error code) or `NonFinite` (NaN or
infinity, which OSP does not detect) without throwing and without changing the numbers;
`convergenceFailure()` names the multistep reason, such as `TooManySubsteps` or `StepTooSmall` for
a stiff interval. `advanceTo` is a convenience that takes whole steps until the time component is
within half a step of the target; it is not Tracker's sequence whenever Tracker's integer step
counts truncate (for example when `firstFrameInClip - startFrame` is not a multiple of the clip
step size), so model code reproduces Tracker with `advance(count)`. Kind lookups never throw, and
a value outside the enumeration yields no name and no solver. A solver has no shared state and is
used from one thread at a time.

## Deliberate differences and upstream findings

The three reference defects are tracked in the
[issue triage](upstream-issues.md#defects-discovered-by-the-reference-harness).

- **Adams6 predictor (fixed, [MoTrack #178](https://github.com/mgradwohl/MoTrack/issues/178),
  upstream [osp#10](https://github.com/OpenSourcePhysics/osp/issues/10)).** OSP divides the
  six-step Adams-Bashforth weights, which sum to 1440, by 720. The doubled predicted increment
  makes OSP's Adams6 first-order accurate after its starter. MoTrack uses 1440 and is sixth order;
  the oracle compares Adams6 only for its five starting steps, and a test shows the Java error
  exceeds the corrected one by more than 10^4.
- **Backward `ODEMultistepSolver` (fixed, [MoTrack #179](https://github.com/mgradwohl/MoTrack/issues/179),
  upstream [osp#11](https://github.com/OpenSourcePhysics/osp/issues/11)).** `minus()` lacks the
  `break` that `plus()` and `RK45MultiStep` have. After a failure Java keeps stepping: it loops
  forever once the engine step underflows, and otherwise finishes the interval while reporting
  `DID_NOT_CONVERGE`. MoTrack stops at the first failure and returns the partial interval, like
  `plus()`. The oracle case `oscillator/multistep/backward-too-many-substeps` pins the difference
  (Java covers all of −10; MoTrack stops after 201 substeps), and forward or successful steps are
  unchanged.
- **Kept OSP behavior.** OSP's velocity Verlet evaluates its second rate at the advanced positions
  with the old velocities and the old time, as its documentation states. The method is second
  order only for position-dependent forces; velocity- or time-dependent forces make it first
  order. Advancing the time alone would not restore second order for velocity-dependent forces
  such as drag and would break parity, and Tracker never selects Verlet, so this is recorded as a
  method limitation rather than fixed (pinned by `VerletUsesTheStepStartTimeForItsSecondRate`).
  The embedded pairs report `DID_NOT_CONVERGE` when only the tenth attempt succeeds and always
  advance with the last attempted step. `RK45MultiStep.setStepSize` overwrites its own minimum
  with the full interval. LeapFrog steps the live state back for its prior-state estimate and then
  restores it, so a callback that reads `state()` sees what the Java ODE sees.
- **Not reproduced.** Console messages, the static message counters and optional exceptions are
  replaced by statuses. A state-length change reinitializes the solver. In OSP only RK4, Ralston2,
  Heun3, Butcher5, Fehlberg8, the Adams methods, Verlet and LeapFrog reinitialize when `step()`
  sees a new length; the other classes keep their old equation count and ignore the new components
  or fail with an index error. Factory names are lowercased as ASCII; Java's locale-dependent
  `toLowerCase` rejects `MULTISTEP` in a Turkish locale.
- **For C5 ([MoTrack #192](https://github.com/mgradwohl/MoTrack/issues/192), upstream
  [tracker#76](https://github.com/OpenSourcePhysics/tracker/issues/76), open).**
  `DynamicSystem.getRate` evaluates each particle's external forces from the committed state (the
  solver's state array), not the intermediate stage state, which reduces RK4 to first order for
  state-dependent external forces. A rate callback can reproduce this by reading `solver.state()`;
  correcting it with independent expectations belongs to C5's model slice.

## Floating point and the Java oracle

Each expression keeps Java's evaluation order, `Math.max`/`Math.min` NaN and signed-zero rules and
`Float.MIN_VALUE`. Java never fuses multiply-add, so the library and its test are compiled with
`MOTRACK_JAVA_FP_OPTIONS` (`-ffp-contract=off`; Clang otherwise contracts on arm64). Fixed-step
results are therefore identical to Java, which the macOS arm64 CI job confirms. The adaptive
solvers call `pow`; Java's x86-64 intrinsic misrounds about 0.07% of these inputs, its arm64 path
uses fdlibm, and C++ libraries differ again, so adaptive results
can differ in the last bit between platforms in Java and in MoTrack.

[`tools/reference/OdeOracle.java`](../tools/reference/OdeOracle.java) runs the pinned OSP classes,
created through `ODESolverFactory` or by class name like Tracker, on six systems: harmonic
oscillator, projectile with quadratic drag, Kepler two-body (Tracker's system layout), a stiff
linear system with eigenvalues −1 and −100, quadratic forcing from t = −0.5, and an acceleration
that turns NaN. The 276 cases cover two step sizes or tolerances per solver and system, backward,
zero and default/`setStepSize` setups, tolerance floors, unstable overflow and too many substeps,
forward and backward. It also reads Tracker's default solver, Model Builder solver list,
`tracePtsPerStep` and `iterationsPerStep` from the JAR (constructing a `DynamicParticle`
headlessly); the step expression, step counts and frame handling above come from the 6.3.5 source.
[`tests/science/ode-reference.json`](../tests/science/ode-reference.json) stores step counts, rate
evaluations, error codes and states, so `motrack_tests` needs no Java.
`python tools/corpus.py ode-oracle --java <java>` regenerates it in CI, with x86-64 Java on Linux
and arm64 Java (whose `pow` is fdlibm) on macOS, and compares; `--record` replaces it for review.

Integer data (step counts, rate evaluations, error codes, so the accept/reject sequence) must match
exactly and fixed-step values must be identical. The step-size controller carries a last-bit `pow`
difference forward. Against the x86-64 Java fixture, MoTrack on Windows was bit-identical except for
four stiff Dormand-Prince cases (at most 2e-8 relative, 2e-11 absolute on a decayed component).
Java itself with fdlibm's `pow` (HotSpot with its libm intrinsics disabled, as on arm64) differed
in 14 adaptive cases, at most 4e-10 relative on the non-stiff Kepler orbit and 6e-8 on the stiff
system, with identical integer data.
Non-stiff adaptive values therefore allow 1e-8 relative (1e-11 absolute against O(1)
trajectories), which still rejects a wrong coefficient or branch at the truncation-error level of
the 1e-6 cases (at least 1e-7). The stiff system holds the step at its stability limit, where the
controller amplifies most, so its adaptive cases allow 1e-6 relative (1e-10 absolute).

Independent tests guard against a consistently wrong port: the convergence order of every
fixed-step method, exact oscillator and forced solutions, the drag terminal-velocity solution, a
Kepler orbit returning after one period with conserved momentum, energy and angular momentum, the
exact stiff solution, Euler instability and overflow, and multistep frame times.

## Driving the kernel from C5

A dynamic particle will create `TRACKER_DEFAULT_ODE_SOLVER` (or a legacy class name) with the
`{x, vx, y, vy, t}` state and a rate that evaluates the K1 force expressions divided by the mass.
On reset it writes the initial state, calls `initialize(dt)` with Tracker's two-rounding step
expression above, and advances with `advance(count)` using Tracker's integer step counts, saving
the state at each frame and restoring it by writing `state()` when the end frame is trimmed. The
rate callback, including the expression evaluator it calls, must be compiled with
`MOTRACK_JAVA_FP_OPTIONS` too; otherwise arm64 contraction changes the rates and Java parity is
lost. Models run on immutable document snapshots off the GUI thread and publish results by
request generation, like other long operations.
