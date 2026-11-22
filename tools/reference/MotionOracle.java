import java.lang.reflect.Field;
import java.math.BigDecimal;
import java.math.MathContext;
import java.math.RoundingMode;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.function.DoubleUnaryOperator;
import org.opensourcephysics.cabrillo.tracker.BounceDerivatives;
import org.opensourcephysics.cabrillo.tracker.ButterworthFilter;
import org.opensourcephysics.cabrillo.tracker.FirstDerivative;
import org.opensourcephysics.cabrillo.tracker.MotionFilter;
import org.opensourcephysics.cabrillo.tracker.MovingAverageFilter;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.SavitzkyGolayFilter;
import org.opensourcephysics.cabrillo.tracker.SecondDerivative;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.numerics.Derivative;
import org.opensourcephysics.numerics.Function;
import org.opensourcephysics.numerics.MultiVarFunction;
import org.opensourcephysics.numerics.NumericMethodException;

/** Runs Tracker 6.3.5's motion-data kernels headless on fixed cases (docs/motion-kernels.md).
 *
 * The derivative classes (FirstDerivative, SecondDerivative, BounceDerivatives), the motion filters
 * (MovingAverageFilter, ButterworthFilter, SavitzkyGolayFilter) and OSP's numerics.Derivative are the
 * actual pinned classes. They are driven the way PointMass and FilteredPointMass drive them: arrays
 * indexed by original frame number, valid flags only on clip frames that have a position, and junk
 * values in every invalid slot to prove they are never read. PointMass itself needs a TrackerPanel, so
 * its time scaling (refreshData's dt_v and dt_a2) and polar-angle unwrapping (getRotationData) are
 * transcribed below operation by operation; slice C2 checks the full PointMass columns end to end.
 */
public final class MotionOracle {
    private static final double JUNK = 98765.4321; // stale values in invalid array slots
    private static final int TRACKER_PADDING = 15; // StepArray growth (10) plus PointMass's +5 padding

    public static void main(String[] args) {
        try {
            if (args.length != 1) throw new IllegalArgumentException("Usage: MotionOracle output.json");
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            StringBuilder json = new StringBuilder();
            json.append("{\"schema_version\":1,\"reference_version\":").append(string(OSPRuntime.VERSION));
            json.append(",\"tracker\":").append(trackerSettings());
            json.append(",\"self_test\":").append(selfTest());
            json.append(",\"derivative_cases\":").append(join(derivativeCases()));
            json.append(",\"difference_cases\":").append(join(differenceCases()));
            json.append(",\"angular_cases\":").append(join(angularCases()));
            List<String> filters = filterCases();
            json.append(",\"filter_signals\":").append(join(SIGNAL_JSON));
            json.append(",\"filter_cases\":").append(join(filters));
            json.append(",\"numerical_cases\":").append(join(numericalCases()));
            json.append("}\n");
            Files.writeString(Path.of(args[0]), json, StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }

    // ------------------------------------------------------------------------------------------
    // Tracker's own constants, read from the pinned classes rather than restated.

    private static String trackerSettings() throws ReflectiveOperationException {
        StringBuilder json = new StringBuilder("{\"algorithm_codes\":{");
        String[] names = {"FINITE_DIFF", "BOUNCE_DETECT", "FINITE_DIFF_VSPILL2"};
        for (int i = 0; i < names.length; i++) {
            if (i > 0) json.append(',');
            json.append(string(names[i])).append(':').append(field(PointMass.class, null, names[i]));
        }
        // A PointMass without a TrackerPanel is enough to read its per-track derivative settings.
        PointMass mass = new PointMass();
        json.append("},\"default_algorithm\":").append(field(PointMass.class, mass, "algorithm")).append(",\"spills\":{");
        String[] spills = {"vDerivSpill", "aDerivSpill", "bounceDerivsSpill"};
        for (int i = 0; i < spills.length; i++) {
            if (i > 0) json.append(',');
            json.append(string(spills[i])).append(':').append(field(PointMass.class, mass, spills[i]));
        }
        MovingAverageFilter ma = new MovingAverageFilter();
        ButterworthFilter bw = new ButterworthFilter();
        SavitzkyGolayFilter sg = new SavitzkyGolayFilter();
        json.append("},\"filter_defaults\":{\"moving_average\":{\"window\":").append(ma.getWindow())
            .append("},\"butterworth\":{\"order\":").append(bw.getOrder()).append(",\"cutoff_hz\":").append(number(bw.getCutoffHz()))
            .append(",\"sample_rate_hz\":").append(number(bw.getSampleRateHz()))
            .append("},\"savitzky_golay\":{\"window\":").append(sg.getWindow()).append(",\"poly_order\":").append(sg.getPolyOrder())
            .append("}}}");
        return json.toString();
    }

    private static int field(Class<?> type, Object instance, String name) throws ReflectiveOperationException {
        Field field = type.getDeclaredField(name);
        field.setAccessible(true);
        return field.getInt(instance);
    }

    // ------------------------------------------------------------------------------------------
    // FilterSelfTest (Tracker src/test/FilterSelfTest.java), recomputed here with the same inputs so
    // that the fixture records the reference's own pass/fail and metrics for each of its six checks.

    private static String selfTest() {
        List<String> checks = new ArrayList<>();
        {
            int n = 200;
            double[] x = new double[n];
            boolean[] valid = new boolean[n];
            double seedState = 42L;
            for (int i = 0; i < n; i++) {
                seedState = (seedState * 1103515245.0 + 12345.0) % (1L << 31);
                double noise = ((seedState / (double) (1L << 31)) - 0.5) * 0.4;
                x[i] = Math.sin(2 * Math.PI * i / 50.0) + noise;
                valid[i] = true;
            }
            double[] y = new MovingAverageFilter(7).apply(x, valid);
            double in = rms(x), out = rms(y);
            checks.add(check("moving_average_reduces_noise", out < in, "rms_in", in, "rms_out", out));
        }
        {
            double[] x = {0, 1, 2, 100, 100, 5, 6, 7};
            boolean[] v = {true, true, true, false, false, true, true, true};
            double[] y = new MovingAverageFilter(3).apply(x, v);
            boolean pass = y[3] == 100 && y[4] == 100 && Math.abs(y[5] - 5.5) < 1e-9 && Math.abs(y[6] - 6.0) < 1e-9
                && Math.abs(y[7] - 6.5) < 1e-9;
            checks.add(check("moving_average_handles_invalid_gap", pass, "y5", y[5], "y7", y[7]));
        }
        {
            double fs = 100.0;
            int n = 1024;
            double[] x = new double[n];
            boolean[] v = new boolean[n];
            for (int i = 0; i < n; i++) {
                x[i] = Math.sin(2 * Math.PI * 20.0 * i / fs);
                v[i] = true;
            }
            double[] y = new ButterworthFilter(4, 5.0, fs).apply(x, v);
            double ratio = peak(y, 50, n - 50) / peak(x, 50, n - 50);
            checks.add(check("butterworth_attenuates_above_cutoff", ratio < 0.05, "ratio", ratio, null, 0));
        }
        {
            int n = 256;
            double[] x = new double[n];
            boolean[] v = new boolean[n];
            for (int i = 0; i < n; i++) {
                x[i] = 3.7;
                v[i] = true;
            }
            double[] y = new ButterworthFilter(4, 5.0, 100.0).apply(x, v);
            double maxErr = 0;
            for (int i = 20; i < n - 20; i++) maxErr = Math.max(maxErr, Math.abs(y[i] - 3.7));
            checks.add(check("butterworth_preserves_dc", maxErr < 1e-6, "max_error", maxErr, null, 0));
        }
        {
            double fs = 100.0;
            int n = 512;
            double[] x = new double[n];
            boolean[] v = new boolean[n];
            for (int i = 0; i < n; i++) {
                x[i] = Math.sin(2 * Math.PI * 2.0 * i / fs);
                v[i] = true;
            }
            double[] y = new ButterworthFilter(4, 10.0, fs).apply(x, v);
            int zeroX = -1, zeroY = -1;
            for (int i = 100; i < 200; i++) {
                if (zeroX < 0 && x[i] >= 0 && x[i + 1] < 0) zeroX = i;
                if (zeroY < 0 && y[i] >= 0 && y[i + 1] < 0) zeroY = i;
            }
            checks.add("{\"name\":\"butterworth_zero_phase\",\"passed\":" + (zeroX > 0 && zeroY > 0 && Math.abs(zeroX - zeroY) <= 2)
                + ",\"zero_x\":" + zeroX + ",\"zero_y\":" + zeroY + "}");
        }
        {
            int n = 50;
            double[] x = new double[n];
            boolean[] v = new boolean[n];
            for (int i = 0; i < n; i++) {
                double t = i;
                x[i] = 1.0 + 2.0 * t + 0.5 * t * t;
                v[i] = true;
            }
            double[] y = new SavitzkyGolayFilter(7, 2).apply(x, v);
            double maxErr = 0;
            for (int i = 0; i < n; i++) maxErr = Math.max(maxErr, Math.abs(y[i] - x[i]));
            checks.add(check("savitzky_golay_preserves_quadratic", maxErr < 1e-6, "max_error", maxErr, null, 0));
        }
        return join(checks);
    }

    private static String check(String name, boolean passed, String key1, double value1, String key2, double value2) {
        StringBuilder json = new StringBuilder("{\"name\":").append(string(name)).append(",\"passed\":").append(passed);
        json.append(',').append(string(key1)).append(':').append(number(value1));
        if (key2 != null) json.append(',').append(string(key2)).append(':').append(number(value2));
        return json.append('}').toString();
    }

    private static double rms(double[] x) {
        double sum = 0;
        for (double value : x) sum += value * value;
        return Math.sqrt(sum / x.length);
    }

    private static double peak(double[] x, int from, int to) {
        double result = 0;
        for (int i = from; i < to; i++) result = Math.max(result, Math.abs(x[i]));
        return result;
    }

    // ------------------------------------------------------------------------------------------
    // Point-mass derivative cases: positions per clip step (null = no mark), clip lattice and step times.

    private record Clip(int first, int step, int count) {
        int frame(int k) {
            return first + step * k;
        }
    }

    /** A deterministic signal generator (the FilterSelfTest LCG), so the fixture is reproducible. */
    private static final class Noise {
        private double state;

        Noise(long seed) {
            state = seed;
        }

        double next() {
            state = (state * 1103515245.0 + 12345.0) % (1L << 31);
            return (state / (double) (1L << 31)) - 0.5;
        }
    }

    /** Unit vectors at angle phase + k * step, by exact rotation arithmetic from literal cos/sin values. */
    private static final class Rotor {
        private final double c, s;
        private double x, y;

        Rotor(double cosPhase, double sinPhase, double cosStep, double sinStep) {
            x = cosPhase;
            y = sinPhase;
            c = cosStep;
            s = sinStep;
        }

        /** Returns {cos, sin} of the current angle, then advances one step. */
        double[] next() {
            double[] value = {x, y};
            double nx = x * c - y * s;
            y = y * c + x * s;
            x = nx;
            return value;
        }
    }

    private interface Trajectory {
        double[] at(double t, int k);
    }

    private static double[][] sample(Clip clip, double[] times, Trajectory trajectory, int... missing) {
        double[][] positions = new double[clip.count()][];
        for (int k = 0; k < clip.count(); k++) positions[k] = trajectory.at(times[k], k);
        for (int k : missing) if (k < positions.length) positions[k] = null;
        return positions;
    }

    private static double[] uniformTimes(int count, double startMs, double durationMs, int stepSize) {
        double[] times = new double[count];
        // VideoPlayer.getStepTime(k) / 1000.0 for a constant frame duration, in its operand order:
        // StepperClipControl.getStepTime returns k * frameDuration * stepSize, and VideoPlayer adds the start.
        for (int k = 0; k < count; k++) times[k] = (k * durationMs * stepSize + startMs) / 1000.0;
        return times;
    }

    private static List<String> derivativeCases() {
        List<String> cases = new ArrayList<>();
        // Free fall from a nonzero time origin, 30 fps: exact for the quadratic.
        Clip fall = new Clip(0, 1, 25);
        double[] fallTimes = uniformTimes(25, 250.0, 1000.0 / 30.0, 1);
        double[][] fallPositions = sample(fall, fallTimes, (t, k) -> new double[] {0.5 + 1.25 * t, 2.0 + 3.0 * t - 4.9 * t * t});
        addAllAlgorithms(cases, "free-fall", fall, fallTimes, fallPositions);
        // Noisy circular-ish motion with single, double and edge gaps.
        Noise noise = new Noise(7);
        Clip noisy = new Clip(0, 1, 40);
        double[] noisyTimes = uniformTimes(40, 0.0, 40.0, 1);
        Rotor turn = new Rotor(1.0, 0.0, 0.9928086358538663, 0.11971220728891936); // 3 rad/s at 25 fps
        double[][] noisyPositions = sample(noisy, noisyTimes, (t, k) -> {
            double[] u = turn.next();
            return new double[] {u[0] + 0.01 * noise.next(), u[1] + 0.01 * noise.next()};
        }, 0, 9, 20, 21, 29, 30, 31, 39);
        noisyPositions[15] = new double[] {Double.NaN, 0.5}; // refreshData removes a step with a NaN world coordinate
        addAllAlgorithms(cases, "noisy-gaps", noisy, noisyTimes, noisyPositions);
        // A bouncing ball: velocity reverses at impacts between frames.
        Clip bounce = new Clip(1234, 1, 50);
        double[] bounceTimes = uniformTimes(50, -500.0, 1000.0 / 60.0, 1);
        double[][] bouncePositions = sample(bounce, bounceTimes, (t, k) -> {
            double period = 0.55;
            double phase = (t + 0.5) % period;
            double v0 = 9.8 * period / 2;
            return new double[] {0.3 * t, v0 * phase - 4.9 * phase * phase};
        }, 37);
        addAllAlgorithms(cases, "bounce", bounce, bounceTimes, bouncePositions);
        // A clip stride of 2 from frame 7, and of 3 with variable step times (Tracker's uniform stencil).
        Clip stride = new Clip(7, 2, 20);
        double[] strideTimes = uniformTimes(20, 1000.0, 10.0, 2);
        double[][] stridePositions = sample(stride, strideTimes, (t, k) -> new double[] {t * t * t, 2.0 - t}, 12);
        addAllAlgorithms(cases, "stride-2", stride, strideTimes, stridePositions);
        Clip variable = new Clip(3, 3, 18);
        double[] variableTimes = new double[18];
        for (int k = 0; k < 18; k++) variableTimes[k] = (100.0 * k + (k % 3 == 1 ? 7.0 : k % 3 == 2 ? -4.0 : 0.0)) / 1000.0;
        double[][] variablePositions = sample(variable, variableTimes, (t, k) -> new double[] {2.0 * t, 1.0 - 0.5 * t * t});
        addAllAlgorithms(cases, "stride-3-variable-time", variable, variableTimes, variablePositions);
        // Degenerate lengths and an all-missing track.
        for (int count = 0; count <= 8; count++) {
            Clip clip = new Clip(5, 1, count);
            double[] times = uniformTimes(count, 0.0, 100.0, 1);
            addAllAlgorithms(cases, "length-" + count, clip, times, sample(clip, times, (t, k) -> new double[] {k * k, -k}));
        }
        Clip empty = new Clip(0, 1, 10);
        double[] emptyTimes = uniformTimes(10, 0.0, 100.0, 1);
        double[][] none = new double[10][];
        addAllAlgorithms(cases, "all-missing", empty, emptyTimes, none);
        // Isolated samples among gaps: Bounce Detection extrapolates from a shifted window.
        Clip sparse = new Clip(0, 1, 24);
        double[] sparseTimes = uniformTimes(24, 0.0, 50.0, 1);
        addAllAlgorithms(cases, "isolated", sparse, sparseTimes,
            sample(sparse, sparseTimes, (t, k) -> new double[] {k, k * 0.5 * k}, 3, 5, 6, 14, 16, 18, 19, 20, 22));
        // Coordinates so large that Bounce Detection's step-time weights overflow (Java recurses forever).
        Clip huge = new Clip(0, 1, 12);
        double[] hugeTimes = uniformTimes(12, 0.0, 100.0, 1);
        addAllAlgorithms(cases, "huge", huge, hugeTimes,
            sample(huge, hugeTimes, (t, k) -> new double[] {1e200 * (k < 6 ? k : 12 - k), 1e200 * k}));
        return cases;
    }

    private static void addAllAlgorithms(List<String> cases, String id, Clip clip, double[] times, double[][] positions) {
        List<String> results = new ArrayList<>();
        results.add(derivativeResult("finite_difference", clip, times, positions));
        results.add(derivativeResult("smooth_finite_difference", clip, times, positions));
        results.add(derivativeResult("bounce_detection", clip, times, positions));
        if (clip.step() > 1) {
            // Tracker's Bounce Detection fails for a stride above 1. The same samples on a stride-1 clip
            // give the reference values that MoTrack's stride-independent evaluation must reproduce.
            results.add(derivativeResult("bounce_detection_compacted", new Clip(clip.first(), 1, clip.count()), times, positions));
        }
        cases.add("{\"id\":" + string(id) + ",\"clip\":{\"first_frame\":" + clip.first() + ",\"step_size\":" + clip.step()
            + ",\"step_count\":" + clip.count() + "},\"times\":" + numbers(times) + ",\"positions\":" + points(positions)
            + ",\"results\":" + join(results) + "}");
    }

    /** PointMass.updateDerivatives(panel, start, count) followed by refreshData's time scaling. */
    private static String derivativeResult(String algorithm, Clip clip, double[] times, double[][] positions) {
        int length = clip.count() == 0 ? TRACKER_PADDING : clip.frame(clip.count() - 1) + 1 + TRACKER_PADDING;
        double[] x = new double[length];
        double[] y = new double[length];
        boolean[] valid = new boolean[length];
        java.util.Arrays.fill(x, JUNK);
        java.util.Arrays.fill(y, -JUNK);
        for (int k = 0; k < clip.count(); k++) {
            if (positions[k] == null || Double.isNaN(positions[k][0]) || Double.isNaN(positions[k][1])) continue;
            x[clip.frame(k)] = positions[k][0];
            y[clip.frame(k)] = positions[k][1];
            valid[clip.frame(k)] = true;
        }
        boolean[] validData = valid.clone();
        int[] params = {0, clip.first(), clip.step(), clip.count()};
        double[] d1x, d1y, d2x, d2y;
        String error = null;
        try {
            if (algorithm.startsWith("bounce_detection")) {
                params[0] = 3;
                Object[] result = new BounceDerivatives().evaluate(new Object[] {params, x, y, valid});
                d1x = (double[]) result[0];
                d1y = (double[]) result[1];
                d2x = (double[]) result[2];
                d2y = (double[]) result[3];
            } else {
                params[0] = algorithm.equals("smooth_finite_difference") ? 2 : 1;
                Object[] result = new FirstDerivative().evaluate(new Object[] {params, x, y, valid});
                d1x = (double[]) result[0];
                d1y = (double[]) result[1];
                params[0] = 2;
                result = new SecondDerivative().evaluate(new Object[] {params, x, y, valid});
                d2x = (double[]) result[2];
                d2y = (double[]) result[3];
            }
        } catch (Throwable thrown) {
            error = thrown.getClass().getName();
            d1x = d1y = d2x = d2y = null;
        }
        StringBuilder json = new StringBuilder("{\"algorithm\":").append(string(algorithm)).append(",\"error\":")
            .append(error == null ? "null" : string(error));
        if (error == null) {
            double[][] first = new double[clip.count()][];
            double[][] second = new double[clip.count()][];
            double[][] velocity = new double[clip.count()][];
            double[][] acceleration = new double[clip.count()][];
            for (int k = 0; k < clip.count(); k++) {
                int n = clip.frame(k);
                // PointMass creates a vector only where validData[n] and xDeriv[n] is not NaN.
                if (validData[n] && !Double.isNaN(d1x[n])) {
                    first[k] = new double[] {d1x[n], d1y[n]};
                    double dtV = velocityStep(times, k);
                    // A NaN step time outside the clip leaves a NaN (blank) table cell.
                    if (!Double.isNaN(d1x[n] / dtV) && !Double.isNaN(d1y[n] / dtV)) velocity[k] = new double[] {d1x[n] / dtV, d1y[n] / dtV};
                }
                if (validData[n] && !Double.isNaN(d2x[n])) {
                    second[k] = new double[] {d2x[n], d2y[n]};
                    double dtA2 = accelerationStepSquared(times, k);
                    if (!Double.isNaN(d2x[n] / dtA2) && !Double.isNaN(d2y[n] / dtA2))
                        acceleration[k] = new double[] {d2x[n] / dtA2, d2y[n] / dtA2};
                }
            }
            json.append(",\"first\":").append(points(first)).append(",\"second\":").append(points(second))
                .append(",\"velocity\":").append(points(velocity)).append(",\"acceleration\":").append(points(acceleration));
        }
        return json.append('}').toString();
    }

    /** VideoPlayer.getStepTime returns NaN outside the clip. */
    private static double stepTime(double[] times, int k) {
        return k < 0 || k >= times.length ? Double.NaN : times[k];
    }

    // PointMass.refreshData: vDerivSpill = 1 and aDerivSpill = 2 for every algorithm.
    private static double velocityStep(double[] times, int k) {
        double tf = stepTime(times, k + 1);
        double to = stepTime(times, k - 1);
        return (tf - to) / (2 * 1);
    }

    private static double accelerationStepSquared(double[] times, int k) {
        double tf = stepTime(times, k + 2);
        double to = stepTime(times, k - 2);
        return (tf - to) * (tf - to) / (4 * 2 * 2);
    }

    // ------------------------------------------------------------------------------------------
    // One-dimensional differences (getRotationData passes yData = null) at both spills.

    private static List<String> differenceCases() {
        List<String> cases = new ArrayList<>();
        double[][] signals = {
            {1, 4, 9, 16, 25, 36, 49, 64, 81, 100},
            {0.1, Double.NaN, 0.3, 0.7, 1.9, 2.0, -1.5, 3.25, 1e-300, 8},
            {5},
            {},
        };
        boolean[][] masks = {
            null,
            {true, false, true, true, true, true, true, true, true, true},
            null,
            null,
        };
        for (int s = 0; s < signals.length; s++) {
            for (int spill = 1; spill <= 2; spill++) {
                double[] data = signals[s];
                int length = data.length + TRACKER_PADDING;
                double[] x = new double[length];
                boolean[] valid = new boolean[length];
                java.util.Arrays.fill(x, JUNK);
                Double[] samples = new Double[data.length];
                for (int i = 0; i < data.length; i++) {
                    boolean present = masks[s] == null || masks[s][i];
                    if (!present) continue;
                    x[i] = data[i];
                    valid[i] = true;
                    samples[i] = data[i];
                }
                int[] params = {spill, 0, 1, data.length};
                double[] first = (double[]) new FirstDerivative().evaluate(new Object[] {params, x, null, valid})[0];
                double[] second = (double[]) new SecondDerivative().evaluate(new Object[] {params, x, null, valid})[2];
                Double[] firstOut = new Double[data.length];
                Double[] secondOut = new Double[data.length];
                for (int i = 0; i < data.length; i++) {
                    if (!Double.isNaN(first[i])) firstOut[i] = first[i];
                    if (!Double.isNaN(second[i])) secondOut[i] = second[i];
                }
                cases.add("{\"id\":" + string("signal-" + s + "/spill-" + spill) + ",\"spill\":" + spill + ",\"samples\":"
                    + nullableNumbers(samples) + ",\"first\":" + nullableNumbers(firstOut) + ",\"second\":" + nullableNumbers(secondOut)
                    + "}");
            }
        }
        return cases;
    }

    // ------------------------------------------------------------------------------------------
    // Angular data: getRotationData's cumulative polar angle, then omega and alpha as in refreshData.

    private static List<String> angularCases() {
        List<String> cases = new ArrayList<>();
        {
            Clip clip = new Clip(0, 1, 90);
            double[] times = uniformTimes(90, 100.0, 1000.0 / 30.0, 1);
            // Over two counterclockwise turns about an offset centre (5 rad/s), crossing +/-pi repeatedly.
            Rotor turn = new Rotor(0.9861432315629250, 0.16589613269341502, 0.986143231562925, 0.16589613269341502);
            cases.add(angularCase("circle-ccw", times, sample(clip, times, (t, k) -> {
                double[] u = turn.next();
                return new double[] {0.2 + 1.5 * u[0], -0.1 + 1.5 * u[1]};
            }, 17, 18, 50)));
        }
        {
            Clip clip = new Clip(4, 2, 40);
            double[] times = uniformTimes(40, 0.0, 25.0, 2);
            // A clockwise spiral, -9 rad/s from a phase of 3 rad.
            Rotor turn = new Rotor(-0.9899924966004454, 0.1411200080598672, 0.9004471023526769, -0.43496553411123023);
            cases.add(angularCase("spiral-cw-stride", times, sample(clip, times, (t, k) -> {
                double[] u = turn.next();
                return new double[] {(1 + t) * u[0], (1 + t) * u[1]};
            })));
        }
        {
            Clip clip = new Clip(0, 1, 12);
            double[] times = uniformTimes(12, 0.0, 100.0, 1);
            // The origin, the negative x axis (atan2 = pi) and signed zeros.
            double[][] positions = {{0.0, 0.0}, {-1.0, 0.0}, {-1.0, -0.0}, {-1.0, 1e-17}, {0.0, -2.0}, {1.0, -1.0}, {1.0, 1.0},
                {-0.0, 1.0}, {-1.0, -1.0}, null, {3.0, 4.0}, {-3.0, -4.0}};
            cases.add(angularCase("axes", times, positions));
        }
        return cases;
    }

    private static String angularCase(String id, double[] times, double[][] positions) {
        int count = positions.length;
        int length = count + TRACKER_PADDING;
        double[] theta = new double[length];
        boolean[] valid = new boolean[length];
        double rotation = 0;
        double prevAngle = 0;
        for (int n = 0; n < length; n++) {
            if (n < count && positions[n] != null) {
                double angle = Math.atan2(positions[n][1], positions[n][0]);
                double delta = angle - prevAngle;
                if (delta < -Math.PI) delta += 2 * Math.PI;
                else if (delta > Math.PI) delta -= 2 * Math.PI;
                rotation += delta;
                prevAngle = angle;
                theta[n] = rotation;
                valid[n] = true;
            } else theta[n] = Double.NaN;
        }
        int[] params = {1, 0, 1, count};
        double[] omega = (double[]) new FirstDerivative().evaluate(new Object[] {params, theta, null, valid})[0];
        params[0] = 2;
        double[] alpha = (double[]) new SecondDerivative().evaluate(new Object[] {params, theta, null, valid})[2];
        Double[] angleOut = new Double[count];
        Double[] omegaOut = new Double[count];
        Double[] alphaOut = new Double[count];
        for (int k = 0; k < count; k++) {
            if (!valid[k]) continue;
            angleOut[k] = theta[k];
            double w = omega[k] / velocityStep(times, k);
            double a = alpha[k] / accelerationStepSquared(times, k);
            if (!Double.isNaN(w)) omegaOut[k] = w;
            if (!Double.isNaN(a)) alphaOut[k] = a;
        }
        return "{\"id\":" + string(id) + ",\"times\":" + numbers(times) + ",\"positions\":" + points(positions) + ",\"angle\":"
            + nullableNumbers(angleOut) + ",\"angular_velocity\":" + nullableNumbers(omegaOut) + ",\"angular_acceleration\":"
            + nullableNumbers(alphaOut) + "}";
    }

    // ------------------------------------------------------------------------------------------
    // Motion filters, applied the way FilteredPointMass.refreshPositions applies them.

    private record FilterSpec(String id, String type, double[] parameters) {}

    private static List<String> filterCases() {
        List<String> specs = new ArrayList<>();
        List<FilterSpec> filters = List.of(
            new FilterSpec("ma-default", "moving_average", new double[] {5}),
            new FilterSpec("ma-1", "moving_average", new double[] {1}),
            new FilterSpec("ma-3", "moving_average", new double[] {3}),
            new FilterSpec("ma-even-4", "moving_average", new double[] {4}),
            new FilterSpec("ma-zero", "moving_average", new double[] {0}),
            new FilterSpec("ma-negative", "moving_average", new double[] {-3}),
            new FilterSpec("ma-99", "moving_average", new double[] {99}),
            new FilterSpec("bw-default", "butterworth", new double[] {4, 6.0, 30.0}),
            new FilterSpec("bw-order-1", "butterworth", new double[] {1, 3.0, 30.0}),
            new FilterSpec("bw-order-2", "butterworth", new double[] {2, 8.0, 60.0}),
            new FilterSpec("bw-order-3", "butterworth", new double[] {3, 5.0, 100.0}),
            new FilterSpec("bw-order-5", "butterworth", new double[] {5, 12.0, 240.0}),
            new FilterSpec("bw-order-8", "butterworth", new double[] {8, 6.0, 30.0}),
            new FilterSpec("bw-order-0", "butterworth", new double[] {0, 6.0, 30.0}),
            new FilterSpec("bw-order-9", "butterworth", new double[] {9, 6.0, 30.0}),
            new FilterSpec("bw-near-nyquist", "butterworth", new double[] {4, 14.9, 30.0}),
            new FilterSpec("bw-at-nyquist", "butterworth", new double[] {4, 15.0, 30.0}),
            new FilterSpec("bw-cutoff-zero", "butterworth", new double[] {2, 0.0, 30.0}),
            new FilterSpec("bw-rate-negative", "butterworth", new double[] {2, 0.25, -5.0}),
            new FilterSpec("bw-low-cutoff", "butterworth", new double[] {4, 0.5, 240.0}),
            new FilterSpec("sg-default", "savitzky_golay", new double[] {7, 2}),
            new FilterSpec("sg-5-1", "savitzky_golay", new double[] {5, 1}),
            new FilterSpec("sg-9-3", "savitzky_golay", new double[] {9, 3}),
            new FilterSpec("sg-21-4", "savitzky_golay", new double[] {21, 4}),
            new FilterSpec("sg-99-6", "savitzky_golay", new double[] {99, 6}),
            new FilterSpec("sg-even-4", "savitzky_golay", new double[] {4, 2}),
            new FilterSpec("sg-order-high", "savitzky_golay", new double[] {3, 5}),
            new FilterSpec("sg-order-zero", "savitzky_golay", new double[] {7, 0}));
        for (String signal : FILTER_SIGNALS)
            SIGNAL_JSON.add("{\"id\":" + string(signal) + ",\"samples\":" + nullableNumbers(filterSignal(signal)) + "}");
        for (FilterSpec filter : filters) {
            specs.add(filterCase(filter, "noisy-gaps"));
            specs.add(filterCase(filter, "short-segments"));
            if (filter.id().endsWith("default") || filter.id().startsWith("sg-")) specs.add(filterCase(filter, "smooth"));
        }
        // Degenerate inputs for one filter of each type.
        for (String id : new String[] {"ma-default", "bw-default", "sg-default"}) {
            FilterSpec filter = filters.stream().filter(f -> f.id().equals(id)).findFirst().orElseThrow();
            for (String signal : new String[] {"empty", "single", "all-missing"}) specs.add(filterCase(filter, signal));
        }
        return specs;
    }

    private static final String[] FILTER_SIGNALS = {"noisy-gaps", "smooth", "short-segments", "empty", "single", "all-missing"};
    private static final List<String> SIGNAL_JSON = new ArrayList<>();

    private static MotionFilter create(FilterSpec spec) {
        double[] p = spec.parameters();
        return switch (spec.type()) {
            case "moving_average" -> new MovingAverageFilter((int) p[0]);
            case "butterworth" -> new ButterworthFilter((int) p[0], p[1], p[2]);
            case "savitzky_golay" -> new SavitzkyGolayFilter((int) p[0], (int) p[1]);
            default -> throw new IllegalArgumentException(spec.type());
        };
    }

    private static String normalized(MotionFilter filter) {
        if (filter instanceof MovingAverageFilter ma) return "{\"window\":" + ma.getWindow() + "}";
        if (filter instanceof ButterworthFilter bw)
            return "{\"order\":" + bw.getOrder() + ",\"cutoff_hz\":" + number(bw.getCutoffHz()) + ",\"sample_rate_hz\":"
                + number(bw.getSampleRateHz()) + "}";
        SavitzkyGolayFilter sg = (SavitzkyGolayFilter) filter;
        return "{\"window\":" + sg.getWindow() + ",\"poly_order\":" + sg.getPolyOrder() + "}";
    }

    private static Double[] filterSignal(String signal) {
        switch (signal) {
            case "noisy-gaps": {
                Noise noise = new Noise(42);
                Rotor slow = new Rotor(1.0, 0.0, 0.9856159103477085, 0.16900082032184907); // period 37 samples
                Rotor fast = new Rotor(1.0, 0.0, 0.30901699437494745, 0.9510565162951535); // period 5 samples
                Double[] values = new Double[80];
                for (int i = 0; i < values.length; i++) values[i] = slow.next()[1] + 0.3 * fast.next()[0] + 0.2 * noise.next();
                for (int i : new int[] {0, 25, 26, 47, 60, 61, 62, 63, 79}) values[i] = null;
                values[40] = Double.NaN; // FilteredPointMass marks NaN world positions invalid
                return values;
            }
            case "smooth": {
                Double[] values = new Double[40];
                for (int i = 0; i < values.length; i++) {
                    double t = i / 10.0;
                    values[i] = 0.25 + 1.5 * t - 0.75 * t * t + 0.0625 * t * t * t;
                }
                return values;
            }
            case "short-segments": {
                // Segments of 1, 2, 3, 4, 5 and 6 samples separated by single gaps (NaN marks a gap here).
                final double gap = Double.NaN;
                double[] data = {3, gap, 4, 5, gap, 6, 7, 8, gap, 1, 2, 4, 8, gap, 9, 9, 9, 9, 2, gap, 0.5, -1, 0.25, -2, 4, -8};
                Double[] values = new Double[data.length];
                for (int i = 0; i < data.length; i++) values[i] = Double.isNaN(data[i]) ? null : data[i];
                return values;
            }
            case "empty":
                return new Double[0];
            case "single":
                return new Double[] {2.5};
            case "all-missing":
                return new Double[6];
            default:
                throw new IllegalArgumentException(signal);
        }
    }

    private static String filterCase(FilterSpec spec, String signal) {
        Double[] samples = filterSignal(signal);
        int n = samples.length;
        double[] data = new double[n];
        boolean[] valid = new boolean[n];
        for (int i = 0; i < n; i++) {
            // FilteredPointMass: valid only for a non-null step with a non-NaN world position.
            valid[i] = samples[i] != null && !Double.isNaN(samples[i]);
            data[i] = valid[i] ? samples[i] : JUNK;
        }
        MotionFilter filter = create(spec);
        double[] output = filter.apply(data, valid);
        Double[] out = new Double[n];
        for (int i = 0; i < n; i++) {
            if (!valid[i]) {
                if (output[i] != data[i]) throw new IllegalStateException("Filter changed an invalid sample in " + spec.id());
                continue;
            }
            out[i] = output[i];
        }
        return "{\"id\":" + string(spec.id() + "/" + signal) + ",\"type\":" + string(spec.type()) + ",\"parameters\":"
            + numbers(spec.parameters()) + ",\"normalized\":" + normalized(filter) + ",\"signal\":" + string(signal)
            + ",\"output\":" + nullableNumbers(out) + "}";
    }

    // ------------------------------------------------------------------------------------------
    // OSP numerics.Derivative on the function of OSP's src/test/Derivative.java benchmark.

    private static double testFunction(double x) {
        return x * Math.log(x) * Math.sin(x) * Math.cos(x);
    }

    private static List<String> numericalCases() {
        List<String> cases = new ArrayList<>();
        Function f = MotionOracle::testFunction;
        Function cubic = x -> x * x * x - 2.0 * x;
        double h = 0.1;
        for (double x : new double[] {1.0, 2.5, 10.0, 1000.0, 999999.0}) {
            cases.add(numerical("first", "x-log-sin-cos", x, h, Derivative.first(f, x, h)));
            cases.add(numerical("centered", "x-log-sin-cos", x, h, Derivative.centered(f, x, h)));
            cases.add(numerical("get-first", "x-log-sin-cos", x, h, Derivative.getFirst(f, h).evaluate(x)));
            cases.add(numerical("forward", "x-log-sin-cos", x, h, Derivative.forward(f, x, h)));
            cases.add(numerical("backward", "x-log-sin-cos", x, h, Derivative.backward(f, x, h)));
            cases.add(numerical("second", "x-log-sin-cos", x, h, Derivative.second(f, x, h)));
            cases.add(numerical("get-second", "x-log-sin-cos", x, h, Derivative.getSecond(f, h).evaluate(x)));
        }
        for (double x : new double[] {-3.0, 0.0, 0.7}) {
            for (double step : new double[] {0.5, 1e-3}) {
                cases.add(numerical("first", "cubic", x, step, Derivative.first(cubic, x, step)));
                cases.add(numerical("forward", "cubic", x, step, Derivative.forward(cubic, x, step)));
                cases.add(numerical("backward", "cubic", x, step, Derivative.backward(cubic, x, step)));
                cases.add(numerical("second", "cubic", x, step, Derivative.second(cubic, x, step)));
            }
        }
        // The benchmark sum: sum of getFirst(f, i, 0.1) for i = 1 .. 999999 (src/test/Derivative.java).
        double sum = 0;
        DoubleUnaryOperator wrapped = x -> Derivative.first(f, x, 0.1);
        for (int i = 1; i < 1000000; i++) sum += wrapped.applyAsDouble(i);
        cases.add("{\"method\":\"benchmark-sum\",\"function\":\"x-log-sin-cos\",\"x\":null,\"h\":" + number(0.1) + ",\"value\":"
            + number(sum) + "}");
        MultiVarFunction surface = v -> v[0] * v[0] * v[1] - 3.0 * v[1] + v[2] / 4.0;
        for (int index = 0; index < 3; index++) {
            double[] point = {1.5, -2.0, 0.25};
            double value = Derivative.firstPartial(surface, point, index, 0.01);
            cases.add("{\"method\":\"partial\",\"function\":\"surface\",\"point\":" + numbers(point) + ",\"index\":" + index
                + ",\"h\":" + number(0.01) + ",\"value\":" + number(value) + "}");
        }
        cases.add(romberg("x-log-sin-cos", f, 2.0, 0.5, 1e-10));
        cases.add(romberg("x-log-sin-cos", f, 2.0, 0.5, 0.0));
        cases.add(romberg("cubic", cubic, 0.7, 1e-161, 1e-10));
        cases.add(romberg("cubic", cubic, 0.7, 3e-162, 1e-10));
        cases.add(romberg("cubic", cubic, 0.7, 0.25, 1e-12));
        cases.add(romberg("cubic", cubic, 0.7, -0.25, 1e-12));
        return cases;
    }

    private static String numerical(String method, String function, double x, double h, double value) {
        return "{\"method\":" + string(method) + ",\"function\":" + string(function) + ",\"x\":" + number(x) + ",\"h\":" + number(h)
            + ",\"value\":" + number(value) + "}";
    }

    private static String romberg(String function, Function f, double x, double h, double tolerance) {
        String status;
        double value;
        try {
            value = Derivative.romberg(f, x, h, tolerance, null);
            status = "converged";
        } catch (NumericMethodException error) {
            value = error.error_value;
            status = error.error_code == 2 ? "step_below_precision" : "not_converged";
            if (error.error_code != 1 && error.error_code != 2) throw new IllegalStateException("Unexpected Romberg code");
        }
        return "{\"method\":\"romberg\",\"function\":" + string(function) + ",\"x\":" + number(x) + ",\"h\":" + number(h)
            + ",\"tolerance\":" + number(tolerance) + ",\"status\":" + string(status) + ",\"value\":" + number(value) + "}";
    }

    // ------------------------------------------------------------------------------------------
    // JSON helpers.

    private static String join(List<String> items) {
        return "[" + String.join(",", items) + "]";
    }

    private static String points(double[][] values) {
        StringBuilder json = new StringBuilder("[");
        for (int i = 0; i < values.length; i++) {
            if (i > 0) json.append(',');
            json.append(values[i] == null ? "null" : numbers(values[i]));
        }
        return json.append(']').toString();
    }

    private static String numbers(double[] values) {
        StringBuilder json = new StringBuilder("[");
        for (int i = 0; i < values.length; i++) {
            if (i > 0) json.append(',');
            json.append(number(values[i]));
        }
        return json.append(']').toString();
    }

    private static String nullableNumbers(Double[] values) {
        StringBuilder json = new StringBuilder("[");
        for (int i = 0; i < values.length; i++) {
            if (i > 0) json.append(',');
            json.append(values[i] == null ? "null" : number(values[i]));
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
