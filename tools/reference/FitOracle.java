import java.awt.Component;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.math.BigDecimal;
import java.math.MathContext;
import java.math.RoundingMode;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.text.NumberFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Random;
import java.util.TreeMap;
import javax.swing.JCheckBox;
import javax.swing.JCheckBoxMenuItem;
import javax.swing.JTextField;
import org.opensourcephysics.display.Data;
import org.opensourcephysics.display.Dataset;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.display.UncertainFunctionDrawer;
import org.opensourcephysics.numerics.HessianMinimize;
import org.opensourcephysics.numerics.LevenbergMarquardt;
import org.opensourcephysics.numerics.MultiVarFunction;
import org.opensourcephysics.tools.DataTool;
import org.opensourcephysics.tools.DataToolTab;
import org.opensourcephysics.tools.DatasetCurveFitter;
import org.opensourcephysics.tools.FitBuilder;
import org.opensourcephysics.tools.KnownFunction;
import org.opensourcephysics.tools.KnownPolynomial;
import org.opensourcephysics.tools.UserFunction;
import sun.misc.Unsafe;

/** Runs Tracker 6.3.5's Data Tool curve fitter (OSP DatasetCurveFitter) headless on fixed datasets.
 *
 * The fitter is a Swing panel whose constructor needs a Fit Builder dialog, so the oracle allocates it
 * without a constructor and installs exactly the plain objects its fit() path uses: the real Dataset,
 * default fit list, HessianMinimize, LevenbergMarquardt, drawer and parameter table, plus a tab whose
 * plot ignores status messages. Every fitting decision therefore runs OSP's own code: getScratchParams,
 * the minimizers, the fixed-parameter test functions, getUncertainties and doLinearRegression. The
 * independent expectations (exact least squares, Gauss-Newton minima, known synthetic parameters) are
 * computed here without OSP. See docs/curve-fitting.md for the case matrix and tolerances.
 */
public final class FitOracle {
    private static final Unsafe UNSAFE = unsafe();
    private static final Class<?> FITTER = DatasetCurveFitter.class;

    /** A tab that never repaints; Unsafe-allocated, so the constructor below never runs. */
    static final class QuietTab extends DataToolTab {
        QuietTab() {
            super((Data) null, (DataTool) null);
        }

        @Override
        protected void refreshPlot() {
        }

        final class QuietPlotter extends DataToolPlotter {
            QuietPlotter() {
                super((Dataset) null);
            }

            @Override
            public boolean setMessage(String message, int location) {
                return false;
            }
        }
    }

    // ------------------------------------------------------------------------------------------
    // Fit definitions

    /** Built-in fits in OSP's defaultFits order: MoTrack id, OSP name, synthetic truth and default grid. */
    private record BuiltIn(String id, String ospName, double[] truth, double x0, double step, int count) {}

    private static final List<BuiltIn> BUILT_INS = List.of(
        new BuiltIn("line", "Poly1", new double[] {2.5, -1.25}, 0.0, 0.1, 21),
        new BuiltIn("parabola", "Poly2", new double[] {-4.9, 3.0, 1.5}, 0.0, 0.05, 21),
        new BuiltIn("cubic", "Poly3", new double[] {0.5, -1.2, 0.3, 2.0}, -1.0, 0.125, 25),
        new BuiltIn("gaussian", "Gaussian", new double[] {3.0, 1.2, 0.4}, 0.0, 0.1, 26),
        new BuiltIn("exponential", "Exponential", new double[] {2.0, -1.5, 0.5}, 0.0, 0.125, 25),
        new BuiltIn("sinusoid", "Sinusoid", new double[] {1.5, 4.0, 0.7, 0.25}, 0.0, 0.1, 31),
        new BuiltIn("dampedsine", "DampedSine", new double[] {2.0, 6.0, 0.3, 0.1, -0.8}, 0.0, 0.0625, 49),
        new BuiltIn("power", "Power", new double[] {1.7, 1.5}, 0.25, 0.125, 23),
        new BuiltIn("log", "Log", new double[] {0.8, 0.3}, 0.5, 0.25, 19));

    /** A user-defined fit (FitBuilder UserFunction) with its MoTrack id. */
    private record User(String id, String name, String expression, String[] parameters, double[] values) {}

    private static final User USER_LINE = new User("user-line", "Fit", "m*x+b", new String[] {"m", "b"}, new double[] {1, 0});
    private static final User USER_CHARGE =
        new User("user-charge", "Charge", "A*(1-exp(-x/T))", new String[] {"A", "T"}, new double[] {1, 1});
    private static final User USER_LORENTZ =
        new User("user-lorentz", "Lorentz", "A/(1+((x-B)/C)^2)", new String[] {"A", "B", "C"}, new double[] {1, 1, 0.5});
    private static final User USER_COS =
        new User("user-cos", "Cosine", "a*cos(b*x)+c", new String[] {"a", "b", "c"}, new double[] {1, 2, 0});
    private static final User USER_COS_RENAMED =
        new User("user-cos-renamed", "Cosine", "a*cos(b*x)+k", new String[] {"a", "b", "k"}, new double[] {1, 2, 0});
    private static final User USER_DAMPED_RENAMED = new User("user-damped-renamed", "Damped",
        "A*exp(F*x)*sin(B*x+C)+D", new String[] {"A", "B", "C", "D", "F"}, new double[] {1, 1, 0, 0, 0});
    private static final User USER_CONSTANT = new User("user-constant", "Twice", "2*x", new String[0], new double[0]);
    private static final User USER_ROOT =
        new User("user-root", "Root", "A*sqrt(x-B)", new String[] {"A", "B"}, new double[] {1, 2});
    private static final List<User> USERS =
        List.of(USER_LINE, USER_CHARGE, USER_LORENTZ, USER_COS, USER_COS_RENAMED, USER_DAMPED_RENAMED, USER_CONSTANT, USER_ROOT);

    /** Evaluates a synthetic model with the same expression as its fit function. The data and independent
     *  expectations use StrictMath (fdlibm) so that every JVM generates the same inputs; only the fits run
     *  through OSP's Math calls. */
    static double model(String id, double[] p, double x) {
        switch (id) {
            case "line": return p[0] * x + p[1];
            case "parabola": return p[0] * x * x + p[1] * x + p[2];
            case "cubic": return p[0] * x * x * x + p[1] * x * x + p[2] * x + p[3];
            case "gaussian": return p[0] * StrictMath.exp(-(x - p[1]) * (x - p[1]) / (2 * p[2] * p[2]));
            case "exponential": return p[0] * StrictMath.exp(p[1] * x) + p[2];
            case "sinusoid": return p[0] * StrictMath.sin(p[1] * x + p[2]) + p[3];
            case "dampedsine":
            case "user-damped-renamed": return p[0] * StrictMath.exp(p[4] * x) * StrictMath.sin(p[1] * x + p[2]) + p[3];
            case "power": return p[0] * StrictMath.pow(x, p[1]);
            case "log": return p[0] * StrictMath.log(x) + p[1];
            case "user-line": return p[0] * x + p[1];
            case "user-charge": return p[0] * (1 - StrictMath.exp(-x / p[1]));
            case "user-lorentz": return p[0] / (1 + ((x - p[1]) / p[2]) * ((x - p[1]) / p[2]));
            case "user-cos":
            case "user-cos-renamed": return p[0] * StrictMath.cos(p[1] * x) + p[2];
            default: throw new IllegalArgumentException(id);
        }
    }

    /** Analytic derivatives for the independent Gauss-Newton minimizer. */
    static double[] gradient(String id, double[] p, double x) {
        switch (id) {
            case "gaussian": {
                double d = x - p[1];
                double g = StrictMath.exp(-d * d / (2 * p[2] * p[2]));
                return new double[] {g, p[0] * g * d / (p[2] * p[2]), p[0] * g * d * d / (p[2] * p[2] * p[2])};
            }
            case "exponential": {
                double e = StrictMath.exp(p[1] * x);
                return new double[] {e, p[0] * x * e, 1};
            }
            case "sinusoid": {
                double s = StrictMath.sin(p[1] * x + p[2]);
                double c = StrictMath.cos(p[1] * x + p[2]);
                return new double[] {s, p[0] * x * c, p[0] * c, 1};
            }
            case "dampedsine":
            case "user-damped-renamed": {
                double e = StrictMath.exp(p[4] * x);
                double s = StrictMath.sin(p[1] * x + p[2]);
                double c = StrictMath.cos(p[1] * x + p[2]);
                return new double[] {e * s, p[0] * e * x * c, p[0] * e * c, 1, p[0] * x * e * s};
            }
            case "power": {
                double v = StrictMath.pow(x, p[1]);
                return new double[] {v, p[0] * v * StrictMath.log(x)};
            }
            case "user-charge": {
                double e = StrictMath.exp(-x / p[1]);
                return new double[] {1 - e, -p[0] * e * x / (p[1] * p[1])};
            }
            case "user-lorentz": {
                double u = (x - p[1]) / p[2];
                double q = 1 + u * u;
                double dq = 2 * p[0] * u / (q * q * p[2]);
                return new double[] {1 / q, dq, dq * u};
            }
            case "user-cos":
            case "user-cos-renamed": {
                return new double[] {StrictMath.cos(p[1] * x), -p[0] * x * StrictMath.sin(p[1] * x), 1};
            }
            default: throw new IllegalArgumentException(id);
        }
    }

    static int polynomialDegree(String id) {
        switch (id) {
            case "line": case "user-line": return 1;
            case "parabola": return 2;
            case "cubic": return 3;
            default: return 0;
        }
    }

    // ------------------------------------------------------------------------------------------
    // Cases

    /** The independent expectation recorded with a case. */
    private record Expect(String source, double[] parameters, double[] uncertainties, double parameterRelative,
                          double parameterAbsolute, double uncertaintyRelative) {}

    private record CaseSpec(String id, String group, String fit, double[] x, double[] y, double[] initial, boolean[] fixed,
                            boolean autofit, boolean scratch, String agreement, Expect expect, String defect, String intendedFit) {}

    private static final List<CaseSpec> CASES = new ArrayList<>();
    /** Tracker.start sets Dataset.maxPointsMultiplier = 32, so Tracker's data sets keep 32 * 16384 rows. */
    private static final double TRACKER_MAX_POINTS_MULTIPLIER = 32;
    private static final int CAPACITY_ROWS = 32 * 16384 + 100;
    private static final int CAPACITY_HEAD = 100;

    private static BuiltIn builtIn(String id) {
        return BUILT_INS.stream().filter(b -> b.id().equals(id)).findFirst().orElseThrow();
    }

    private static User user(String id) {
        return USERS.stream().filter(u -> u.id().equals(id)).findFirst().orElse(null);
    }

    private static double[] grid(double x0, double step, int count) {
        double[] x = new double[count];
        for (int i = 0; i < count; i++) x[i] = x0 + i * step;
        return x;
    }

    private static double[] evaluate(String id, double[] p, double[] x) {
        double[] y = new double[x.length];
        for (int i = 0; i < x.length; i++) y[i] = model(id, p, x[i]);
        return y;
    }

    /** Adds Gaussian noise with a fixed seed; the values are recorded, so only Java needs to draw them. */
    private static double[] noisy(double[] y, double sigma, long seed) {
        Random random = new Random(seed);
        double[] result = y.clone();
        for (int i = 0; i < y.length; i++) result[i] = y[i] + sigma * random.nextGaussian();
        return result;
    }

    private static double range(double[] y) {
        double min = Double.MAX_VALUE, max = -Double.MAX_VALUE;
        for (double v : y) {
            min = Math.min(min, v);
            max = Math.max(max, v);
        }
        return max - min;
    }

    /** Agreement class: arithmetic-only fits are bitwise; Math.exp/sin/log/pow fits carry last-bit differences. */
    private static String agreement(String fit) {
        switch (fit) {
            // A/(1+((x-B)/C)^2) is arithmetic too: pow(u, 2) is u*u in fdlibm and HotSpot.
            case "line": case "parabola": case "user-line": case "user-constant": case "user-lorentz": return "exact";
            case "cubic": return "polynomial";
            default: return "transcendental";
        }
    }

    private static void add(String id, String group, String fit, double[] x, double[] y, double[] initial, boolean[] fixed,
                            boolean autofit, boolean scratch, Expect expect, String defect, String intendedFit) {
        CASES.add(new CaseSpec(id, group, fit, x, y, initial, fixed, autofit, scratch, agreement(fit), expect, defect, intendedFit));
    }

    private static void add(String id, String group, String fit, double[] x, double[] y, Expect expect) {
        add(id, group, fit, x, y, null, null, true, true, expect, null, null);
    }

    private static boolean[] fixedAt(int count, int index) {
        boolean[] fixed = new boolean[count];
        fixed[index] = true;
        return fixed;
    }

    private static double[] perturbed(double[] p, double factor) {
        double[] result = p.clone();
        for (int i = 0; i < result.length; i++) result[i] = result[i] * factor + (i % 2 == 0 ? 0.05 : -0.05);
        return result;
    }

    // Tolerances of the independent expectations (justified in docs/curve-fitting.md).
    private static final double TRUTH_RELATIVE = 1e-6;
    private static final double TRUTH_ABSOLUTE = 1e-7;
    private static final double LEAST_SQUARES_RELATIVE = 1e-6;
    private static final double LEAST_SQUARES_ABSOLUTE = 1e-8;
    private static final double LINEAR_UNCERTAINTY_RELATIVE = 1e-4;
    private static final double NONLINEAR_UNCERTAINTY_RELATIVE = 0.05;

    private static void curatedCases() {
        long seed = 1000;
        for (BuiltIn b : BUILT_INS) {
            double[] x = grid(b.x0(), b.step(), b.count());
            double[] exact = evaluate(b.id(), b.truth(), x);
            double sigma = 0.02 * range(exact);
            boolean linear = polynomialDegree(b.id()) > 0 || b.id().equals("log");
            double uncertaintyTolerance = linear ? LINEAR_UNCERTAINTY_RELATIVE : NONLINEAR_UNCERTAINTY_RELATIVE;
            int p = b.truth().length;
            add(b.id() + "/exact", "exact", b.id(), x, exact,
                new Expect("truth", b.truth(), null, TRUTH_RELATIVE, TRUTH_ABSOLUTE, 0));
            double[] noisy = noisy(exact, sigma, seed++);
            add(b.id() + "/noisy", "noisy", b.id(), x, noisy, leastSquares(b.id(), x, noisy, b.truth(), null, uncertaintyTolerance));
            double[] outlier = noisy.clone();
            outlier[x.length / 3] += 8 * sigma;
            // From its own initial guess (E = 0) the damped-sine fit does not converge with this outlier.
            add(b.id() + "/outlier", "outlier", b.id(), x, outlier, b.id().equals("dampedsine") ? null
                : leastSquares(b.id(), x, outlier, b.truth(), null, uncertaintyTolerance));
            // Data order only matters to the initial-guess sort.
            Integer[] order = new Integer[x.length];
            for (int i = 0; i < order.length; i++) order[i] = i;
            java.util.Collections.shuffle(Arrays.asList(order), new Random(seed++));
            double[] sx = new double[x.length], sy = new double[x.length];
            for (int i = 0; i < order.length; i++) {
                sx[i] = x[order[i]];
                sy[i] = noisy[order[i]];
            }
            add(b.id() + "/shuffled", "shuffled", b.id(), sx, sy, leastSquares(b.id(), sx, sy, b.truth(), null, uncertaintyTolerance));
            // Repeated x: OSP's initial-guess TreeMap keeps one y per x and pads its arrays with zeros.
            double[] dx = new double[2 * x.length], dy = new double[2 * x.length];
            double[] second = noisy(exact, sigma, seed++);
            for (int i = 0; i < x.length; i++) {
                dx[2 * i] = dx[2 * i + 1] = x[i];
                dy[2 * i] = noisy[i];
                dy[2 * i + 1] = second[i];
            }
            add(b.id() + "/duplicate-x", "duplicate-x", b.id(), dx, dy, null, null, true, true, b.id().equals("dampedsine") ? null
                : leastSquares(b.id(), dx, dy, b.truth(), null, uncertaintyTolerance), "duplicate-x-initial-guess", null);
            // Exactly as many points as parameters, then one fewer.
            int stride = x.length / p;
            double[] mx = new double[p], my = new double[p];
            for (int i = 0; i < p; i++) {
                mx[i] = x[i * stride];
                my[i] = exact[i * stride];
            }
            add(b.id() + "/minimum-points", "few-points", b.id(), mx, my,
                linear ? new Expect("truth", b.truth(), null, 1e-9, 1e-9, 0) : null);
            add(b.id() + "/too-few-points", "few-points", b.id(), Arrays.copyOf(mx, p - 1), Arrays.copyOf(my, p - 1), null);
            // One fixed parameter at its true value; the others are fitted from scratch.
            int fixedIndex = b.id().equals("dampedsine") ? 3 : p - 1;
            double[] start = defaults(b.id());
            start[fixedIndex] = b.truth()[fixedIndex];
            boolean[] fixed = fixedAt(p, fixedIndex);
            add(b.id() + "/fixed", "fixed", b.id(), x, noisy, start, fixed, true, true, b.id().equals("dampedsine") ? null
                : leastSquares(b.id(), x, noisy, b.truth(), fixed, uncertaintyTolerance), null, null);
            // Refit from current values (Tracker's data-change refit) and a manual (autofit off) evaluation.
            double[] current = perturbed(b.truth(), 1.05);
            add(b.id() + "/current", "current", b.id(), x, noisy, current, new boolean[p], true, false,
                leastSquares(b.id(), x, noisy, b.truth(), null, uncertaintyTolerance), null, null);
            add(b.id() + "/manual", "manual", b.id(), x, noisy, current, new boolean[p], false, false, null, null, null);
        }

        // Dataset validity: NaN y is dropped, while NaN/infinite x and infinite y make Dataset.append
        // compact the arrays without shortening them, so the last points are counted twice.
        BuiltIn line = builtIn("line");
        double[] lx = grid(line.x0(), line.step(), 12);
        double[] ly = noisy(evaluate("line", line.truth(), lx), 0.05, 77);
        double[] nanY = ly.clone();
        nanY[3] = Double.NaN;
        nanY[8] = Double.NaN;
        add("line/nan-y", "validity", "line", lx, nanY, leastSquares("line", lx, nanY, line.truth(), null, LINEAR_UNCERTAINTY_RELATIVE));
        double[] nanX = lx.clone();
        nanX[4] = Double.NaN;
        add("line/nan-x", "validity", "line", nanX, ly, null, null, true, true,
            leastSquares("line", nanX, ly, line.truth(), null, LINEAR_UNCERTAINTY_RELATIVE), "dataset-append-duplicates", null);
        double[] infY = ly.clone();
        infY[2] = Double.POSITIVE_INFINITY;
        add("line/infinite-y", "validity", "line", lx, infY, null, null, true, true,
            leastSquares("line", lx, infY, line.truth(), null, LINEAR_UNCERTAINTY_RELATIVE), "dataset-append-duplicates", null);
        double[] infX = lx.clone();
        infX[0] = Double.NEGATIVE_INFINITY;
        add("gaussian/infinite-x", "validity", "gaussian", infX, evaluate("gaussian", builtIn("gaussian").truth(), lx), null, null,
            true, true, null, "dataset-append-duplicates", null);
        add("line/empty", "validity", "line", new double[0], new double[0], null);
        // More rows than Tracker's data sets keep (524288): the first, deviating rows are dropped. Generated, not
        // stored, and evaluated with Autofit off (y = 2x + 1) so that the oracle stays fast.
        double[] longX = new double[CAPACITY_ROWS];
        double[] longY = new double[CAPACITY_ROWS];
        for (int i = 0; i < CAPACITY_ROWS; i++) {
            longX[i] = i;
            longY[i] = i < CAPACITY_HEAD ? 50.0 : 2.0 * i + 1.0;
        }
        add("line/over-capacity", "validity", "line", longX, longY, new double[] {2.0, 1.0}, null, false, false, null,
            "dataset-capacity", null);
        double[] constant = new double[8];
        Arrays.fill(constant, 1.25);
        add("line/constant-y", "degenerate", "line", grid(0, 0.5, 8), constant, null);
        double[] sameX = new double[6];
        Arrays.fill(sameX, 0.7);
        add("parabola/same-x", "degenerate", "parabola", sameX, grid(1, 0.25, 6), null, null, true, true, null,
            "singular-polynomial", null);
        add("line/same-x", "degenerate", "line", Arrays.copyOf(sameX, 4), grid(1, 0.25, 4), null, null, true, true, null,
            "singular-polynomial", null);
        add("exponential/constant-y", "degenerate", "exponential", grid(0, 0.5, 8), constant, null, null, true, true, null,
            "non-finite-initial-guess", null);
        double[] valley = evaluate("gaussian", new double[] {-2.0, 1.2, 0.4}, grid(0, 0.1, 26));
        for (int i = 0; i < valley.length; i++) valley[i] += 3;
        add("gaussian/valley", "degenerate", "gaussian", grid(0, 0.1, 26), valley, null, null, true, true, null,
            "non-finite-initial-guess", null);
        add("power/negative-x", "degenerate", "power", grid(-1.0, 0.25, 12), noisy(grid(-1.7, 0.4, 12), 0.05, 99), null);
        add("log/nonpositive-x", "degenerate", "log", grid(0, 0.25, 10), noisy(grid(0.1, 0.2, 10), 0.05, 98), null);
        add("gaussian/all-fixed", "fixed", "gaussian", grid(0, 0.1, 26), evaluate("gaussian", builtIn("gaussian").truth(),
            grid(0, 0.1, 26)), new double[] {3.0, 1.1, 0.5}, new boolean[] {true, true, true}, true, false, null, null, null);
        // A zero width is defined in Tracker: the parser divides by 1.0e-128 instead of 0, so the model is A at a data
        // point with x = B and 0 at every other one (MoTrack #374). Fitted from the current values, with C free, with
        // C fixed, and with Autofit off. The model does not depend on B or C there, so Tracker's autofit reports the
        // last vector it evaluated (#306) and rounding-noise uncertainties (#307): "zero-width-model" keeps these
        // cases out of the comparisons with the intended fit.
        double[] zeroWidthX = grid(-1.0, 0.25, 9);
        double[] zeroWidthY = noisy(evaluate("gaussian", new double[] {2.0, 0.1, 0.5}, zeroWidthX), 0.02, 7);
        double[] zeroWidth = {2.0, 0.0, 0.0};
        add("gaussian/zero-width", "degenerate", "gaussian", zeroWidthX, zeroWidthY, zeroWidth, new boolean[3], true, false,
            null, "zero-width-model", null);
        add("gaussian/zero-width-fixed", "fixed", "gaussian", zeroWidthX, zeroWidthY, zeroWidth,
            new boolean[] {false, false, true}, true, false, null, "zero-width-model", null);
        add("gaussian/zero-width-no-autofit", "degenerate", "gaussian", zeroWidthX, zeroWidthY, zeroWidth, new boolean[3],
            false, false, null, null, null);
        double[] negativeStart = defaults("line");
        negativeStart[1] = -1.25;
        add("line/fixed-negative", "fixed", "line", lx, ly, negativeStart, fixedAt(2, 1), true, true,
            leastSquares("line", lx, ly, line.truth(), fixedAt(2, 1), LINEAR_UNCERTAINTY_RELATIVE), null, null);
        double[] parabolaStart = defaults("parabola");
        parabolaStart[0] = -4.9;
        BuiltIn parabola = builtIn("parabola");
        double[] px = grid(parabola.x0(), parabola.step(), parabola.count());
        double[] py = noisy(evaluate("parabola", parabola.truth(), px), 0.03, 55);
        add("parabola/fixed-leading", "fixed", "parabola", px, py, parabolaStart, fixedAt(3, 0), true, true,
            leastSquares("parabola", px, py, parabola.truth(), fixedAt(3, 0), LINEAR_UNCERTAINTY_RELATIVE), null, null);
        double[] cubicStart = defaults("cubic");
        cubicStart[1] = -1.2;
        cubicStart[3] = 2.0;
        BuiltIn cubic = builtIn("cubic");
        double[] cx = grid(cubic.x0(), cubic.step(), cubic.count());
        double[] cy = noisy(evaluate("cubic", cubic.truth(), cx), 0.02, 56);
        boolean[] cubicFixed = {false, true, false, true};
        add("cubic/two-fixed", "fixed", "cubic", cx, cy, cubicStart, cubicFixed, true, true,
            leastSquares("cubic", cx, cy, cubic.truth(), cubicFixed, LINEAR_UNCERTAINTY_RELATIVE), null, null);
        // A large x offset: the normal equations lose most digits of a cubic fit.
        double[] offsetX = grid(100.0, 0.5, 21);
        double[] offsetY = noisy(evaluate("cubic", new double[] {0.002, -0.5, 3.0, 1.0}, offsetX), 0.01, 59);
        add("cubic/large-offset", "conditioning", "cubic", offsetX, offsetY, null, null, true, true,
            leastSquares("cubic", offsetX, offsetY, new double[] {0.002, -0.5, 3.0, 1.0}, null, LINEAR_UNCERTAINTY_RELATIVE),
            "normal-equation-conditioning", null);
        double[] timeX = grid(20.0, 0.04, 26);
        double[] timeY = noisy(evaluate("parabola", new double[] {-4.9, 200.0, -2030.0}, timeX), 0.002, 57);
        add("parabola/time-offset", "conditioning", "parabola", timeX, timeY,
            leastSquares("parabola", timeX, timeY, new double[] {-4.9, 200.0, -2030.0}, null, LINEAR_UNCERTAINTY_RELATIVE));

        // Parameter values are substituted into the expression text for fixed parameters and uncertainty
        // tests: a value written as 1.0E-4 is corrupted when parameter "E" is substituted next.
        BuiltIn damped = builtIn("dampedsine");
        double[] sx = grid(damped.x0(), damped.step(), damped.count());
        double[] dampedTruth = {2.0, 6.0, 0.3, 1.0E-4, -0.8};
        double[] sy = noisy(evaluate("dampedsine", dampedTruth, sx), 0.04, 58);
        double[] dampedStart = perturbed(dampedTruth, 1.02);
        dampedStart[3] = 1.0E-4;
        boolean[] dampedFixed = fixedAt(5, 3);
        add("dampedsine/fixed-exponent-text", "substitution", "dampedsine", sx, sy, dampedStart, dampedFixed, true, false,
            leastSquares("dampedsine", sx, sy, dampedTruth, dampedFixed, NONLINEAR_UNCERTAINTY_RELATIVE), "text-substitution",
            "user-damped-renamed");

        // User-defined fits run through UserFunction and the minimizers, including a linear one.
        double[] ux = grid(0, 0.25, 17);
        double[] uy = noisy(evaluate("user-line", new double[] {1.5, -0.5}, ux), 0.1, 60);
        add("user-line/noisy", "user", "user-line", ux, uy, null, null, true, true,
            leastSquares("user-line", ux, uy, new double[] {1.5, -0.5}, null, LINEAR_UNCERTAINTY_RELATIVE), null, null);
        double[] cy2 = noisy(evaluate("user-charge", new double[] {5.0, 0.8}, ux), 0.05, 61);
        add("user-charge/noisy", "user", "user-charge", ux, cy2, new double[] {4.0, 1.0}, null, true, true,
            leastSquares("user-charge", ux, cy2, new double[] {5.0, 0.8}, null, NONLINEAR_UNCERTAINTY_RELATIVE), null, null);
        // From the default values 1 the Newton steps overflow; whether they end in NaN (and Tracker falls
        // back to Levenberg-Marquardt) depends on the last bits of Math.exp, so only the data handling compares.
        CASES.add(new CaseSpec("user-charge/default-start", "user", "user-charge", ux, cy2, null, null, true, true, "chaotic", null,
            null, null));
        double[] lx2 = grid(-1, 0.125, 25);
        double[] ly2 = noisy(evaluate("user-lorentz", new double[] {2.0, 0.6, 0.3}, lx2), 0.02, 62);
        add("user-lorentz/noisy", "user", "user-lorentz", lx2, ly2, null, null, true, true,
            leastSquares("user-lorentz", lx2, ly2, new double[] {2.0, 0.6, 0.3}, null, NONLINEAR_UNCERTAINTY_RELATIVE), null, null);
        // From this start Newton's last vector is worse than the start although its best is not, so Tracker falls
        // back to Levenberg-Marquardt from Newton's best, which converges to the minimum: a bitwise LM path.
        add("user-lorentz/levenberg-marquardt", "user", "user-lorentz", lx2, ly2, new double[] {0.5, 0, 1}, null, true, false,
            leastSquares("user-lorentz", lx2, ly2, new double[] {2.0, 0.6, 0.3}, null, NONLINEAR_UNCERTAINTY_RELATIVE), null, null);
        double[] cosTruth = {1.5, 2.2, 0.4};
        double[] oy = noisy(evaluate("user-cos", cosTruth, ux), 0.05, 63);
        add("user-cos/uncertainty-text", "substitution", "user-cos", ux, oy, new double[] {1.4, 2.1, 0.3}, null, true, false,
            leastSquares("user-cos", ux, oy, cosTruth, null, NONLINEAR_UNCERTAINTY_RELATIVE), "text-substitution", "user-cos-renamed");
        add("user-cos/fixed-text", "substitution", "user-cos", ux, oy, new double[] {1.4, 2.1, 0.4}, fixedAt(3, 2), true, false,
            leastSquares("user-cos", ux, oy, cosTruth, fixedAt(3, 2), NONLINEAR_UNCERTAINTY_RELATIVE), "text-substitution",
            "user-cos-renamed");
        add("user-constant/no-parameters", "user", "user-constant", ux, uy, null, null, true, true, null, null, null);
        double[] rx = grid(1, 0.25, 12);
        add("user-root/undefined-start", "user", "user-root", rx, noisy(grid(0.1, 0.2, 12), 0.02, 64), null, null, true, true, null,
            null, null);
    }

    /** Seeded random datasets: parameters, grid, order, point count and noise all vary. */
    private static void randomCases() {
        for (BuiltIn b : BUILT_INS) {
            for (int k = 0; k < 4; k++) {
                long seed = 5000L + 37L * BUILT_INS.indexOf(b) + k;
                Random random = new Random(seed);
                int p = b.truth().length;
                double[] params = b.truth().clone();
                for (int i = 0; i < p; i++) params[i] = params[i] * (0.7 + 0.6 * random.nextDouble());
                int count = p + 3 + random.nextInt(28);
                double step = b.step() * (0.5 + random.nextDouble());
                double x0 = b.x0() + (b.id().equals("power") || b.id().equals("log") ? 0.1 * random.nextDouble()
                                                                                        : 0.5 * (random.nextDouble() - 0.5));
                double[] x = grid(x0, step, count);
                double[] y = noisy(evaluate(b.id(), params, x), 0.03 * random.nextDouble() * range(evaluate(b.id(), params, x)),
                    random.nextLong());
                for (int i = count - 1; i > 0; i--) {
                    int j = random.nextInt(i + 1);
                    double t = x[i];
                    x[i] = x[j];
                    x[j] = t;
                    t = y[i];
                    y[i] = y[j];
                    y[j] = t;
                }
                add(b.id() + "/random-" + k, "random", b.id(), x, y, null);
            }
        }
    }

    private static double[] defaults(String id) {
        for (KnownFunction f : defaultFits()) {
            if (f.getName().equals(builtIn(id).ospName())) {
                double[] values = new double[f.getParameterCount()];
                for (int i = 0; i < values.length; i++) values[i] = f.getParameterValue(i);
                return values;
            }
        }
        throw new IllegalArgumentException(id);
    }

    // ------------------------------------------------------------------------------------------
    // Independent least squares (no OSP code)

    /** Valid points by the corrected rule: both coordinates finite. */
    private static double[][] finitePoints(double[] x, double[] y) {
        int n = 0;
        for (int i = 0; i < x.length; i++) if (Double.isFinite(x[i]) && Double.isFinite(y[i])) n++;
        double[][] points = new double[2][n];
        for (int i = 0, k = 0; i < x.length; i++) {
            if (Double.isFinite(x[i]) && Double.isFinite(y[i])) {
                points[0][k] = x[i];
                points[1][k++] = y[i];
            }
        }
        return points;
    }

    private static final MathContext EXACT = new MathContext(80, RoundingMode.HALF_EVEN);

    /** The least-squares minimum over the free parameters and its linearized standard errors. */
    private static Expect leastSquares(String id, double[] x, double[] y, double[] truth, boolean[] fixed, double uncertaintyTolerance) {
        double[][] points = finitePoints(x, y);
        int p = truth.length;
        boolean[] fix = fixed == null ? new boolean[p] : fixed;
        int degree = polynomialDegree(id);
        double[] parameters;
        double[] uncertainties;
        if (degree > 0 || id.equals("log")) {
            // Exact normal equations in 80-digit decimal arithmetic for models linear in their parameters.
            double[][] basis = new double[points[0].length][p];
            for (int i = 0; i < basis.length; i++) {
                double xi = points[0][i];
                for (int j = 0; j < p; j++) {
                    if (id.equals("log")) basis[i][j] = j == 0 ? StrictMath.log(xi) : 1;
                    else basis[i][j] = StrictMath.pow(xi, degree - j);
                }
            }
            parameters = linearLeastSquares(basis, points[1], truth, fix);
            uncertainties = linearUncertainties(basis, points[1], parameters, fix);
        } else {
            parameters = gaussNewton(id, points[0], points[1], truth, fix);
            uncertainties = nonlinearUncertainties(id, points[0], points[1], parameters, fix);
        }
        return new Expect("least-squares", parameters, uncertainties, LEAST_SQUARES_RELATIVE, LEAST_SQUARES_ABSOLUTE,
            uncertaintyTolerance);
    }

    private static double[] linearLeastSquares(double[][] basis, double[] y, double[] start, boolean[] fixed) {
        int p = start.length;
        List<Integer> free = new ArrayList<>();
        for (int j = 0; j < p; j++) if (!fixed[j]) free.add(j);
        int q = free.size();
        BigDecimal[][] a = new BigDecimal[q][q + 1];
        for (int r = 0; r < q; r++) for (int c = 0; c <= q; c++) a[r][c] = BigDecimal.ZERO;
        for (int i = 0; i < y.length; i++) {
            BigDecimal residual = new BigDecimal(y[i]);
            for (int j = 0; j < p; j++) {
                if (fixed[j]) residual = residual.subtract(new BigDecimal(start[j]).multiply(new BigDecimal(basis[i][j])));
            }
            for (int r = 0; r < q; r++) {
                BigDecimal br = new BigDecimal(basis[i][free.get(r)]);
                for (int c = 0; c < q; c++) a[r][c] = a[r][c].add(br.multiply(new BigDecimal(basis[i][free.get(c)])));
                a[r][q] = a[r][q].add(br.multiply(residual));
            }
        }
        BigDecimal[] solution = solve(a);
        double[] result = start.clone();
        for (int r = 0; r < q; r++) result[free.get(r)] = solution[r].doubleValue();
        return result;
    }

    private static BigDecimal[] solve(BigDecimal[][] a) {
        int q = a.length;
        for (int k = 0; k < q; k++) {
            int pivot = k;
            for (int r = k + 1; r < q; r++) if (a[r][k].abs().compareTo(a[pivot][k].abs()) > 0) pivot = r;
            BigDecimal[] t = a[k];
            a[k] = a[pivot];
            a[pivot] = t;
            for (int r = k + 1; r < q; r++) {
                BigDecimal f = a[r][k].divide(a[k][k], EXACT);
                for (int c = k; c <= q; c++) a[r][c] = a[r][c].subtract(f.multiply(a[k][c]), EXACT);
            }
        }
        BigDecimal[] x = new BigDecimal[q];
        for (int r = q - 1; r >= 0; r--) {
            BigDecimal s = a[r][q];
            for (int c = r + 1; c < q; c++) s = s.subtract(a[r][c].multiply(x[c]), EXACT);
            x[r] = s.divide(a[r][r], EXACT);
        }
        return x;
    }

    /** sigma_i^2 = s^2 (B^T B)^-1_ii with s^2 = SSR / (N - free), in decimal arithmetic. */
    private static double[] linearUncertainties(double[][] basis, double[] y, double[] parameters, boolean[] fixed) {
        int p = parameters.length;
        double[][] jacobian = basis;
        double[] residuals = new double[y.length];
        for (int i = 0; i < y.length; i++) {
            BigDecimal fit = BigDecimal.ZERO;
            for (int j = 0; j < p; j++) fit = fit.add(new BigDecimal(parameters[j]).multiply(new BigDecimal(basis[i][j])));
            residuals[i] = new BigDecimal(y[i]).subtract(fit).doubleValue();
        }
        return standardErrors(jacobian, residuals, fixed);
    }

    private static double[] nonlinearUncertainties(String id, double[] x, double[] y, double[] parameters, boolean[] fixed) {
        double[][] jacobian = new double[x.length][];
        double[] residuals = new double[x.length];
        for (int i = 0; i < x.length; i++) {
            jacobian[i] = gradient(id, parameters, x[i]);
            residuals[i] = y[i] - model(id, parameters, x[i]);
        }
        return standardErrors(jacobian, residuals, fixed);
    }

    private static double[] standardErrors(double[][] jacobian, double[] residuals, boolean[] fixed) {
        int p = fixed.length;
        List<Integer> free = new ArrayList<>();
        for (int j = 0; j < p; j++) if (!fixed[j]) free.add(j);
        int q = free.size();
        double[] sigmas = new double[p];
        Arrays.fill(sigmas, Double.NaN);
        int n = residuals.length;
        if (q == 0 || n <= q) return sigmas;
        BigDecimal ssr = BigDecimal.ZERO;
        for (double r : residuals) ssr = ssr.add(new BigDecimal(r).multiply(new BigDecimal(r)));
        BigDecimal s2 = ssr.divide(new BigDecimal(n - q), EXACT);
        for (int target = 0; target < q; target++) {
            BigDecimal[][] a = new BigDecimal[q][q + 1];
            for (int r = 0; r < q; r++) {
                for (int c = 0; c < q; c++) {
                    BigDecimal s = BigDecimal.ZERO;
                    for (double[] row : jacobian) s = s.add(new BigDecimal(row[free.get(r)]).multiply(new BigDecimal(row[free.get(c)])));
                    a[r][c] = s;
                }
                a[r][q] = r == target ? BigDecimal.ONE : BigDecimal.ZERO;
            }
            BigDecimal inverse = solve(a)[target];
            sigmas[free.get(target)] = s2.multiply(inverse, EXACT).sqrt(EXACT).doubleValue();
        }
        return sigmas;
    }

    /** Damped Gauss-Newton from the synthetic parameters, run to a fixed point. */
    private static double[] gaussNewton(String id, double[] x, double[] y, double[] start, boolean[] fixed) {
        int p = start.length;
        double[] params = start.clone();
        double lambda = 1e-3;
        double ssr = ssr(id, x, y, params);
        for (int iteration = 0; iteration < 500; iteration++) {
            List<Integer> free = new ArrayList<>();
            for (int j = 0; j < p; j++) if (!fixed[j]) free.add(j);
            int q = free.size();
            double[][] a = new double[q][q + 1];
            for (int i = 0; i < x.length; i++) {
                double[] g = gradient(id, params, x[i]);
                double r = y[i] - model(id, params, x[i]);
                for (int u = 0; u < q; u++) {
                    for (int v = 0; v < q; v++) a[u][v] += g[free.get(u)] * g[free.get(v)];
                    a[u][q] += g[free.get(u)] * r;
                }
            }
            for (int u = 0; u < q; u++) a[u][u] *= 1 + lambda;
            BigDecimal[][] b = new BigDecimal[q][q + 1];
            for (int u = 0; u < q; u++) for (int v = 0; v <= q; v++) b[u][v] = new BigDecimal(a[u][v]);
            BigDecimal[] delta = solve(b);
            double[] trial = params.clone();
            double size = 0;
            for (int u = 0; u < q; u++) {
                double d = delta[u].doubleValue();
                trial[free.get(u)] += d;
                size = Math.max(size, Math.abs(d) / (Math.abs(params[free.get(u)]) + 1e-300));
            }
            double trialSsr = ssr(id, x, y, trial);
            if (trialSsr <= ssr) {
                params = trial;
                ssr = trialSsr;
                lambda = Math.max(lambda / 10, 1e-12);
                if (size < 1e-15) break;
            } else {
                lambda *= 10;
                if (lambda > 1e12) break;
            }
        }
        return params;
    }

    private static double ssr(String id, double[] x, double[] y, double[] p) {
        double sum = 0;
        for (int i = 0; i < x.length; i++) {
            double r = y[i] - model(id, p, x[i]);
            sum += r * r;
        }
        return sum;
    }

    // ------------------------------------------------------------------------------------------
    // Running OSP's curve fitter

    @SuppressWarnings("unchecked")
    private static List<KnownFunction> defaultFits() {
        try {
            return (List<KnownFunction>) field(FITTER, "defaultFits").get(null);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException(error);
        }
    }

    private static Unsafe unsafe() {
        try {
            Field field = Unsafe.class.getDeclaredField("theUnsafe");
            field.setAccessible(true);
            return (Unsafe) field.get(null);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException(error);
        }
    }

    private static Field field(Class<?> owner, String name) throws ReflectiveOperationException {
        Field field = owner.getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }

    private static void set(Object target, Class<?> owner, String name, Object value) throws ReflectiveOperationException {
        field(owner, name).set(target, value);
    }

    private static Object get(Object target, Class<?> owner, String name) throws ReflectiveOperationException {
        return field(owner, name).get(target);
    }

    /** Allocates a Swing component without its constructor and gives it the AWT lock object it synchronizes on. */
    private static <T> T allocate(Class<T> type) throws ReflectiveOperationException {
        T object = type.cast(UNSAFE.allocateInstance(type));
        UNSAFE.putObject(object, UNSAFE.objectFieldOffset(Component.class.getDeclaredField("objectLock")), new Object());
        return object;
    }

    /** The fitter's initial uncertainty array; still present after a fit, it was never computed. */
    private static final double[] UNSET_UNCERTAINTIES = new double[2];

    /** The default parameter values before any fit runs. */
    private static final List<double[]> CANONICAL_DEFAULTS = new ArrayList<>();

    /**
     * UserFunction.clone() and KnownPolynomial.clone() share their parameter arrays with the static
     * defaultFits, so every fit in a JVM writes into the defaults that later fitters clone (and that the
     * DampedSine initial guess reads for E). Restoring them gives each case a fresh Tracker session.
     */
    private static void restoreDefaults() {
        List<KnownFunction> defaults = defaultFits();
        if (CANONICAL_DEFAULTS.isEmpty()) {
            for (KnownFunction f : defaults) {
                double[] values = new double[f.getParameterCount()];
                for (int i = 0; i < values.length; i++) values[i] = f.getParameterValue(i);
                CANONICAL_DEFAULTS.add(values);
            }
        }
        for (int k = 0; k < defaults.size(); k++) {
            double[] values = CANONICAL_DEFAULTS.get(k);
            for (int i = 0; i < values.length; i++) defaults.get(k).setParameterValue(i, values[i]);
        }
    }

    /** A fresh curve fitter, as DataToolTab creates one, holding the given raw columns. */
    private static DatasetCurveFitter fitter(double[] x, double[] y, boolean autofit) throws ReflectiveOperationException {
        restoreDefaults();
        DatasetCurveFitter fitter = allocate(DatasetCurveFitter.class);
        QuietTab tab = allocate(QuietTab.class);
        set(tab, DataToolTab.class, "plot", UNSAFE.allocateInstance(QuietTab.QuietPlotter.class));
        set(tab, DataToolTab.class, "statsCheckbox", new JCheckBoxMenuItem());
        Dataset data = new Dataset();
        data.append(x, y);
        set(fitter, FITTER, "dataset", data);
        set(fitter, FITTER, "tab", tab);
        set(fitter, FITTER, "testFunctions", new ArrayList<UserFunction>());
        ArrayList<KnownFunction> local = new ArrayList<>();
        TreeMap<String, KnownFunction> map = new TreeMap<>();
        for (KnownFunction f : defaultFits()) {
            KnownFunction copy = f.clone();
            local.add(copy);
            map.put(copy.getName(), copy);
        }
        set(fitter, FITTER, "localFits", local);
        set(fitter, FITTER, "fitMap", map);
        set(fitter, FITTER, "hessian", new HessianMinimize());
        set(fitter, FITTER, "levmar", new LevenbergMarquardt());
        set(fitter, FITTER, "fixedParams", new HashMap<KnownFunction, boolean[]>());
        set(fitter, FITTER, "initialParams", new HashMap<KnownFunction, double[]>());
        field(FITTER, "sigma_y_squared").setDouble(fitter, 1.0);
        field(FITTER, "correlation").setDouble(fitter, Double.NaN);
        set(fitter, FITTER, "uncertainties", UNSET_UNCERTAINTIES);
        field(FITTER, "autofit").setBoolean(fitter, autofit);
        set(fitter, FITTER, "autofitCheckBox", new JCheckBox());
        set(fitter, FITTER, "eqnField", new JTextField());
        Constructor<?> numberField = Class.forName(FITTER.getName() + "$NumberField").getDeclaredConstructor(int.class);
        numberField.setAccessible(true);
        set(fitter, FITTER, "rmsField", numberField.newInstance(6));
        Class<?> modelClass = Class.forName(FITTER.getName() + "$ParamTableModel");
        Constructor<?> modelConstructor = modelClass.getDeclaredConstructor(FITTER);
        modelConstructor.setAccessible(true);
        Object model = modelConstructor.newInstance(fitter);
        set(fitter, FITTER, "paramModel", model);
        Constructor<?> table = Class.forName(FITTER.getName() + "$ParamTable").getDeclaredConstructor(FITTER, modelClass);
        table.setAccessible(true);
        set(fitter, FITTER, "paramTable", table.newInstance(fitter, model));
        return fitter;
    }

    private static UserFunction userFunction(User spec) {
        UserFunction f = new UserFunction(spec.name());
        f.setParameters(spec.parameters().clone(), spec.values().clone(), new String[spec.parameters().length]);
        if (!f.setExpression(spec.expression(), new String[] {"x"})) throw new IllegalStateException(spec.expression());
        return f;
    }

    private static KnownFunction function(DatasetCurveFitter fitter, String id) throws ReflectiveOperationException {
        User spec = user(id);
        if (spec != null) return userFunction(spec);
        @SuppressWarnings("unchecked")
        TreeMap<String, KnownFunction> map = (TreeMap<String, KnownFunction>) get(fitter, FITTER, "fitMap");
        return map.get(builtIn(id).ospName());
    }

    /** HessianMinimize and LevenbergMarquardt that record their calls, to identify the main fit's path. */
    static final class RecordingHessian extends HessianMinimize {
        final List<String> calls;
        /** The objective at the first fit's start: the deviation the fitter compares with. */
        double startValue = Double.NaN;
        /** The objective at the last vector the first fit evaluated, which the fitted function keeps. */
        double firstLastValue = Double.NaN;
        /** The free parameters the first fit started from: the from-scratch guess or the current values. */
        double[] startParameters;

        RecordingHessian(List<String> calls) {
            this.calls = calls;
        }

        @Override
        public double minimize(MultiVarFunction f, double[] x, int max, double tol) {
            boolean first = calls.isEmpty();
            if (first) startParameters = x.clone();
            calls.add("hessian");
            return super.minimize(p -> {
                double value = f.evaluate(p);
                if (first && Double.isNaN(startValue)) startValue = value;
                if (first) firstLastValue = value;
                return value;
            }, x, max, tol);
        }
    }

    static final class RecordingLevenberg extends LevenbergMarquardt {
        final List<String> calls;
        /** The objective at the last vector LM evaluated, which the fitted function keeps. */
        double lastValue = Double.NaN;

        RecordingLevenberg(List<String> calls) {
            this.calls = calls;
        }

        @Override
        public boolean minimize(MultiVarFunction f, double[] x, int max, double tol) {
            boolean converged = super.minimize(p -> lastValue = f.evaluate(p), x, max, tol);
            calls.add(converged ? "lm" : "lm-failed");
            return converged;
        }
    }

    private record Result(int points, double[] validX, double[] parameters, double[] uncertainties, double[][] band,
                          String intendedBand, double rms, double correlation, boolean nonReal, String method, String[] formatted,
                          boolean diverged, double[] start, double startValue, boolean tied) {}

    /** Fits one case with OSP's DatasetCurveFitter. */
    private static Result run(CaseSpec spec, String fitId, double[] y) throws ReflectiveOperationException {
        DatasetCurveFitter fitter = fitter(spec.x(), y, spec.autofit());
        List<String> calls = new ArrayList<>();
        RecordingHessian hessian = new RecordingHessian(calls);
        RecordingLevenberg levenberg = new RecordingLevenberg(calls);
        set(fitter, FITTER, "hessian", hessian);
        set(fitter, FITTER, "levmar", levenberg);
        KnownFunction f = function(fitter, fitId);
        if (spec.initial() != null) {
            for (int i = 0; i < spec.initial().length; i++) f.setParameterValue(i, spec.initial()[i]);
        }
        @SuppressWarnings("unchecked")
        HashMap<KnownFunction, boolean[]> fixed = (HashMap<KnownFunction, boolean[]>) get(fitter, FITTER, "fixedParams");
        if (spec.fixed() != null) fixed.put(f, spec.fixed().clone());
        set(fitter, FITTER, "fit", f);
        UncertainFunctionDrawer drawer = new UncertainFunctionDrawer(f);
        set(fitter, FITTER, "drawer", drawer);
        double rms = fitter.fit(f, spec.scratch());
        double correlation = field(FITTER, "correlation").getDouble(fitter);
        boolean nonReal = field(FITTER, "fitEvaluatedToNaN").getBoolean(fitter);
        int p = f.getParameterCount();
        double[] values = new double[p];
        for (int i = 0; i < p; i++) values[i] = f.getParameterValue(i);
        // The main fit is the first HessianMinimize call and, when its result was worse, the next
        // LevenbergMarquardt call; only after LM can the fitter restore its starting values.
        String method = "none";
        boolean diverged = false;
        boolean closedForm = f instanceof KnownPolynomial && (spec.fixed() == null || !anyTrue(spec.fixed()));
        int points = ((Dataset) get(fitter, FITTER, "dataset")).getValidXPoints().length;
        if (closedForm) {
            // Any minimizer calls here are the uncertainty refits.
            if (spec.autofit() && points >= p) method = "closed-form";
        } else if (!calls.isEmpty()) {
            method = "hessian";
            if (calls.size() > 1 && calls.get(1).startsWith("lm")) {
                // Restored when LM fails or its last evaluated vector is still worse than the start.
                boolean restored = calls.get(1).equals("lm-failed") || levenberg.lastValue > hessian.startValue;
                method = restored ? "restored" : "levenberg-marquardt";
                // Newton diverged rather than missing the start by rounding: the outcome of the three-step
                // Levenberg-Marquardt fallback that follows is then decided by last bits.
                double scale = 0;
                for (double value : y) if (Double.isFinite(value)) scale += value * value;
                diverged = !(hessian.firstLastValue <= hessian.startValue * (1 + 1e-6) + 1e-12 * scale);
            }
        }
        double[] sigmas = (double[]) get(fitter, FITTER, "uncertainties");
        if (sigmas == UNSET_UNCERTAINTIES || !spec.autofit()) sigmas = null;
        double[][] band = (double[][]) get(drawer, UncertainFunctionDrawer.class, "uncertainParams");
        Dataset data = (Dataset) get(fitter, FITTER, "dataset");
        String[] formatted = new String[p];
        for (int i = 0; i < p; i++) {
            double sigma = sigmas == null || i >= sigmas.length ? Double.NaN : sigmas[i];
            String[] text = fitter.formatUncertainParameter(values[i], sigma, 0, NumberFormat.getInstance());
            formatted[i] = text == null ? null : text[0];
        }
        String intended = intendedBand(fitter, f, spec, values, sigmas);
        // Tied: the main fit ended at the deviation it started from (to 1e-12), as when the start is already the
        // minimum. Which minimizer's result Tracker then keeps is decided by last bits.
        boolean minimized = method.equals("hessian") || method.equals("levenberg-marquardt") || method.equals("restored");
        double finalValue = rms * rms * points;
        boolean tied = minimized && Math.abs(finalValue - hessian.startValue) <= 1e-12 * hessian.startValue;
        return new Result(data.getValidXPoints().length, data.getValidXPoints(), values, sigmas, band, intended, rms, correlation,
            nonReal, method, formatted, diverged, minimized ? hessian.startParameters : null, hessian.startValue, tied);
    }

    private static boolean anyTrue(boolean[] flags) {
        for (boolean flag : flags) if (flag) return true;
        return false;
    }

    private static String resultJson(Result r, Result perturbed, boolean validity) {
        StringBuilder json = new StringBuilder("{");
        json.append("\"points\":").append(r.points());
        // For the data-validity cases, the fitted abscissae show Dataset.append's compaction.
        json.append(",\"valid_x\":").append(validity && r.validX().length <= 200 ? numbers(r.validX()) : "null");
        json.append(",\"method\":").append(string(r.method()));
        json.append(",\"parameters\":").append(numbers(r.parameters()));
        json.append(",\"uncertainties\":").append(r.uncertainties() == null ? "null" : numbers(r.uncertainties()));
        json.append(",\"band\":").append(matrix(r.band()));
        json.append(",\"intended_band\":").append(r.intendedBand());
        json.append(",\"rms\":").append(number(r.rms()));
        json.append(",\"correlation\":").append(number(r.correlation()));
        json.append(",\"non_real\":").append(r.nonReal());
        json.append(",\"formatted\":").append(strings(r.formatted()));
        // Where the main fit minimized: the free parameters and deviation it started from.
        json.append(",\"start\":").append(r.start() == null ? "null"
            : "{\"parameters\":" + numbers(r.start()) + ",\"ssr\":" + number(r.startValue()) + "}");
        json.append(",\"tied\":").append(r.tied());
        if (perturbed != null) {
            // How far OSP's own result moves when every y changes by one ulp: the last-bit noise that
            // separates Math.exp/sin/log/pow from <cmath> moves it similarly.
            json.append(",\"sensitivity\":{\"parameters\":").append(number(deviation(r.parameters(), perturbed.parameters())))
                .append(",\"rms\":").append(number(deviation(r.rms(), perturbed.rms())))
                .append(",\"uncertainties\":")
                .append(number(Math.max(deviation(r.uncertainties(), perturbed.uncertainties()),
                    deviation(flatten(r.band()), flatten(perturbed.band())))))
                .append('}');
        }
        return json.append('}').toString();
    }

    private static double[] flatten(double[][] rows) {
        if (rows == null) return null;
        return Arrays.stream(rows).flatMapToDouble(Arrays::stream).toArray();
    }

    /** Largest relative difference; 1 when only one side is undefined or the shapes differ. */
    private static double deviation(double[] a, double[] b) {
        if (a == null || b == null) return a == b ? 0 : 1;
        if (a.length != b.length) return 1;
        double worst = 0;
        for (int i = 0; i < a.length; i++) worst = Math.max(worst, deviation(a[i], b[i]));
        return worst;
    }

    private static double deviation(double a, double b) {
        if (Double.isNaN(a) || Double.isNaN(b)) return Double.isNaN(a) && Double.isNaN(b) ? 0 : 1;
        if (a == b) return 0;
        // Values below 1e-12 in magnitude (such as a zero start moved by one ulp) count as zero.
        return Math.min(1, Math.abs(a - b) / Math.max(1e-12, Math.max(Math.abs(a), Math.abs(b))));
    }

    private static String matrix(double[][] rows) {
        if (rows == null) return "null";
        StringBuilder json = new StringBuilder("[");
        for (int i = 0; i < rows.length; i++) json.append(i > 0 ? "," : "").append(numbers(rows[i]));
        return json.append(']').toString();
    }

    /** The band the fitter refits before its inner `continue` discards the result: each tested
     *  parameter at value -/+ sigma and every other free parameter refitted (fixed ones at their values). */
    private static String intendedBand(DatasetCurveFitter fitter, KnownFunction f, CaseSpec spec, double[] values, double[] sigmas)
        throws ReflectiveOperationException {
        if (sigmas == null || !spec.autofit()) return "null";
        Method testFunction = FITTER.getDeclaredMethod("getTestFunction", KnownFunction.class, boolean[].class);
        testFunction.setAccessible(true);
        Method oneFixed = FITTER.getDeclaredMethod("getTestFunction", KnownFunction.class, String.class, double.class);
        oneFixed.setAccessible(true);
        boolean[] fix = spec.fixed() == null ? new boolean[values.length] : spec.fixed();
        KnownFunction fitted = (KnownFunction) testFunction.invoke(fitter, f, fix);
        StringBuilder json = new StringBuilder("[");
        boolean first = true;
        for (int i = 0; i < values.length; i++) {
            if (fix[i] || i >= sigmas.length || !(sigmas[i] > 0)) continue;
            for (int j = 0; j < 2; j++) {
                double value = j == 0 ? values[i] - sigmas[i] : values[i] + sigmas[i];
                UserFunction test = (UserFunction) oneFixed.invoke(fitter, fitted, f.getParameterName(i), value);
                fitter.fit(test);
                double[] set = new double[values.length];
                for (int k = 0; k < values.length; k++) {
                    set[k] = k == i ? value : values[k];
                    for (int m = 0; m < test.getParameterCount(); m++) {
                        if (test.getParameterName(m).equals(f.getParameterName(k))) set[k] = test.getParameterValue(m);
                    }
                }
                json.append(first ? "" : ",").append(numbers(set));
                first = false;
            }
        }
        return json.append(']').toString();
    }

    // ------------------------------------------------------------------------------------------
    // Direct minimizer cases: HessianMinimize and LevenbergMarquardt on arithmetic objectives

    private record MinimizerSpec(String id, String objective, double[] start) {}

    private static final List<MinimizerSpec> MINIMIZERS = List.of(
        new MinimizerSpec("rosenbrock", "rosenbrock", new double[] {-1.2, 1.0}),
        new MinimizerSpec("bowl", "bowl", new double[] {3.0, -2.0, 0.5}),
        new MinimizerSpec("bowl-at-minimum", "bowl", new double[] {1.0, 2.0, -3.0}),
        new MinimizerSpec("flat", "flat", new double[] {0.5, 0.5}),
        new MinimizerSpec("quartic", "quartic", new double[] {2.0}),
        new MinimizerSpec("cliff", "cliff", new double[] {0.9, 0.2}),
        new MinimizerSpec("large-offset", "bowl", new double[] {1.0e6, -2.0e6, 5.0e5}));

    static double objective(String id, double[] p) {
        switch (id) {
            case "rosenbrock": return 100 * (p[1] - p[0] * p[0]) * (p[1] - p[0] * p[0]) + (1 - p[0]) * (1 - p[0]);
            case "bowl": return (p[0] - 1) * (p[0] - 1) + 10 * (p[1] - 2) * (p[1] - 2) + 0.5 * (p[2] + 3) * (p[2] + 3)
                + (p[0] - 1) * (p[1] - 2);
            case "flat": return (p[0] + p[1] - 1) * (p[0] + p[1] - 1);
            case "quartic": return (p[0] - 1) * (p[0] - 1) * (p[0] - 1) * (p[0] - 1);
            case "cliff": return p[0] < 1 ? (p[0] - 2) * (p[0] - 2) + p[1] * p[1] : Double.NaN;
            default: throw new IllegalArgumentException(id);
        }
    }

    private static String runMinimizer(MinimizerSpec spec, boolean levenberg) throws ReflectiveOperationException {
        MultiVarFunction f = p -> objective(spec.objective(), p);
        double[] x = spec.start().clone();
        String outcome;
        int iterations;
        if (levenberg) {
            LevenbergMarquardt minimizer = new LevenbergMarquardt();
            outcome = Boolean.toString(minimizer.minimize(f, x, 20, 1e-6));
            iterations = minimizer.getIterations();
        } else {
            HessianMinimize minimizer = new HessianMinimize();
            outcome = number(minimizer.minimize(f, x, 20, 1e-6));
            iterations = minimizer.getIterations();
        }
        return "{\"id\":" + string((levenberg ? "levenberg-marquardt/" : "hessian/") + spec.id()) + ",\"objective\":"
            + string(spec.objective()) + ",\"start\":" + numbers(spec.start()) + ",\"result\":" + outcome + ",\"iterations\":"
            + iterations + ",\"parameters\":" + numbers(x) + "}";
    }

    // ------------------------------------------------------------------------------------------
    // Uncertainty display probes (formatUncertainParameter)

    private static final double[][] FORMAT_PROBES = {
        {2.0100000000000002, 0.041231056256139244}, {0.125, 0.25}, {9.295376724356178, 0.7018574138262079},
        {-0.4286411426676149, 0.8874524323238784}, {76.0500000000003, 4.460823921607544}, {1234.5, 0.05}, {0.00012345, 0.0000045},
        {0, 0.003}, {-15.25, 25}, {0.15, 0.1}, {2.5, 1}, {9.96, 0.5}, {1.0e-7, 3.0e-9}, {-3.0e8, 2.5e6}, {0.995, 0.0049},
        {1, Double.NaN}, {1, 0}, {1, -1}};

    private static String formatProbes() throws ReflectiveOperationException {
        DatasetCurveFitter fitter = fitter(new double[0], new double[0], true);
        StringBuilder json = new StringBuilder("[");
        for (int i = 0; i < FORMAT_PROBES.length; i++) {
            for (int extra = 0; extra <= 1; extra++) {
                double[] probe = FORMAT_PROBES[i];
                String[] text = fitter.formatUncertainParameter(probe[0], probe[1], extra, NumberFormat.getInstance());
                json.append(i > 0 || extra > 0 ? "," : "").append("{\"value\":").append(number(probe[0])).append(",\"sigma\":")
                    .append(number(probe[1])).append(",\"extra_places\":").append(extra).append(",\"text\":")
                    .append(text == null ? "null" : string(text[0])).append('}');
            }
        }
        return json.append(']').toString();
    }

    // ------------------------------------------------------------------------------------------
    // Output

    public static void main(String[] args) {
        try {
            if (args.length != 1) throw new IllegalArgumentException("Usage: FitOracle output.json");
            Locale.setDefault(Locale.US);
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            // As Tracker.start does (Tracker 6.3.5), unlike a standalone OSP Data Tool.
            Dataset.maxPointsMultiplier = TRACKER_MAX_POINTS_MULTIPLIER;
            restoreDefaults();
            curatedCases();
            randomCases();
            StringBuilder json = new StringBuilder();
            json.append("{\"schema_version\":1,\"reference_version\":").append(string(OSPRuntime.VERSION));
            json.append(",\"fits\":[");
            List<KnownFunction> defaults = defaultFits();
            for (int i = 0; i < BUILT_INS.size(); i++) {
                BuiltIn b = BUILT_INS.get(i);
                KnownFunction f = defaults.get(i);
                if (!f.getName().equals(b.ospName())) throw new IllegalStateException("Unexpected default fit " + f.getName());
                json.append(i > 0 ? "," : "").append(fitJson(b.id(), f));
            }
            json.append("],\"user_fits\":[");
            for (int i = 0; i < USERS.size(); i++) {
                User u = USERS.get(i);
                json.append(i > 0 ? "," : "").append("{\"id\":").append(string(u.id())).append(",\"name\":").append(string(u.name()))
                    .append(",\"expression\":").append(string(userFunction(u).getExpression("x"))).append(",\"parameters\":")
                    .append(strings(u.parameters())).append(",\"values\":").append(numbers(u.values())).append('}');
            }
            json.append("],\"cases\":[");
            for (int i = 0; i < CASES.size(); i++) json.append(i > 0 ? "," : "").append(caseJson(CASES.get(i)));
            json.append("],\"minimizers\":[");
            boolean first = true;
            for (boolean levenberg : new boolean[] {false, true}) {
                for (MinimizerSpec spec : MINIMIZERS) {
                    json.append(first ? "" : ",").append(runMinimizer(spec, levenberg));
                    first = false;
                }
            }
            json.append("],\"format\":").append(formatProbes()).append("}\n");
            Files.writeString(Path.of(args[0]), json, StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }

    private static String fitJson(String id, KnownFunction f) {
        int p = f.getParameterCount();
        String[] names = new String[p];
        String[] descriptions = new String[p];
        double[] values = new double[p];
        for (int i = 0; i < p; i++) {
            names[i] = f.getParameterName(i);
            descriptions[i] = f.getParameterDescription(i);
            values[i] = f.getParameterValue(i);
        }
        String expression = f instanceof UserFunction ? ((UserFunction) f).getFullExpression(new String[] {"x"})
                                                      : f.getExpression("x").replace(" ", "");
        return "{\"id\":" + string(id) + ",\"name\":" + string(f.getName()) + ",\"display_name\":"
            + string(FitBuilder.localize(f.getName())) + ",\"description\":" + string(f.getDescription()) + ",\"expression\":"
            + string(expression) + ",\"polynomial\":" + (f instanceof KnownPolynomial) + ",\"parameters\":" + strings(names)
            + ",\"descriptions\":" + strings(descriptions) + ",\"defaults\":" + numbers(values) + "}";
    }

    private static String caseJson(CaseSpec spec) throws ReflectiveOperationException {
        Result java = run(spec, spec.fit(), spec.y());
        // A diverged Newton iteration makes the case chaotic, and Tracker's result then no independent minimum.
        boolean chaotic = spec.agreement().equals("chaotic") || (java.diverged() && spec.agreement().equals("transcendental"));
        String agreement = chaotic ? "chaotic" : spec.agreement();
        StringBuilder json = new StringBuilder("{");
        json.append("\"id\":").append(string(spec.id())).append(",\"group\":").append(string(spec.group()))
            .append(",\"fit\":").append(string(spec.fit())).append(",\"agreement\":").append(string(agreement))
            .append(",\"autofit\":").append(spec.autofit()).append(",\"start\":").append(string(spec.scratch() ? "scratch" : "current"))
            .append(",\"initial\":").append(spec.initial() == null ? "null" : numbers(spec.initial()))
            .append(",\"fixed\":").append(spec.fixed() == null ? "null" : booleans(spec.fixed()))
            .append(spec.id().equals("line/over-capacity")
                ? ",\"generated\":{\"rows\":" + CAPACITY_ROWS + ",\"head\":" + CAPACITY_HEAD + ",\"head_y\":50.0}"
                : ",\"x\":" + numbers(spec.x()) + ",\"y\":" + numbers(spec.y()));
        if (spec.defect() != null) json.append(",\"defect\":").append(string(spec.defect()));
        Expect e = chaotic ? null : spec.expect();
        if (e != null) {
            json.append(",\"expect\":{\"source\":").append(string(e.source())).append(",\"parameters\":").append(numbers(e.parameters()))
                .append(",\"uncertainties\":").append(e.uncertainties() == null ? "null" : numbers(e.uncertainties()))
                .append(",\"parameter_tolerance\":{\"relative\":").append(number(e.parameterRelative())).append(",\"absolute\":")
                .append(number(e.parameterAbsolute())).append("},\"uncertainty_relative\":").append(number(e.uncertaintyRelative()))
                .append('}');
        }
        Result perturbed = null;
        if (!spec.agreement().equals("exact")) {
            double[] y = spec.y().clone();
            for (int i = 0; i < y.length; i++) if (Double.isFinite(y[i])) y[i] = Math.nextUp(y[i]);
            perturbed = run(spec, spec.fit(), y);
        }
        boolean validity = spec.group().equals("validity");
        json.append(",\"java\":").append(resultJson(java, perturbed, validity));
        if (spec.intendedFit() != null) {
            json.append(",\"intended_fit\":").append(string(spec.intendedFit()))
                .append(",\"intended\":").append(resultJson(run(spec, spec.intendedFit(), spec.y()), null, validity));
        }
        return json.append('}').toString();
    }

    private static String numbers(double[] values) {
        StringBuilder json = new StringBuilder("[");
        for (int i = 0; i < values.length; i++) json.append(i > 0 ? "," : "").append(number(values[i]));
        return json.append(']').toString();
    }

    private static String booleans(boolean[] values) {
        StringBuilder json = new StringBuilder("[");
        for (int i = 0; i < values.length; i++) json.append(i > 0 ? "," : "").append(values[i]);
        return json.append(']').toString();
    }

    private static String strings(String[] values) {
        StringBuilder json = new StringBuilder("[");
        for (int i = 0; i < values.length; i++) json.append(i > 0 ? "," : "").append(values[i] == null ? "null" : string(values[i]));
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
            else if (c > 0x7e) json.append(String.format("\\u%04x", (int) c));
            else json.append(c);
        }
        return json.append('"').toString();
    }
}
