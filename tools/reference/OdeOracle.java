import java.lang.reflect.Field;
import java.math.BigDecimal;
import java.math.MathContext;
import java.math.RoundingMode;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Supplier;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.numerics.ODE;
import org.opensourcephysics.numerics.ODEAdaptiveSolver;
import org.opensourcephysics.numerics.ODESolver;
import org.opensourcephysics.numerics.ODESolverFactory;

/** Integrates fixed test systems with the actual OSP ODE solvers in the pinned Tracker 6.3.5 JAR.
 *
 * The rate functions are written with the same operation order as tests/science/ode_solver_test.cpp,
 * using only arithmetic and sqrt so that Java and C++ round identically. Solvers are created the
 * way Tracker can create them: through ODESolverFactory, or by class name and the ODE constructor
 * (DynamicParticle.setSolver). See docs/ode-solvers.md for the case matrix and tolerances.
 */
public final class OdeOracle {
    private static final String NUMERICS = "org.opensourcephysics.numerics.";
    private static final double FRAME = 0.1; // multistep wrappers advance one "frame" per step

    /** An ODE that owns its state, like DynamicParticle, and counts rate evaluations. */
    private abstract static class TestSystem implements ODE {
        final double[] state;
        long rates;

        TestSystem(double... initial) {
            state = initial.clone();
        }

        @Override
        public double[] getState() {
            return state;
        }

        @Override
        public void getRate(double[] s, double[] r) {
            rates++;
            rate(s, r);
        }

        abstract void rate(double[] s, double[] r);
    }

    // Harmonic oscillator {x, vx, t}: x'' = -4 x.
    private static final double OSCILLATOR_OMEGA_SQUARED = 4.0;

    private static final class Oscillator extends TestSystem {
        Oscillator() {
            super(1.0, 0.0, 0.0);
        }

        @Override
        void rate(double[] s, double[] r) {
            r[0] = s[1];
            r[1] = -OSCILLATOR_OMEGA_SQUARED * s[0];
            r[2] = 1.0;
        }
    }

    // Projectile with quadratic drag {x, vx, y, vy, t}, Tracker's Cartesian layout.
    private static final double DRAG_GRAVITY = 9.8;
    private static final double DRAG_COEFFICIENT = 0.1;

    private static final class Drag extends TestSystem {
        Drag() {
            super(0.0, 10.0, 0.0, 10.0, 0.0);
        }

        @Override
        void rate(double[] s, double[] r) {
            double speed = Math.sqrt(s[1] * s[1] + s[3] * s[3]);
            r[0] = s[1];
            r[1] = -DRAG_COEFFICIENT * speed * s[1];
            r[2] = s[3];
            r[3] = -DRAG_GRAVITY - DRAG_COEFFICIENT * speed * s[3];
            r[4] = 1.0;
        }
    }

    // Kepler two-body {x1, vx1, y1, vy1, x2, vx2, y2, vy2, t}, Tracker's DynamicSystem layout.
    // G (m1 + m2) = pi^2 gives the e = 0.5, a = 1 relative orbit a period of exactly 2.
    private static final double KEPLER_G = Math.PI * Math.PI / 4.0;
    private static final double KEPLER_M1 = 3.0;
    private static final double KEPLER_M2 = 1.0;
    private static final double KEPLER_PERIAPSIS_SPEED = Math.PI * Math.sqrt(3.0);

    private static final class Kepler extends TestSystem {
        Kepler() {
            super(0.125, 0.0, 0.0, 0.25 * KEPLER_PERIAPSIS_SPEED, -0.375, 0.0, 0.0, -0.75 * KEPLER_PERIAPSIS_SPEED, 0.0);
        }

        @Override
        void rate(double[] s, double[] r) {
            double dx = s[0] - s[4];
            double dy = s[2] - s[6];
            double r2 = dx * dx + dy * dy;
            double r3 = r2 * Math.sqrt(r2);
            double f = KEPLER_G * KEPLER_M1 * KEPLER_M2 / r3;
            double fx = -f * dx;
            double fy = -f * dy;
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
    }

    // Stiff linear system {y1, y2, t} with eigenvalues -1 and -100.
    private static final class Stiff extends TestSystem {
        Stiff() {
            super(2.0, 0.0, 0.0);
        }

        @Override
        void rate(double[] s, double[] r) {
            r[0] = -50.5 * s[0] + 49.5 * s[1];
            r[1] = 49.5 * s[0] - 50.5 * s[1];
            r[2] = 1.0;
        }
    }

    // Time-dependent forcing {x, vx, t} from a negative start time: x'' = -9 x + 2 t - t^2.
    private static final double FORCED_OMEGA_SQUARED = 9.0;
    private static final double FORCED_LINEAR = 2.0;
    private static final double FORCED_QUADRATIC = -1.0;

    private static final class Forced extends TestSystem {
        Forced() {
            super(0.5, 0.0, -0.5);
        }

        @Override
        void rate(double[] s, double[] r) {
            r[0] = s[1];
            r[1] = -FORCED_OMEGA_SQUARED * s[0] + FORCED_LINEAR * s[2] + FORCED_QUADRATIC * s[2] * s[2];
            r[2] = 1.0;
        }
    }

    // Oscillator whose acceleration becomes NaN from t = 0.25 (degenerate-input behavior).
    private static final double NAN_RATE_START = 0.25;

    private static final class NanRate extends TestSystem {
        NanRate() {
            super(1.0, 0.0, 0.0);
        }

        @Override
        void rate(double[] s, double[] r) {
            r[0] = s[1];
            r[1] = s[2] < NAN_RATE_START ? -OSCILLATOR_OMEGA_SQUARED * s[0] : Double.NaN;
            r[2] = 1.0;
        }
    }

    private record SystemSpec(String id, Supplier<TestSystem> factory, double duration, double[] steps, String description) {}

    private record SolverSpec(String id, String className, String factoryName, boolean adaptive, boolean multistep) {}

    private record CaseSpec(String system, String solver, String variant, String setup, Double step, Double tolerance, int steps,
                            int[] checkpoints) {}

    private static final List<SystemSpec> SYSTEMS = List.of(
        new SystemSpec("oscillator", Oscillator::new, 2.0, new double[] {0.01, 0.001}, "x'' = -4 x; exact x = cos 2t"),
        new SystemSpec("drag", Drag::new, 1.5, new double[] {0.01, 0.001}, "projectile with quadratic drag k = 0.1, g = 9.8"),
        new SystemSpec("kepler", Kepler::new, 2.0, new double[] {0.01, 0.001}, "two-body e = 0.5 orbit, period 2"),
        new SystemSpec("stiff", Stiff::new, 1.0, new double[] {0.01, 0.025}, "y' = A y, eigenvalues -1 and -100"),
        new SystemSpec("forced", Forced::new, 2.0, new double[] {0.01, 0.001}, "x'' = -9 x + 2 t - t^2 from t = -0.5"),
        new SystemSpec("nan-rate", NanRate::new, 0.5, new double[] {0.1}, "oscillator whose acceleration is NaN from t = 0.25"));

    private static final List<SolverSpec> SOLVERS = List.of(
        new SolverSpec("euler", "Euler", "euler", false, false),
        new SolverSpec("eulerrichardson", "EulerRichardson", "eulerrichardson", false, false),
        new SolverSpec("ralston2", "Ralston2", "ralston2", false, false),
        new SolverSpec("heun3", "Heun3", "heun3", false, false),
        new SolverSpec("rk4", "RK4", "rk4", false, false),
        new SolverSpec("butcher5", "Butcher5", "butcher5", false, false),
        new SolverSpec("fehlberg8", "Fehlberg8", "fehlberg8", false, false),
        new SolverSpec("adams4", "Adams4", "adams4", false, false),
        new SolverSpec("adams5", "Adams5", "adams5", false, false),
        new SolverSpec("adams6", "Adams6", "adams6", false, false),
        new SolverSpec("verlet", "Verlet", "verlet", false, false),
        new SolverSpec("leapfrog", "LeapFrog", null, false, false),
        new SolverSpec("cashkarp45", "CashKarp45", "cashkarp45", true, false),
        new SolverSpec("dormandprince45", "DormandPrince45", "dormandprince45", true, false),
        new SolverSpec("rk45", "RK45", null, true, false),
        new SolverSpec("rk45multistep", "RK45MultiStep", null, true, true),
        new SolverSpec("multistep", "ODEMultistepSolver", "multistep", true, true));

    public static void main(String[] args) {
        try {
            if (args.length != 1) throw new IllegalArgumentException("Usage: OdeOracle output.json");
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            StringBuilder json = new StringBuilder();
            json.append("{\"schema_version\":1,\"reference_version\":").append(string(OSPRuntime.VERSION));
            json.append(",\"tracker\":").append(trackerDriving());
            json.append(",\"systems\":[");
            for (int i = 0; i < SYSTEMS.size(); i++) {
                SystemSpec system = SYSTEMS.get(i);
                if (i > 0) json.append(',');
                json.append("{\"id\":").append(string(system.id())).append(",\"description\":").append(string(system.description()))
                    .append(",\"initial_state\":").append(numbers(system.factory().get().state))
                    .append(",\"duration\":").append(number(system.duration())).append('}');
            }
            json.append("],\"solvers\":[");
            for (int i = 0; i < SOLVERS.size(); i++) {
                SolverSpec solver = SOLVERS.get(i);
                if (i > 0) json.append(',');
                json.append("{\"id\":").append(string(solver.id())).append(",\"class\":").append(string(NUMERICS + solver.className()))
                    .append(",\"factory\":").append(solver.factoryName() == null ? "null" : string(solver.factoryName()))
                    .append(",\"adaptive\":").append(solver.adaptive()).append('}');
            }
            json.append("],\"cases\":[");
            List<CaseSpec> cases = cases();
            for (int i = 0; i < cases.size(); i++) {
                if (i > 0) json.append(',');
                json.append(run(cases.get(i)));
            }
            json.append("]}\n");
            Files.writeString(Path.of(args[0]), json, StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }

    /** Tracker's default solver, solver list and stepping constants, read from the pinned classes. */
    private static String trackerDriving() throws ReflectiveOperationException {
        String tracker = "org.opensourcephysics.cabrillo.tracker.";
        Class<?> builder = Class.forName(tracker + "ModelBuilder");
        Field names = builder.getDeclaredField("solverClassNames");
        names.setAccessible(true);
        String[] solvers = (String[]) names.get(null);
        Class<?> model = Class.forName(tracker + "ParticleModel");
        Field tracePoints = model.getDeclaredField("tracePtsPerStep");
        tracePoints.setAccessible(true);
        // A new DynamicParticle (headless, without a TrackerPanel) holds its default solver.
        Class<?> dynamic = Class.forName(tracker + "DynamicParticle");
        Object particle = dynamic.getDeclaredConstructor().newInstance();
        Field solver = dynamic.getDeclaredField("solver");
        solver.setAccessible(true);
        Field iterations = dynamic.getDeclaredField("iterationsPerStep");
        iterations.setAccessible(true);
        StringBuilder json = new StringBuilder("{\"default_solver\":").append(string(solver.get(particle).getClass().getName()));
        json.append(",\"model_builder_solvers\":[");
        for (int i = 0; i < solvers.length; i++) {
            if (i > 0) json.append(',');
            json.append(string(solvers[i]));
        }
        return json.append("],\"trace_points_per_step\":").append(tracePoints.getInt(null))
            .append(",\"iterations_per_step\":").append(iterations.getInt(particle)).append('}').toString();
    }

    private static List<CaseSpec> cases() {
        List<CaseSpec> cases = new ArrayList<>();
        for (SystemSpec system : SYSTEMS) {
            if (system.id().equals("nan-rate")) continue;
            for (SolverSpec solver : SOLVERS) {
                if (solver.multistep()) {
                    for (double tolerance : new double[] {1e-6, 1e-10}) {
                        int steps = (int) Math.round(system.duration() / FRAME);
                        cases.add(new CaseSpec(system.id(), solver.id(), "tol=" + tolerance, "initialize", FRAME, tolerance, steps,
                            checkpoints(tolerance == 1e-6, steps)));
                    }
                } else if (solver.adaptive()) {
                    for (double tolerance : new double[] {1e-6, 1e-9}) {
                        cases.add(new CaseSpec(system.id(), solver.id(), "tol=" + tolerance, "initialize", system.steps()[0], tolerance, 40,
                            checkpoints(tolerance == 1e-6, 40)));
                    }
                } else {
                    for (double step : system.steps()) {
                        int steps = (int) Math.round(system.duration() / step);
                        cases.add(new CaseSpec(system.id(), solver.id(), "h=" + step, "initialize", step, null, steps,
                            checkpoints(step == system.steps()[0], steps)));
                    }
                }
            }
        }
        for (SolverSpec solver : SOLVERS) {
            // Backward integration, the constructor's defaults, setStepSize without initialize and a zero step.
            double backward = solver.multistep() ? -FRAME : -0.01;
            int backwardSteps = solver.multistep() ? 20 : solver.adaptive() ? 40 : 200;
            // A tight multistep tolerance makes minus() shorten its last substep of each interval.
            Double backwardTolerance = solver.multistep() ? Double.valueOf(1e-10) : solver.adaptive() ? Double.valueOf(1e-6) : null;
            cases.add(new CaseSpec("oscillator", solver.id(), "backward", "initialize", backward, backwardTolerance,
                backwardSteps, new int[] {1, backwardSteps}));
            cases.add(new CaseSpec("oscillator", solver.id(), "defaults", "construct", null, null, 5, new int[] {0, 1, 5}));
            cases.add(new CaseSpec("oscillator", solver.id(), "set-step", "set_step", 0.05, null, 5, new int[] {0, 1, 5}));
            cases.add(new CaseSpec("oscillator", solver.id(), "zero-step", "initialize", 0.0, null, 3, new int[] {0, 3}));
            cases.add(new CaseSpec("nan-rate", solver.id(), "nan", "initialize", 0.1, null, 5, new int[] {1, 5}));
            if (solver.adaptive()) {
                // Below the 1e-12 floor: DormandPrince45 clamps, CashKarp45 only warns.
                cases.add(new CaseSpec("oscillator", solver.id(), "tol=1e-14", "initialize", solver.multistep() ? FRAME : 0.01, 1e-14, 10,
                    new int[] {0, 10}));
            }
            if (solver.multistep()) {
                // A stiff interval needs more than 200 substeps: DID_NOT_CONVERGE.
                cases.add(new CaseSpec("stiff", solver.id(), "too-many-substeps", "initialize", 10.0, 1e-6, 2, new int[] {1, 2}));
                // The same failure backward on a non-stiff interval: RK45MultiStep stops after its 201st
                // substep, while ODEMultistepSolver.minus() keeps going and finishes the interval
                // (MoTrack stops there too, MoTrack #179; see tests/science/ode_solver_test.cpp).
                cases.add(new CaseSpec("oscillator", solver.id(), "backward-too-many-substeps", "initialize", -10.0, 1e-10, 1,
                    new int[] {1}));
            } else if (!solver.adaptive()) {
                // An unstable fixed step overflows to infinity and then NaN.
                cases.add(new CaseSpec("stiff", solver.id(), "unstable", "initialize", 0.1, null, 400, new int[] {1, 400}));
            }
        }
        return cases;
    }

    /** The first variant also records step 1; further variants record only the final state. */
    private static int[] checkpoints(boolean first, int steps) {
        return first ? new int[] {1, steps} : new int[] {steps};
    }

    private static ODESolver create(SolverSpec spec, ODE ode) throws ReflectiveOperationException {
        if (spec.factoryName() != null) {
            ODESolver solver = ODESolverFactory.createODESolver(ode, spec.factoryName());
            if (solver == null || !solver.getClass().getName().equals(NUMERICS + spec.className()))
                throw new IllegalStateException("ODESolverFactory returned an unexpected class for " + spec.factoryName());
            return solver;
        }
        // DynamicParticle.setSolver: Class.forName and the (ODE) constructor.
        return (ODESolver) Class.forName(NUMERICS + spec.className()).getDeclaredConstructor(ODE.class).newInstance(ode);
    }

    private static String run(CaseSpec spec) throws ReflectiveOperationException {
        SystemSpec systemSpec = SYSTEMS.stream().filter(s -> s.id().equals(spec.system())).findFirst().orElseThrow();
        SolverSpec solverSpec = SOLVERS.stream().filter(s -> s.id().equals(spec.solver())).findFirst().orElseThrow();
        TestSystem system = systemSpec.factory().get();
        ODESolver solver = create(solverSpec, system);
        switch (spec.setup()) {
            case "initialize" -> solver.initialize(spec.step());
            case "set_step" -> solver.setStepSize(spec.step());
            case "construct" -> { }
            default -> throw new IllegalArgumentException(spec.setup());
        }
        ODEAdaptiveSolver adaptive = solver instanceof ODEAdaptiveSolver a ? a : null;
        if (spec.tolerance() != null) {
            if (adaptive == null) throw new IllegalStateException("Tolerance for a fixed-step solver");
            adaptive.setTolerance(spec.tolerance());
        }
        StringBuilder json = new StringBuilder();
        json.append("{\"id\":").append(string(spec.system() + "/" + spec.solver() + "/" + spec.variant()))
            .append(",\"system\":").append(string(spec.system())).append(",\"solver\":").append(string(spec.solver()))
            .append(",\"setup\":").append(string(spec.setup())).append(",\"step_size\":").append(nullable(spec.step()))
            .append(",\"tolerance\":").append(nullable(spec.tolerance()))
            .append(",\"effective_tolerance\":").append(adaptive == null ? "null" : number(adaptive.getTolerance()))
            .append(",\"steps\":").append(spec.steps()).append(",\"checkpoints\":[");
        int next = 0;
        double taken = Double.NaN;
        for (int n = 0; n <= spec.steps(); n++) {
            if (n > 0) taken = solver.step();
            if (next < spec.checkpoints().length && spec.checkpoints()[next] == n) {
                if (next > 0) json.append(',');
                json.append("{\"n\":").append(n).append(",\"taken\":").append(n == 0 ? "null" : number(taken))
                    .append(",\"step_size\":").append(number(solver.getStepSize()))
                    .append(",\"error_code\":").append(adaptive == null ? "null" : Integer.toString(adaptive.getErrorCode()))
                    .append(",\"rates\":").append(system.rates).append(",\"state\":").append(numbers(system.state)).append('}');
                next++;
            }
        }
        if (next != spec.checkpoints().length) throw new IllegalStateException("Checkpoint beyond the step count in " + spec);
        return json.append("]}").toString();
    }

    private static String nullable(Double value) {
        return value == null ? "null" : number(value);
    }

    private static String numbers(double[] values) {
        StringBuilder json = new StringBuilder("[");
        for (int i = 0; i < values.length; i++) {
            if (i > 0) json.append(',');
            json.append(number(values[i]));
        }
        return json.append(']').toString();
    }

    /** The shortest decimal that parses back to exactly this double; non-finite values as strings. */
    static String number(double value) {
        if (Double.isNaN(value)) return "\"NaN\"";
        if (Double.isInfinite(value)) return value > 0 ? "\"Infinity\"" : "\"-Infinity\"";
        if (value == 0) return 1 / value < 0 ? "-0.0" : "0.0";
        BigDecimal exact = new BigDecimal(value);
        for (int digits = 1; digits <= 17; digits++) {
            String text = exact.round(new MathContext(digits, RoundingMode.HALF_EVEN)).toString();
            if (Double.parseDouble(text) == value) return text.contains(".") || text.contains("E") ? text : text + ".0";
        }
        throw new IllegalStateException("No round-trip decimal for " + value);
    }

    private static String string(String value) {
        StringBuilder json = new StringBuilder("\"");
        for (char c : value.toCharArray()) {
            if (c == '"' || c == '\\') json.append('\\').append(c);
            else if (c < 0x20) json.append(String.format("\\u%04x", (int) c));
            else json.append(c);
        }
        return json.append('"').toString();
    }
}
