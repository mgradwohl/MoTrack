import java.awt.Component;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.math.BigDecimal;
import java.math.MathContext;
import java.math.RoundingMode;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import javax.swing.JSplitPane;
import org.opensourcephysics.analysis.FourierSinCosAnalysis;
import org.opensourcephysics.display.Dataset;
import org.opensourcephysics.display.HighlightableDataset;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.display.UncertainFunctionDrawer;
import org.opensourcephysics.numerics.FFT;
import org.opensourcephysics.numerics.FFTReal;
import org.opensourcephysics.tools.DataColumn;
import org.opensourcephysics.tools.DataToolTab;
import org.opensourcephysics.tools.DatasetCurveFitter;
import org.opensourcephysics.tools.FourierPanel;
import org.opensourcephysics.tools.KnownPolynomial;

/** Runs the Data Tool analysis of the pinned Tracker 6.3.5 JAR headless on fixed datasets.
 *
 * The statistics table, the measuring tools and the Fourier panel compute inside Swing classes
 * whose constructors need a display (DataToolTable asks the toolkit for menu shortcuts). The
 * oracle therefore allocates the owning objects without their constructors, gives them only the
 * fields the computation reads, and calls the real methods: DataToolStatsTable.getStatistics,
 * DataToolPlotter.refreshArea/refreshMeasurements, DataToolTab.findNearestXIndex,
 * DatasetCurveFitter.doLinearRegression, FourierPanel.createFourierData with
 * FourierSinCosAnalysis.getDatasets, FFTReal and FFT. Column, working and selected datasets are
 * built through DataColumn.setPoints, WorkingDataset.setXSource and Dataset.append as Data Tool
 * builds them. Only the row gathering of the statistics table and getSelectedData (plain array
 * copies from JTable selections) is restated here. See docs/data-analysis-kernels.md.
 */
public final class AnalysisOracle {
    private static final String TOOLS = "org.opensourcephysics.tools.";
    private static final sun.misc.Unsafe UNSAFE = unsafe();
    private static final double NAN = Double.NaN;
    private static final double INF = Double.POSITIVE_INFINITY;

    private record Column(String id, String description, String defect, double[] values) {}

    private record Series(String id, String description, double[] x, double[] y) {}

    private static final List<Column> COLUMNS = new ArrayList<>();
    private static final List<Series> SERIES = new ArrayList<>();
    private static final List<String> CASES = new ArrayList<>();

    // ------------------------------------------------------------------------------------------
    // Deterministic inputs. The 64-bit LCG (Knuth's MMIX constants) and the conversion to [0, 1)
    // are exact integer and power-of-two operations, and StrictMath is fdlibm on every JVM, so the
    // inputs are identical on every platform; the fixture stores them for the C++ tests.

    private static final class Lcg {
        private long state;

        Lcg(long seed) {
            state = seed;
        }

        double next() {
            state = state * 6364136223846793005L + 1442695040888963407L;
            return (state >>> 11) * 0x1.0p-53;
        }
    }

    private static double[] filled(int n, double value) {
        double[] values = new double[n];
        java.util.Arrays.fill(values, value);
        return values;
    }

    private static double[] uniform(long seed, int n, double scale, double offset) {
        Lcg random = new Lcg(seed);
        double[] values = new double[n];
        for (int i = 0; i < n; i++) values[i] = offset + scale * random.next();
        return values;
    }

    /** Times t0 + i dt, as Tracker's constant frame timing produces them. */
    private static double[] times(int n, double t0, double dt) {
        double[] values = new double[n];
        for (int i = 0; i < n; i++) values[i] = t0 + i * dt;
        return values;
    }

    private static void columns() {
        column("empty", "no values", null);
        column("all-nan", "only missing values", null, NAN, NAN);
        column("single", "one value", null, 2.5);
        column("pair", "two values", null, 1.0, 4.0);
        column("textbook", "2 4 4 4 5 5 7 9: mean 5, sample sd sqrt(32/7)", null, 2, 4, 4, 4, 5, 5, 7, 9);
        column("gaps", "missing values between values", null, 1.0, NAN, 3.0, NAN, 5.0, 7.0);
        column("frame-times", "t = 0.04 i for 100 frames", null, times(100, 0.0, 0.04));
        column("negative-times", "t = -1.5 + 0.1 i for 31 frames", null, times(31, -1.5, 0.1));
        column("noise", "1000 uniform values in [-5, 5)", null, uniform(17, 1000, 10.0, -5.0));
        column("pixels", "200 positions near 412.3 px", null, uniform(29, 200, 0.75, 412.0));
        column("signed-zeros", "0 -0 0", null, 0.0, -0.0, 0.0);
        column("constant-three", "3 3 3 3: sd 0", null, filled(4, 3.0));
        column("constant-tenth", "seven times 0.1: sd 0", "one-pass-variance", filled(7, 0.1));
        column("constant-third", "five times 1/3: sd 0", "one-pass-variance", filled(5, 1.0 / 3.0));
        column("offset-1e8", "1e8 + 0 1 2: sd 1", "one-pass-variance", 1e8, 1e8 + 1, 1e8 + 2);
        double[] clock = uniform(41, 50, 1.0, 0.0);
        for (int i = 0; i < clock.length; i++) clock[i] += 1e9;
        column("offset-1e9", "1e9 + 50 uniform values in [0, 1)", "one-pass-variance", clock);
        column("huge", "1e200 -1e200 1e200: sd 2e200/sqrt(3)", "one-pass-variance", 1e200, -1e200, 1e200);
        column("overflowing-sum", "1e308 1e308: mean 1e308, sd 0", "overflow", 1e308, 1e308);
        column("overflowing-deviation", "1.5e308 -1.5e308 -1.5e308: mean -5e307, sd 3e308/sqrt(3)", "overflow", 1.5e308, -1.5e308, -1.5e308);
        column("tiny", "1e-200 2e-200 3e-200: sd 1e-200", "one-pass-variance", 1e-200, 2e-200, 3e-200);
        column("infinite", "1 inf 3", null, 1.0, INF, 3.0);
        column("opposite-infinities", "-inf inf", null, -INF, INF);
        column("negative-infinity", "-inf -inf", "max-sentinel", -INF, -INF);
        column("most-negative", "-MAX_VALUE -MAX_VALUE", "max-sentinel", -Double.MAX_VALUE, -Double.MAX_VALUE);
        column("most-positive", "MAX_VALUE MAX_VALUE", "max-sentinel", Double.MAX_VALUE, Double.MAX_VALUE);
    }

    private static void column(String id, String description, String defect, double... values) {
        COLUMNS.add(new Column(id, description, defect, values));
    }

    private static void series() {
        double[] x = times(21, 0.0, 0.1);
        double[] parabola = new double[x.length];
        double[] line = new double[x.length];
        for (int i = 0; i < x.length; i++) {
            parabola[i] = x[i] * x[i];
            line[i] = 3.0 * x[i] - 2.0;
        }
        SERIES.add(new Series("parabola", "y = x^2 on [0, 2], h = 0.1", x, parabola));
        SERIES.add(new Series("line", "y = 3 x - 2 on [0, 2]", x, line));
        double[] jitter = uniform(53, 25, 0.08, 0.06);
        double[] uneven = new double[25];
        double[] sine = new double[25];
        double position = 0.0;
        for (int i = 0; i < uneven.length; i++) {
            uneven[i] = position;
            sine[i] = StrictMath.sin(position);
            position += jitter[i];
        }
        SERIES.add(new Series("uneven-sine", "y = sin x at 25 unequally spaced x", uneven, sine));
        double[] gapX = times(12, 0.0, 0.5);
        double[] gapY = new double[12];
        for (int i = 0; i < gapY.length; i++) gapY[i] = 2.0 * gapX[i] + 1.0;
        gapY[4] = NAN;
        gapY[5] = NAN;
        gapY[9] = NAN;
        SERIES.add(new Series("y-gaps", "y = 2 x + 1 with missing y at rows 4, 5 and 9", gapX, gapY));
        double[] missingX = times(10, 1.0, 1.0);
        double[] missingXY = new double[10];
        for (int i = 0; i < missingXY.length; i++) missingXY[i] = missingX[i] * missingX[i];
        missingX[0] = NAN;
        missingX[6] = NAN;
        SERIES.add(new Series("x-gaps", "y = x^2 with missing x at rows 0 and 6", missingX, missingXY));
        SERIES.add(new Series("infinite-y", "y with an infinite value at row 2", times(6, 0.0, 1.0),
                new double[] {1.0, 2.0, INF, 4.0, 5.0, 6.0}));
        SERIES.add(new Series("path", "a path whose x reverses: x = cos t, y = sin t, t = 0..pi", circleX(), circleY()));
        SERIES.add(new Series("repeated-x", "x = 0 1 1 2 3 3", new double[] {0, 1, 1, 2, 3, 3}, new double[] {0, 1, 2, 3, 4, 5}));
        SERIES.add(new Series("empty", "no rows", new double[0], new double[0]));
        SERIES.add(new Series("single", "one row", new double[] {2.0}, new double[] {5.0}));
        SERIES.add(new Series("two", "two rows", new double[] {1.0, 3.0}, new double[] {2.0, 6.0}));
        SERIES.add(new Series("all-gaps", "no y values", new double[] {0, 1, 2}, new double[] {NAN, NAN, NAN}));
        SERIES.add(new Series("short-y", "the y column is two rows shorter", times(6, 0.0, 1.0), new double[] {1, 2, 3, 4}));
        // Fourier inputs: sinusoids at known bins, phase origins, factorizations and degenerate spacing.
        SERIES.add(fourier("sine-64", 64, 0.0, 0.05, new double[][] {{0.5, 0, 0}, {3.0, 2.5, 0.25}, {2.0, 1.25, 0}}));
        SERIES.add(fourier("shifted-60", 60, 2.3, 0.1, new double[][] {{-1.0, 0, 0}, {1.5, 0.5, 0}, {0.75, 2.0, 0.25}}));
        SERIES.add(fourier("odd-61", 61, 0.0, 0.1, new double[][] {{0, 0, 0}, {2.0, 1.0, 0.25}}));
        SERIES.add(fourier("nyquist-16", 16, 0.5, 0.1, new double[][] {{0, 0, 0}, {3.0, 2.5, 0.25}, {2.0, 1.25, 0}, {0.5, 5.0, 0}}));
        SERIES.add(fourier("nyquist-shifted-12", 12, 0.3, 0.1, new double[][] {{0, 0, 0}, {1.0, 5.0, 0}}));
        SERIES.add(fourier("mod4-30", 30, 0.0, 0.1, new double[][] {{0, 0, 0}, {1.0, 7.0 / 3.0, 0}, {0.5, 8.0 / 3.0, 0.25}}));
        SERIES.add(noise("noise-10", 10, 61));
        SERIES.add(noise("noise-22", 22, 67));
        SERIES.add(noise("noise-26", 26, 71));
        SERIES.add(noise("noise-42", 42, 73));
        SERIES.add(noise("noise-98", 98, 79));
        SERIES.add(noise("noise-128", 128, 83));
        SERIES.add(new Series("two-points", "two points", new double[] {0.0, 0.5}, new double[] {1.0, 3.0}));
        SERIES.add(new Series("three-points", "three points: the last is dropped", new double[] {0.0, 0.5, 1.0},
                new double[] {1.0, 3.0, 8.0}));
        SERIES.add(new Series("constant-8", "a constant signal", times(8, 0.0, 0.25), filled(8, 2.5)));
        SERIES.add(new Series("descending-8", "x decreasing", times(8, 1.75, -0.25), new double[] {0, 1, 0, -1, 0, 1, 0, -1}));
        SERIES.add(new Series("gap-8", "a missing y value", times(8, 0.0, 1.0), new double[] {0, 1, 0, NAN, 0, 1, 0, -1}));
        SERIES.add(new Series("repeated-start-8", "x[1] = x[0]", new double[] {0, 0, 1, 2, 3, 4, 5, 6},
                new double[] {0, 1, 0, -1, 0, 1, 0, -1}));
        SERIES.add(new Series("same-x-8", "every x equal", filled(8, 1.0), new double[] {0, 1, 0, -1, 0, 1, 0, -1}));
    }

    private static double[] circleX() {
        double[] values = new double[13];
        for (int i = 0; i < values.length; i++) values[i] = StrictMath.cos(Math.PI * i / 12);
        return values;
    }

    private static double[] circleY() {
        double[] values = new double[13];
        for (int i = 0; i < values.length; i++) values[i] = StrictMath.sin(Math.PI * i / 12);
        return values;
    }

    /** terms: {amplitude, frequency, phase in cycles}; a zero frequency is a constant. */
    private static Series fourier(String id, int n, double t0, double dt, double[][] terms) {
        double[] t = times(n, t0, dt);
        double[] y = new double[n];
        StringBuilder description = new StringBuilder("y =");
        for (double[] term : terms) {
            description.append(String.format(" + %s cos(2 pi (%s t - %s))", term[0], term[1], term[2]));
        }
        for (int i = 0; i < n; i++) {
            for (double[] term : terms) y[i] += term[0] * StrictMath.cos(2 * Math.PI * (term[1] * t[i] - term[2]));
        }
        return new Series(id, description + String.format(", t = %s + %s i, %d points", t0, dt, n), t, y);
    }

    private static Series noise(String id, int n, long seed) {
        return new Series(id, n + " uniform values in [-1, 1) at t = 0.02 i", times(n, 0.0, 0.02), uniform(seed, n, 2.0, -1.0));
    }

    // ------------------------------------------------------------------------------------------
    // Reflection over the pinned classes.

    private static sun.misc.Unsafe unsafe() {
        try {
            Field field = sun.misc.Unsafe.class.getDeclaredField("theUnsafe");
            field.setAccessible(true);
            return (sun.misc.Unsafe) field.get(null);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }

    private static Field field(Class<?> type, String name) throws NoSuchFieldException {
        for (Class<?> c = type; c != null; c = c.getSuperclass()) {
            try {
                Field field = c.getDeclaredField(name);
                field.setAccessible(true);
                return field;
            } catch (NoSuchFieldException e) {
                // try the superclass
            }
        }
        throw new NoSuchFieldException(name);
    }

    private static Object get(Object target, String name) throws ReflectiveOperationException {
        return field(target.getClass(), name).get(target);
    }

    private static void set(Object target, String name, Object value) throws ReflectiveOperationException {
        field(target.getClass(), name).set(target, value);
    }

    private static Method method(Class<?> type, String name, Class<?>... parameters) throws NoSuchMethodException {
        Method method = type.getDeclaredMethod(name, parameters);
        method.setAccessible(true);
        return method;
    }

    private static Object invoke(Method method, Object target, Object... arguments) throws ReflectiveOperationException {
        try {
            return method.invoke(target, arguments);
        } catch (InvocationTargetException e) {
            if (e.getCause() instanceof RuntimeException runtime) throw runtime;
            throw e;
        }
    }

    /** The objects that the plot's measuring code reads, wired without Swing constructors. */
    private static final class Harness {
        final DataToolTab tab;
        final Object table;
        final Object plot;
        final Object[] limits;
        final DatasetCurveFitter fitter;
        final Object statsTable;
        final Class<?> tableClass;
        final Constructor<?> workingConstructor;
        final Method setXSource;
        final Method refreshArea;
        final Method refreshMeasurements;
        final Method getStatistics;
        final JSplitPane dataOnly = new JSplitPane();
        final JSplitPane withFitter;

        Harness() throws ReflectiveOperationException {
            tableClass = Class.forName(TOOLS + "DataToolTable");
            tab = (DataToolTab) UNSAFE.allocateInstance(DataToolTab.class);
            table = UNSAFE.allocateInstance(tableClass);
            fitter = (DatasetCurveFitter) UNSAFE.allocateInstance(DatasetCurveFitter.class);
            set(tab, "dataTable", table);
            set(tab, "haveGUI", true); // getCurveFitter() would otherwise build the GUI
            set(tab, "curveFitter", fitter);
            set(tab, "dataTool", UNSAFE.allocateInstance(Class.forName(TOOLS + "DataTool")));
            withFitter = new JSplitPane() {
                @Override
                public Component getBottomComponent() {
                    return fitter; // isFitterVisible(): the fitter is the bottom component
                }
            };
            set(tab, "splitPanes", new JSplitPane[] {new JSplitPane(), dataOnly});
            Class<?> plotter = Class.forName(TOOLS + "DataToolTab$DataToolPlotter");
            Constructor<?> plotConstructor = plotter.getDeclaredConstructor(DataToolTab.class, Dataset.class);
            plotConstructor.setAccessible(true);
            plot = plotConstructor.newInstance(tab, null);
            set(tab, "plot", plot);
            limits = (Object[]) get(plot, "areaLimits");
            Class<?> working = Class.forName(TOOLS + "DataToolTable$WorkingDataset");
            workingConstructor = working.getDeclaredConstructor(tableClass, Dataset.class);
            workingConstructor.setAccessible(true);
            setXSource = method(working, "setXSource", Dataset.class);
            refreshArea = method(plotter, "refreshArea");
            refreshMeasurements = method(plotter, "refreshMeasurements");
            Class<?> stats = Class.forName(TOOLS + "DataToolStatsTable");
            statsTable = UNSAFE.allocateInstance(stats);
            getStatistics = method(stats, "getStatistics", double[].class);
        }

        /** DataToolTable.getWorkingData for an x and a y DataColumn. */
        HighlightableDataset working(DataColumn x, DataColumn y) throws ReflectiveOperationException {
            HighlightableDataset data = (HighlightableDataset) workingConstructor.newInstance(table, y);
            invoke(setXSource, data, x);
            set(table, "workingData", data);
            return data;
        }

        void fitMode(double[] coefficients, double pixelsPerUnit, double pixelOrigin) throws ReflectiveOperationException {
            set(fitter, "drawer", new UncertainFunctionDrawer(new KnownPolynomial(coefficients)));
            set(tab, "splitPanes", new JSplitPane[] {new JSplitPane(), withFitter});
            set(tab, "measureFit", true);
            set(tab, "toggleMeasurement", false);
            double[] matrix = (double[]) get(plot, "pixelMatrix");
            matrix[0] = pixelsPerUnit;
            matrix[4] = pixelOrigin;
            set(plot, "xPixPerUnit", pixelsPerUnit);
            set(plot, "xmin", -pixelOrigin / pixelsPerUnit);
            set(plot, "leftGutter", 0);
        }

        void dataMode() throws ReflectiveOperationException {
            set(fitter, "drawer", null);
            set(tab, "splitPanes", new JSplitPane[] {new JSplitPane(), dataOnly});
            set(tab, "measureFit", false);
            set(tab, "toggleMeasurement", false);
        }
    }

    private static DataColumn dataColumn(double[] values) {
        DataColumn column = new DataColumn();
        column.setPoints(values, values.length);
        return column;
    }

    /** DataToolTable.getSelectedData: the x column with y padded by NaN, or the selected rows. */
    private static HighlightableDataset selected(DataColumn xColumn, DataColumn yColumn, int[] rows) {
        double[] x = xColumn.getYPoints();
        double[] y = yColumn.getYPoints();
        double[] xValues;
        double[] yValues;
        if (rows == null) {
            xValues = x;
            yValues = new double[x.length];
            for (int i = 0; i < yValues.length; i++) yValues[i] = i < y.length ? y[i] : NAN;
        } else {
            xValues = new double[rows.length];
            yValues = new double[rows.length];
            for (int i = 0; i < rows.length; i++) {
                xValues[i] = rows[i] >= x.length ? NAN : x[rows[i]];
                yValues[i] = rows[i] >= y.length ? NAN : y[rows[i]];
            }
        }
        HighlightableDataset data = new HighlightableDataset();
        data.append(xValues, yValues);
        return data;
    }

    private static String points(Dataset data) {
        double[] x = new double[data.getIndex()];
        double[] y = new double[data.getIndex()];
        System.arraycopy(data.getXPointsRaw(), 0, x, 0, x.length);
        System.arraycopy(data.getYPointsRaw(), 0, y, 0, y.length);
        return "{\"x\":" + numbers(x) + ",\"y\":" + numbers(y) + "}";
    }

    // ------------------------------------------------------------------------------------------
    // Cases.

    private static void statistics(Harness harness) throws ReflectiveOperationException {
        for (Column column : COLUMNS) {
            statisticsCase(harness, column, null);
        }
        statisticsCase(harness, find("textbook"), new int[] {1, 2, 3, 7});
        statisticsCase(harness, find("gaps"), new int[] {0, 1, 2});
        statisticsCase(harness, find("gaps"), new int[] {1, 3});
        statisticsCase(harness, find("gaps"), new int[] {4, 5, 6, 9});
        statisticsCase(harness, find("noise"), new int[] {10, 11, 12, 13, 14, 500, 999});
    }

    private static Column find(String id) {
        return COLUMNS.stream().filter(column -> column.id().equals(id)).findFirst().orElseThrow();
    }

    private static void statisticsCase(Harness harness, Column column, int[] rows) throws ReflectiveOperationException {
        double[] values = column.values();
        double[] selected = values;
        if (rows != null) {
            // refreshStatistics: model.getValueAt(row, column), a null cell (past the end) as NaN.
            selected = new double[rows.length];
            for (int i = 0; i < rows.length; i++) selected[i] = rows[i] < values.length ? values[rows[i]] : NAN;
        }
        Object[] result = (Object[]) invoke(harness.getStatistics, harness.statsTable, (Object) selected);
        String id = "statistics/" + column.id() + (rows == null ? "" : "/rows=" + joined(rows));
        CASES.add("{\"id\":" + string(id) + ",\"kind\":\"statistics\",\"column\":" + string(column.id())
                + ",\"rows\":" + integers(rows) + ",\"defect\":" + nullableString(column.defect())
                + ",\"max\":" + number((Double) result[0]) + ",\"min\":" + number((Double) result[1])
                + ",\"mean\":" + number((Double) result[2]) + ",\"sd\":" + number((Double) result[3])
                + ",\"se\":" + number((Double) result[4]) + ",\"count\":" + result[5] + "}");
    }

    private static void plots(Harness harness) throws ReflectiveOperationException {
        for (Series series : SERIES) {
            DataColumn x = dataColumn(series.x());
            DataColumn y = dataColumn(series.y());
            HighlightableDataset working = harness.working(x, y);
            HighlightableDataset selected = selected(x, y, null);
            // Null when the stored columns equal the input and the selected data equals the working data.
            boolean stored = java.util.Arrays.equals(x.getYPoints(), series.x()) && java.util.Arrays.equals(y.getYPoints(), series.y());
            String workingPoints = points(working);
            String selectedPoints = points(selected);
            CASES.add("{\"id\":" + string("plot/" + series.id()) + ",\"kind\":\"plot\",\"series\":" + string(series.id())
                    + ",\"rows\":null,\"stored_columns\":"
                    + (stored ? "null" : "{\"x\":" + numbers(x.getYPoints()) + ",\"y\":" + numbers(y.getYPoints()) + "}")
                    + ",\"working\":" + workingPoints + ",\"selected\":" + (selectedPoints.equals(workingPoints) ? "null" : selectedPoints)
                    + "}");
        }
        for (int[] rows : new int[][] {{1, 2, 3, 5, 8}, {0, 4, 9, 11, 12}}) {
            Series series = SERIES.stream().filter(s -> s.id().equals("y-gaps")).findFirst().orElseThrow();
            HighlightableDataset selected = selected(dataColumn(series.x()), dataColumn(series.y()), rows);
            CASES.add("{\"id\":" + string("plot/y-gaps/rows=" + joined(rows)) + ",\"kind\":\"plot\",\"series\":\"y-gaps\""
                    + ",\"rows\":" + integers(rows) + ",\"selected\":" + points(selected) + "}");
        }
    }

    private static void correlations() {
        for (Series series : SERIES) {
            if (series.x().length > 30) continue;
            HighlightableDataset selected = selected(dataColumn(series.x()), dataColumn(series.y()), null);
            DatasetCurveFitter fitter;
            try {
                fitter = (DatasetCurveFitter) UNSAFE.allocateInstance(DatasetCurveFitter.class);
                fitter.doLinearRegression(selected.getValidXPoints(), selected.getValidYPoints());
                double correlation = (double) get(fitter, "correlation");
                CASES.add("{\"id\":" + string("correlation/" + series.id()) + ",\"kind\":\"correlation\",\"series\":"
                        + string(series.id()) + ",\"correlation\":" + number(correlation) + "}");
            } catch (ReflectiveOperationException e) {
                throw new IllegalStateException(e);
            }
        }
    }

    private static Series series(String id) {
        return SERIES.stream().filter(s -> s.id().equals(id)).findFirst().orElseThrow();
    }

    private static HighlightableDataset workingFor(Harness harness, Series series) throws ReflectiveOperationException {
        return harness.working(dataColumn(series.x()), dataColumn(series.y()));
    }

    private static void areas(Harness harness) throws ReflectiveOperationException {
        harness.dataMode();
        set(harness.tab, "areaVisible", true);
        Object[][] specs = {
            {"parabola", 0.0, 2.0}, {"parabola", 2.0, 0.0}, {"parabola", 0.35, 1.25}, {"parabola", 0.3, 1.2},
            {"parabola", 0.31, 0.39}, {"parabola", -5.0, 5.0}, {"parabola", 0.5, 0.5}, {"line", 0.0, 2.0},
            {"line", 1.0, 0.2}, {"uneven-sine", 0.0, 3.0}, {"uneven-sine", 0.5, 2.0}, {"y-gaps", 0.0, 5.5},
            {"y-gaps", 1.5, 4.0}, {"x-gaps", 0.0, 10.0}, {"infinite-y", 0.0, 5.0}, {"path", -1.0, 1.0},
            {"repeated-x", 0.0, 3.0}, {"empty", 0.0, 1.0}, {"single", 0.0, 4.0}, {"two", 1.0, 3.0}, {"all-gaps", 0.0, 2.0},
            {"line", NAN, 1.0}};
        for (Object[] spec : specs) {
            Series series = series((String) spec[0]);
            workingFor(harness, series);
            double first = (double) spec[1];
            double second = (double) spec[2];
            set(harness.limits[0], "x", first);
            set(harness.limits[1], "x", second);
            Dataset area = new Dataset();
            set(harness.plot, "areaDataset", area);
            invoke(harness.refreshArea, harness.plot);
            CASES.add("{\"id\":" + string("area/" + series.id() + "/" + first + "," + second) + ",\"kind\":\"area\",\"series\":"
                    + string(series.id()) + ",\"limits\":" + numbers(new double[] {first, second}) + ",\"area\":"
                    + number((double) get(harness.plot, "area")) + ",\"points\":" + (area.getIndex() == 0 ? 0 : area.getIndex() - 2)
                    + ",\"true_limits\":" + numbers(new double[] {(double) get(harness.limits[0], "trueLimit"),
                        (double) get(harness.limits[1], "trueLimit")}) + "}");
        }
    }

    private static void measurements(Harness harness) throws ReflectiveOperationException {
        harness.dataMode();
        set(harness.tab, "areaVisible", false);
        set(harness.tab, "positionVisible", true);
        set(harness.tab, "slopeVisible", true);
        Object crossbars = get(harness.plot, "valueCrossbars");
        Object slopeLine = get(harness.plot, "slopeLine");
        for (String id : new String[] {"parabola", "line", "uneven-sine", "y-gaps", "x-gaps", "path", "repeated-x", "single", "two"}) {
            Series series = series(id);
            HighlightableDataset working = workingFor(harness, series);
            for (int index = 0; index < working.getIndex(); index++) {
                set(harness.plot, "value", NAN);
                set(harness.plot, "slope", NAN);
                set(crossbars, "x", NAN);
                set(crossbars, "y", NAN);
                set(slopeLine, "x", NAN);
                set(slopeLine, "y", NAN);
                set(harness.plot, "measurementIndex", index);
                invoke(harness.refreshMeasurements, harness.plot);
                // The crossbars move only for a point with a y value; the value then equals their y.
                if (!Double.valueOf((double) get(harness.plot, "value")).equals((double) get(crossbars, "y"))) {
                    throw new IllegalStateException("Unexpected crossbar value for " + id + "/" + index);
                }
                CASES.add("{\"id\":" + string("measure/" + id + "/" + index) + ",\"kind\":\"measure\",\"series\":" + string(id)
                        + ",\"index\":" + index
                        + ",\"coordinates\":" + numbers(new double[] {(double) get(crossbars, "x"), (double) get(crossbars, "y")})
                        + ",\"slope\":" + number((double) get(harness.plot, "slope")) + "}");
            }
        }
        set(harness.tab, "positionVisible", false);
        set(harness.tab, "slopeVisible", false);
    }

    private static void nearest(Harness harness) throws ReflectiveOperationException {
        Object[][] specs = {
            {"parabola", 0.73, 0.0, 2.0}, {"parabola", 0.75, 0.0, 2.0}, {"parabola", -1.0, 0.0, 2.0}, {"parabola", 3.0, 0.0, 2.0},
            {"parabola", 0.73, 0.76, 2.0}, {"parabola", 1.9, 0.0, 1.62}, {"y-gaps", 2.4, 0.0, 6.0}, {"y-gaps", 4.6, 0.0, 6.0},
            {"y-gaps", 5.4, 0.0, 6.0}, {"repeated-x", 1.2, -1.0, 4.0}, {"repeated-x", 3.0, -1.0, 2.5}, {"path", 0.1, -1.0, 1.0},
            {"path", 0.95, -0.5, 0.9}, {"uneven-sine", 1.0, 0.0, 3.0}, {"single", 0.0, -1.0, 1.0}, {"all-gaps", 1.0, 0.0, 2.0},
            {"empty", 0.0, 0.0, 1.0}, {"line", NAN, 0.0, 2.0}};
        for (Object[] spec : specs) {
            Series series = series((String) spec[0]);
            HighlightableDataset working = workingFor(harness, series);
            double x = (double) spec[1];
            double min = (double) spec[2];
            double max = (double) spec[3];
            String result;
            try {
                result = Integer.toString(DataToolTab.findNearestXIndex(x, working.getXPointsRaw(), working.getYPointsRaw(),
                        working.getIndex(), min, max));
            } catch (RuntimeException e) {
                result = string(e.getClass().getSimpleName());
            }
            CASES.add("{\"id\":" + string("nearest/" + series.id() + "/" + x + "/" + min + "," + max) + ",\"kind\":\"nearest\""
                    + ",\"series\":" + string(series.id()) + ",\"x\":" + number(x) + ",\"visible\":" + numbers(new double[] {min, max})
                    + ",\"index\":" + result + "}");
        }
    }

    private static void fits(Harness harness) throws ReflectiveOperationException {
        double[][] polynomials = {{1.0, 2.0, -0.5, 0.25}, {0.0, 0.0, 3.0}, {4.0}};
        Object[][] areaSpecs = {{0, 0.0, 2.0, 200.0, 37.0}, {0, 2.0, -1.0, 157.3, 300.0}, {1, 0.0, 1.0, 512.0, 0.0},
            {1, 0.25, 0.75, 96.5, 11.0}, {2, -3.0, 5.0, 40.0, 200.0}, {0, 1.0, 1.0, 100.0, 0.0}};
        set(harness.tab, "areaVisible", true);
        set(harness.tab, "positionVisible", false);
        set(harness.tab, "slopeVisible", false);
        workingFor(harness, series("line"));
        for (Object[] spec : areaSpecs) {
            double[] coefficients = polynomials[(int) spec[0]];
            double first = (double) spec[1];
            double second = (double) spec[2];
            harness.fitMode(coefficients, (double) spec[3], (double) spec[4]);
            set(harness.limits[0], "x", first);
            set(harness.limits[1], "x", second);
            Dataset area = new Dataset();
            set(harness.plot, "areaDataset", area);
            invoke(harness.refreshArea, harness.plot);
            int samples = area.getIndex() == 0 ? 0 : area.getIndex() - 2;
            double[] sampleX = area.getXPointsRaw();
            CASES.add("{\"id\":" + string("function-area/" + spec[0] + "/" + first + "," + second + "/" + spec[3])
                    + ",\"kind\":\"function-area\",\"coefficients\":" + numbers(coefficients) + ",\"limits\":"
                    + numbers(new double[] {first, second}) + ",\"pixels_per_unit\":" + number((double) spec[3])
                    + ",\"pixel_origin\":" + number((double) spec[4]) + ",\"samples\":" + samples + ",\"first_sample\":"
                    + number(samples > 0 ? sampleX[1] : NAN) + ",\"last_sample\":" + number(samples > 0 ? sampleX[samples] : NAN)
                    + ",\"area\":" + number((double) get(harness.plot, "area")) + "}");
        }
        set(harness.tab, "areaVisible", false);
        set(harness.tab, "positionVisible", true);
        set(harness.tab, "slopeVisible", true);
        Object[][] slopeSpecs = {{0, 150, 100.0, 50.0}, {0, 3, 7.5, 0.0}, {1, 400, 512.0, 64.0}, {2, 10, 20.0, 0.0}};
        for (Object[] spec : slopeSpecs) {
            double[] coefficients = polynomials[(int) spec[0]];
            harness.fitMode(coefficients, (double) spec[2], (double) spec[3]);
            set(harness.plot, "measurementIndex", 0);
            set(harness.plot, "measurementX", (int) spec[1]);
            set(harness.plot, "slope", NAN);
            set(harness.plot, "value", NAN);
            Object slopeLine = get(harness.plot, "slopeLine");
            set(slopeLine, "x", NAN);
            invoke(harness.refreshMeasurements, harness.plot);
            CASES.add("{\"id\":" + string("function-slope/" + spec[0] + "/" + spec[1] + "/" + spec[2]) + ",\"kind\":\"function-slope\""
                    + ",\"coefficients\":" + numbers(coefficients) + ",\"x\":" + number((double) get(slopeLine, "x")) + ",\"dx\":"
                    + number(1 / (double) spec[2]) + ",\"value\":" + number((double) get(harness.plot, "value")) + ",\"slope\":"
                    + number((double) get(harness.plot, "slope")) + "}");
        }
        harness.dataMode();
        set(harness.tab, "positionVisible", false);
        set(harness.tab, "slopeVisible", false);
    }

    private static void spectra() {
        for (Series series : SERIES) {
            if (series.x().length > 1 && series.x().length < 8 && !series.id().endsWith("-points")) continue;
            HighlightableDataset selected = selected(dataColumn(series.x()), dataColumn(series.y()), null);
            int before = selected.getIndex();
            FourierSinCosAnalysis analysis = FourierPanel.createFourierData(selected);
            StringBuilder json = new StringBuilder("{\"id\":" + string("fourier/" + series.id()) + ",\"kind\":\"fourier\",\"series\":"
                    + string(series.id()) + ",\"input_points\":" + before);
            if (analysis == null) {
                CASES.add(json.append(",\"spectrum\":null}").toString());
                continue;
            }
            List<Dataset> datasets = analysis.getDatasets();
            Dataset power = datasets.get(0);
            Dataset cosine = datasets.get(1);
            Dataset sine = datasets.get(2);
            json.append(",\"transformed_points\":").append(selected.getIndex());
            json.append(",\"spectrum\":{\"rows\":").append(power.getIndex());
            json.append(",\"frequency\":").append(numbers(java.util.Arrays.copyOf(power.getXPointsRaw(), power.getIndex())));
            json.append(",\"omega\":").append(numbers(analysis.getNaturalOmega()));
            json.append(",\"power\":").append(numbers(java.util.Arrays.copyOf(power.getYPointsRaw(), power.getIndex())));
            json.append(",\"cosine\":").append(numbers(java.util.Arrays.copyOf(cosine.getYPointsRaw(), cosine.getIndex())));
            json.append(",\"sine\":").append(numbers(java.util.Arrays.copyOf(sine.getYPointsRaw(), sine.getIndex())));
            CASES.add(json.append("}}").toString());
        }
    }

    private static void transforms() {
        Lcg random = new Lcg(97);
        for (int n = 2; n <= 40; n += 2) realTransform(random, n);
        for (int n : new int[] {64, 100, 128}) realTransform(random, n);
        for (int n = 1; n <= 16; n++) complexTransform(random, n);
        for (int n : new int[] {22, 25, 27, 49, 60, 64, 77, 121, 128}) complexTransform(random, n);
    }

    private static double[] signed(Lcg random, int n) {
        double[] values = new double[n];
        for (int i = 0; i < n; i++) values[i] = 2.0 * random.next() - 1.0;
        return values;
    }

    private static void realTransform(Lcg random, int n) {
        double[] input = signed(random, n);
        double[] output = new FFTReal().transform(input.clone());
        CASES.add("{\"id\":" + string("fft-real/" + n) + ",\"kind\":\"fft-real\",\"input\":" + numbers(input) + ",\"output\":"
                + numbers(output) + "}");
    }

    private static void complexTransform(Lcg random, int n) {
        double[] input = signed(random, 2 * n);
        double[] output = new FFT().transform(input.clone());
        CASES.add("{\"id\":" + string("fft-complex/" + n) + ",\"kind\":\"fft-complex\",\"input\":" + numbers(input) + ",\"output\":"
                + numbers(output) + "}");
    }

    /** What DataColumn stores for a column with missing and infinite values (Dataset.append). */
    private static void storedColumns() {
        double[][] inputs = {{1, 2, INF, 4, 5}, {1, NAN, 3, -INF, 5, 6}, {INF, 2, 3}};
        for (int i = 0; i < inputs.length; i++) {
            DataColumn column = dataColumn(inputs[i]);
            CASES.add("{\"id\":" + string("column/" + i) + ",\"kind\":\"column\",\"input\":" + numbers(inputs[i]) + ",\"rows\":"
                    + numbers(java.util.Arrays.copyOf(column.getXPointsRaw(), column.getIndex())) + ",\"values\":"
                    + numbers(column.getYPoints()) + "}");
        }
    }

    public static void main(String[] args) {
        try {
            if (args.length != 1) throw new IllegalArgumentException("Usage: AnalysisOracle output.json");
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            columns();
            series();
            Harness harness = new Harness();
            statistics(harness);
            plots(harness);
            correlations();
            areas(harness);
            measurements(harness);
            nearest(harness);
            fits(harness);
            spectra();
            transforms();
            storedColumns();
            StringBuilder json = new StringBuilder();
            json.append("{\"schema_version\":1,\"reference_version\":").append(string(OSPRuntime.VERSION));
            json.append(",\"osp_commit\":\"d56207b0ae737458a31365d368f1c28e4bd3202a\"");
            json.append(",\"columns\":[");
            for (int i = 0; i < COLUMNS.size(); i++) {
                Column column = COLUMNS.get(i);
                json.append(i > 0 ? "," : "").append("{\"id\":").append(string(column.id())).append(",\"description\":")
                        .append(string(column.description())).append(",\"values\":").append(numbers(column.values())).append('}');
            }
            json.append("],\"series\":[");
            for (int i = 0; i < SERIES.size(); i++) {
                Series series = SERIES.get(i);
                json.append(i > 0 ? "," : "").append("{\"id\":").append(string(series.id())).append(",\"description\":")
                        .append(string(series.description())).append(",\"x\":").append(numbers(series.x())).append(",\"y\":")
                        .append(numbers(series.y())).append('}');
            }
            json.append("],\"cases\":[");
            for (int i = 0; i < CASES.size(); i++) json.append(i > 0 ? "," : "").append(CASES.get(i));
            json.append("]}\n");
            Files.writeString(Path.of(args[0]), json, StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable e) {
            e.printStackTrace();
            System.exit(1);
        }
    }

    // ------------------------------------------------------------------------------------------
    // JSON.

    private static String joined(int[] values) {
        StringBuilder text = new StringBuilder();
        for (int i = 0; i < values.length; i++) text.append(i > 0 ? "," : "").append(values[i]);
        return text.toString();
    }

    private static String integers(int[] values) {
        return values == null ? "null" : "[" + joined(values) + "]";
    }

    private static String numbers(double[] values) {
        StringBuilder json = new StringBuilder("[");
        for (int i = 0; i < values.length; i++) json.append(i > 0 ? "," : "").append(number(values[i]));
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

    private static String nullableString(String value) {
        return value == null ? "null" : string(value);
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
