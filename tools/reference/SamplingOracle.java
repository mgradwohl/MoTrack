import java.awt.Rectangle;
import java.awt.Shape;
import java.awt.geom.AffineTransform;
import java.awt.image.BufferedImage;
import java.io.ByteArrayOutputStream;
import java.io.PrintStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.math.BigDecimal;
import java.math.MathContext;
import java.math.RoundingMode;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.opensourcephysics.cabrillo.tracker.LineProfile;
import org.opensourcephysics.cabrillo.tracker.LineProfileStep;
import org.opensourcephysics.cabrillo.tracker.RGBRegion;
import org.opensourcephysics.cabrillo.tracker.RGBStep;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.TPoint;
import org.opensourcephysics.media.core.Video;

/** Measures synthetic frames with the actual RGBStep and LineProfileStep code of the pinned Tracker 6.3.5 JAR.
 *
 * Frames are generated from integer formulas (and a SplitMix64 stream for noise) that
 * tests/science/sampling_reference_test.cpp repeats exactly; each frame's FNV-1a hash is recorded so
 * that the C++ test proves it measured the same pixels. Steps are created as Tracker creates them and
 * measured through their public getRGBData/getProfileData methods. The panel is a TrackerPanel
 * subclass allocated without its GUI constructor that reports a fixed frame, a synthetic Video
 * whose getImage() returns the frame, and a coordinate system whose x-axis angle selects the
 * profile algorithm while its image-to-world transform is the identity, so positions stay in image
 * pixels. See docs/image-sampling.md for the case matrix, the tolerances and the reference defects.
 */
public final class SamplingOracle {
    private static final int SHAPE_ELLIPSE = 0;
    private static final int SHAPE_RECTANGLE = 1;
    private static final int SHAPE_POLYGON = 2;
    /** Any x-axis angle with |sin| >= 1e-5 selects the tilted profile algorithm. */
    private static final double TILTED_AXIS_ANGLE = 0.5;

    /** The panel API that RGBStep.getRGBData and LineProfileStep.getProfileData use, without a GUI. */
    static final class SyntheticPanel extends TrackerPanel {
        Video syntheticVideo;
        ImageCoordSystem syntheticCoords;
        double syntheticImageWidth;

        SyntheticPanel() {
            super(true); // never runs: instances come from Unsafe.allocateInstance
        }

        @Override
        public Video getVideo() {
            return syntheticVideo;
        }

        @Override
        public int getFrameNumber() {
            return 0;
        }

        @Override
        public ImageCoordSystem getCoords() {
            return syntheticCoords;
        }

        @Override
        public double getImageWidth() {
            return syntheticImageWidth;
        }
    }

    /** Reports an x-axis angle for the profile switch but keeps world coordinates equal to image pixels. */
    static final class AxisAngle extends ImageCoordSystem {
        private final double angle;

        AxisAngle(double angle) {
            super(1);
            this.angle = angle;
        }

        @Override
        public double getAngle(int n) {
            return angle;
        }

        @Override
        public AffineTransform getToWorldTransform(int n) {
            return new AffineTransform();
        }
    }

    private record FrameSpec(String id, String kind, int width, int height, long seed) {}

    private record RegionSpec(String id, String frame, int shape, int width, int height, double x, double y, double[][] vertices) {}

    private record ProfileSpec(String id, String frame, boolean alwaysHorizontal, double axisAngle, double x0, double y0, double x1,
                               double y1, int spread) {}

    private static final List<FrameSpec> FRAMES = List.of(
        new FrameSpec("noise", "noise", 96, 72, 0x5EED0001L),
        new FrameSpec("gradient", "gradient", 128, 96, 0),
        new FrameSpec("checker", "checker", 80, 60, 0),
        new FrameSpec("edge", "edge", 64, 48, 0),
        new FrameSpec("vedge", "vedge", 64, 48, 0),
        new FrameSpec("band", "band", 64, 48, 0),
        new FrameSpec("white", "white", 260, 260, 0),
        new FrameSpec("bright", "bright", 260, 260, 0x5EED0002L),
        new FrameSpec("sparse", "sparse", 1008, 1008, 0x5EED0003L));

    /** Pixel formulas shared with the C++ test; every value is 0x00RRGGBB as Tracker's TYPE_INT_RGB raster holds it. */
    static int[] pixels(FrameSpec frame) {
        int[] pixels = new int[frame.width() * frame.height()];
        long[] state = {frame.seed()};
        for (int y = 0; y < frame.height(); y++) {
            for (int x = 0; x < frame.width(); x++) {
                int r, g, b;
                switch (frame.kind()) {
                    case "noise" -> {
                        int value = (int) (next(state) >>> 40);
                        r = value >>> 16 & 0xff;
                        g = value >>> 8 & 0xff;
                        b = value & 0xff;
                    }
                    case "bright" -> {
                        long value = next(state);
                        r = 224 + (int) (value >>> 40 & 31);
                        g = 224 + (int) (value >>> 45 & 31);
                        b = 224 + (int) (value >>> 50 & 31);
                    }
                    case "sparse" -> {
                        int value = Long.remainderUnsigned(next(state), 1000) < 92 ? 255 : 0;
                        r = g = b = value;
                    }
                    case "gradient" -> {
                        r = x & 0xff;
                        g = y & 0xff;
                        b = (x + y) & 0xff;
                    }
                    case "checker" -> {
                        boolean on = ((x / 4 + y / 4) & 1) == 0;
                        r = on ? 230 : 10;
                        g = on ? 20 : 200;
                        b = on ? 120 : 60;
                    }
                    case "edge" -> {
                        // anti-aliased edges at x = 20.3 and y = 30.6 (coverage in tenths)
                        r = 255 * Math.max(0, Math.min(10, 203 - 10 * x)) / 10;
                        g = 255 * Math.max(0, Math.min(10, 306 - 10 * y)) / 10;
                        b = 90;
                    }
                    case "vedge" -> {
                        boolean left = x < 32;
                        r = left ? 200 : 20;
                        g = left ? 100 : 40;
                        b = left ? 50 : 240;
                    }
                    case "band" -> {
                        boolean black = y < 24;
                        r = black ? 0 : x & 0xff;
                        g = black ? 0 : y & 0xff;
                        b = black ? 0 : 50;
                    }
                    case "white" -> r = g = b = 255;
                    default -> throw new IllegalArgumentException(frame.kind());
                }
                pixels[x + y * frame.width()] = r << 16 | g << 8 | b;
            }
        }
        return pixels;
    }

    /** SplitMix64 (Steele, Lea and Flood), identical in Java's wrapping long arithmetic and C++ uint64_t. */
    static long next(long[] state) {
        long z = state[0] += 0x9E3779B97F4A7C15L;
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }

    /** FNV-1a 64 over each 0x00RRGGBB value as four little-endian bytes, row-major. */
    static long hash(int[] values) {
        long hash = 0xcbf29ce484222325L;
        for (int value : values) {
            for (int shift = 0; shift < 32; shift += 8) {
                hash ^= value >>> shift & 0xff;
                hash *= 0x100000001b3L;
            }
        }
        return hash;
    }

    private static double[][] v(double... xy) {
        double[][] vertices = new double[xy.length / 2][];
        for (int i = 0; i < vertices.length; i++) vertices[i] = new double[] {xy[2 * i], xy[2 * i + 1]};
        return vertices;
    }

    private static RegionSpec ellipse(String id, String frame, int w, int h, double x, double y) {
        return new RegionSpec(id, frame, SHAPE_ELLIPSE, w, h, x, y, null);
    }

    private static RegionSpec rectangle(String id, String frame, int w, int h, double x, double y) {
        return new RegionSpec(id, frame, SHAPE_RECTANGLE, w, h, x, y, null);
    }

    private static RegionSpec polygon(String id, String frame, double x, double y, double[][] vertices) {
        // Tracker creates polygon steps with the default 20 x 20 size, which polygons ignore.
        return new RegionSpec(id, frame, SHAPE_POLYGON, 20, 20, x, y, vertices);
    }

    private static final double[][] TRIANGLE = v(0, 0, 15, 0, 0, 12);
    private static final double[][] PENTAGRAM = v(0, 0, 8.8168, 27.1353, -14.2658, 10.3647, 14.2658, 10.3647, -8.8168, 27.1353);

    private static final List<RegionSpec> REGIONS = List.of(
        // Ellipses: -size/2 uses integer division, so odd sizes extend half a pixel further right/down.
        ellipse("ellipse-20-integer", "noise", 20, 20, 48, 36),
        ellipse("ellipse-20-half", "noise", 20, 20, 48.5, 36.5),
        ellipse("ellipse-21-subpixel", "noise", 21, 21, 48.3, 36.7),
        ellipse("ellipse-7x13-subpixel", "noise", 7, 13, 30.25, 40.75),
        ellipse("ellipse-1-integer", "noise", 1, 1, 10, 10),
        ellipse("ellipse-1-subpixel", "noise", 1, 1, 10.2, 10.9),
        ellipse("ellipse-2-integer", "noise", 2, 2, 10, 10),
        ellipse("ellipse-3-half", "noise", 3, 3, 10.5, 10.5),
        ellipse("ellipse-9x5-near-integer", "noise", 9, 5, 20.001, 50.999),
        ellipse("ellipse-3x8-subpixel", "noise", 3, 8, 70.75, 8.5),
        ellipse("ellipse-zero-width", "noise", 0, 20, 40, 30),
        ellipse("ellipse-zero-height", "noise", 20, 0, 40, 30),
        ellipse("ellipse-negative-width", "noise", -4, 6, 40, 30),
        ellipse("ellipse-touching-left", "noise", 5, 5, 2.0, 30),
        ellipse("ellipse-bounds-left-outside", "noise", 5, 5, 1.9, 30),
        ellipse("ellipse-touching-right", "noise", 5, 5, 93.0, 30),
        ellipse("ellipse-bounds-right-outside", "noise", 5, 5, 93.5, 30),
        ellipse("ellipse-60x50", "noise", 60, 50, 48, 36),
        ellipse("ellipse-30-gradient", "gradient", 30, 30, 64, 48),
        ellipse("ellipse-checker", "checker", 23, 17, 40.4, 30.2),
        // Rectangles: half-open [left, right) x [top, bottom) by the pixel-centre rule.
        rectangle("rectangle-20-integer", "noise", 20, 20, 48, 36),
        rectangle("rectangle-20-half", "noise", 20, 20, 48.5, 36.5),
        rectangle("rectangle-21-subpixel", "noise", 21, 21, 48.3, 36.7),
        rectangle("rectangle-7x13-subpixel", "noise", 7, 13, 30.25, 40.75),
        rectangle("rectangle-1-integer", "noise", 1, 1, 10, 10),
        rectangle("rectangle-1-half", "noise", 1, 1, 10.5, 10.5),
        rectangle("rectangle-1-subpixel", "noise", 1, 1, 10.6, 10.4),
        rectangle("rectangle-2x3-half", "noise", 2, 3, 10.5, 20.5),
        rectangle("rectangle-empty", "noise", 0, 0, 40, 30),
        rectangle("rectangle-full-frame", "noise", 96, 72, 48, 36),
        rectangle("rectangle-full-frame-shifted", "noise", 96, 72, 48.5, 36),
        rectangle("rectangle-95x71", "noise", 95, 71, 48, 36),
        rectangle("rectangle-gradient", "gradient", 20, 10, 40.3, 30.6),
        rectangle("rectangle-checker", "checker", 16, 16, 40, 30),
        rectangle("rectangle-edge", "edge", 10, 10, 20.3, 30.6),
        // Polygons: vertices relative to the step position, non-zero winding, implicitly closed.
        polygon("polygon-triangle-open", "noise", 48, 36, TRIANGLE),
        polygon("polygon-triangle-closed", "noise", 48, 36, v(0, 0, 15, 0, 0, 12, 0, 0)),
        polygon("polygon-concave", "noise", 40, 30, v(0, 0, 20, 0, 20, 20, 10, 8, 0, 20)),
        polygon("polygon-bowtie", "noise", 40, 30, v(0, 0, 20, 12, 20, 0, 0, 12)),
        polygon("polygon-pentagram", "noise", 48, 30, PENTAGRAM),
        polygon("polygon-collinear", "noise", 48, 36, v(0, 0, 10, 10, 20, 20)),
        polygon("polygon-single-vertex", "noise", 48, 36, v(0, 0)),
        polygon("polygon-two-vertices", "noise", 48, 36, v(0, 0, 10, 5)),
        polygon("polygon-subpixel", "noise", 40.3, 30.7, v(0, 0, 12.25, -3.5, 17.75, 9.125, 3.5, 14.875)),
        polygon("polygon-bounds-outside", "noise", 90, 36, TRIANGLE),
        polygon("polygon-offset-first-vertex", "noise", 40, 30, v(5, 5, 15, 5, 10, 15)),
        polygon("polygon-repeated-vertex", "noise", 40, 30, v(0, 0, 10, 0, 10, 0, 10, 10, 0, 10)),
        polygon("polygon-double-wound", "noise", 40, 30, v(0, 0, 10, 0, 10, 10, 0, 10, 0, 0, 10, 0, 10, 10, 0, 10)),
        polygon("polygon-gradient", "gradient", 30.5, 20.5, v(0, 0, 24, 0, 24, 18)),
        // Sums of squares are Java ints: more than 33025 bright pixels overflow the deviations.
        rectangle("overflow-white-181", "white", 181, 181, 130.5, 130.5),
        rectangle("overflow-white-182", "white", 182, 182, 130, 130),
        rectangle("overflow-white-200", "white", 200, 200, 130, 130),
        ellipse("overflow-white-ellipse-210", "white", 210, 210, 130, 130),
        rectangle("overflow-bright-200", "bright", 200, 200, 130, 130),
        rectangle("overflow-sparse-1000", "sparse", 1000, 1000, 504, 504));

    private static ProfileSpec horizontal(String id, String frame, double x0, double y0, double x1, double y1, int spread) {
        return new ProfileSpec(id, frame, true, 0, x0, y0, x1, y1, spread);
    }

    private static ProfileSpec tilted(String id, String frame, double x0, double y0, double x1, double y1, int spread) {
        return new ProfileSpec(id, frame, false, TILTED_AXIS_ANGLE, x0, y0, x1, y1, spread);
    }

    private static final List<ProfileSpec> PROFILES = List.of(
        horizontal("horizontal-gradient", "gradient", 10, 20, 30, 20, 0),
        horizontal("horizontal-gradient-subpixel", "gradient", 10.7, 20.4, 30.2, 20.4, 1),
        horizontal("horizontal-reversed", "noise", 30.2, 20.4, 10.7, 20.4, 2),
        horizontal("horizontal-left-clamp", "noise", -5.3, 10.5, 15.2, 10.5, 0),
        horizontal("horizontal-right-clamp", "gradient", 110.5, 50.2, 140, 50.2, 3),
        horizontal("horizontal-spread-above-image", "noise", 5.5, 1.5, 25.5, 1.5, 3),
        horizontal("horizontal-bottom-row", "noise", 5.5, 71.5, 25.5, 71.5, 0),
        horizontal("horizontal-spread-below-image", "noise", 5.5, 71.5, 25.5, 71.5, 1),
        horizontal("horizontal-negative-y-truncates", "noise", 5.5, -0.5, 25.5, -0.5, 0),
        horizontal("horizontal-negative-x-truncates", "noise", -0.9, 40, 8, 40, 0),
        horizontal("horizontal-empty", "noise", 10.2, 5, 10.9, 5, 0),
        horizontal("horizontal-left-of-image", "noise", -20, 30, -5, 30, 0),
        horizontal("horizontal-right-partial", "noise", 90, 30, 120, 30, 0),
        horizontal("horizontal-sloped-uses-end0", "noise", 10, 30, 20, 34, 0),
        horizontal("horizontal-full-width", "edge", -3.5, 26, 70.5, 26, 3),
        horizontal("horizontal-checker", "checker", 3.3, 12.8, 41.9, 12.8, 4),
        horizontal("horizontal-all-black", "band", 5, 10, 40, 10, 2),
        horizontal("horizontal-band-edge", "band", 5, 22, 40, 22, 2),
        new ProfileSpec("dispatch-axis-zero", "gradient", false, 0, 10, 20, 30, 20, 0),
        new ProfileSpec("dispatch-axis-tiny", "gradient", false, 5e-6, 10, 20, 30, 20, 0),
        new ProfileSpec("dispatch-axis-pi", "gradient", false, Math.PI, 10, 20, 30, 20, 0),
        new ProfileSpec("dispatch-axis-small", "noise", false, 2e-5, 20.37, 20.61, 47.91, 29.12, 0),
        // Tilted probes, by direction quadrant of end0 -> end1 in image coordinates (y down).
        tilted("tilted-q2-s0", "noise", 20.37, 20.61, 47.91, 29.12, 0),
        tilted("tilted-q2-s1", "noise", 20.37, 20.61, 47.91, 29.12, 1),
        tilted("tilted-q2-s3", "noise", 20.37, 20.61, 47.91, 29.12, 3),
        tilted("tilted-q2-steep", "noise", 10.43, 10.18, 30.71, 45.66, 2),
        tilted("tilted-q0-s1", "noise", 47.91, 42.1, 20.37, 33.59, 1),
        tilted("tilted-q0-checker", "checker", 60.4, 50.3, 20.2, 12.5, 2),
        tilted("tilted-q1-s1", "noise", 20.37, 50.61, 47.91, 42.1, 1),
        tilted("tilted-q3-s1", "noise", 47.91, 29.12, 20.37, 37.63, 1),
        tilted("tilted-q2-left-edge", "noise", -5.37, 20.25, 30.75, 35.5, 1),
        tilted("tilted-q2-far-edge", "noise", 70.31, 50.27, 110.25, 75, 1),
        tilted("tilted-q2-wide", "noise", 10.33, 30.21, 70.12, 41.37, 20),
        tilted("tilted-outside", "noise", -10.3, -10.6, -2.2, -3.1, 0),
        tilted("tilted-short", "noise", 10.3, 10.2, 10.9, 10.7, 0),
        tilted("tilted-vertical-integer", "noise", 40, 10, 40, 40, 1),
        tilted("tilted-vertical-half", "noise", 40.5, 10.25, 40.5, 40.25, 0),
        tilted("tilted-diagonal-integer", "noise", 10, 10, 40, 40, 1),
        tilted("tilted-integer-start", "noise", 30, 20, 58.7, 28.9, 1),
        tilted("tilted-horizontal-line", "noise", 10.25, 30.5, 50.75, 30.5, 1),
        tilted("tilted-q2-vedge", "vedge", 20.13, 10.29, 44.07, 30.18, 2),
        tilted("tilted-q1-vedge", "vedge", 20.13, 30.18, 44.07, 10.29, 2),
        tilted("tilted-q2-gradient", "gradient", 15.37, 30.52, 55.81, 60.07, 1),
        tilted("tilted-q1-gradient", "gradient", 15.37, 60.07, 55.81, 30.52, 1),
        tilted("tilted-q1-uniform", "white", 20.5, 60.25, 60.75, 30.5, 5));

    public static void main(String[] args) {
        try {
            if (args.length != 1) throw new IllegalArgumentException("Usage: SamplingOracle output.json");
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            Map<String, BufferedImage> images = new LinkedHashMap<>();
            StringBuilder json = new StringBuilder();
            json.append("{\"schema_version\":1,\"reference_version\":").append(string(OSPRuntime.VERSION));
            json.append(",\"tracker\":").append(trackerConstants());
            json.append(",\"frames\":[");
            for (int i = 0; i < FRAMES.size(); i++) {
                FrameSpec frame = FRAMES.get(i);
                int[] values = pixels(frame);
                BufferedImage image = new BufferedImage(frame.width(), frame.height(), BufferedImage.TYPE_INT_RGB);
                image.getRaster().setDataElements(0, 0, frame.width(), frame.height(), values);
                images.put(frame.id(), image);
                if (i > 0) json.append(',');
                json.append("{\"id\":").append(string(frame.id())).append(",\"kind\":").append(string(frame.kind()))
                    .append(",\"width\":").append(frame.width()).append(",\"height\":").append(frame.height())
                    .append(",\"seed\":").append(string(Long.toHexString(frame.seed())))
                    .append(",\"fnv1a64\":").append(string(String.format("%016x", hash(values)))).append('}');
            }
            json.append("],\"regions\":[");
            for (int i = 0; i < REGIONS.size(); i++) {
                if (i > 0) json.append(',');
                json.append(region(REGIONS.get(i), images));
            }
            json.append("],\"profiles\":[");
            for (int i = 0; i < PROFILES.size(); i++) {
                if (i > 0) json.append(',');
                json.append(profile(PROFILES.get(i), images));
            }
            json.append("]}\n");
            Files.writeString(Path.of(args[0]), json, StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }

    /** Tracker's own limits, read from the pinned classes. */
    private static String trackerConstants() throws ReflectiveOperationException {
        return "{\"rgb_default_edge_length\":" + staticInt(RGBRegion.class, "defaultEdgeLength")
            + ",\"rgb_default_max_edge_length\":" + staticInt(RGBRegion.class, "defaultMaxEdgeLength")
            + ",\"line_profile_max_spread\":" + LineProfile.MAX_SPREAD
            + ",\"luma\":[" + number(RGBRegion.getLuma(1, 0, 0)) + ',' + number(RGBRegion.getLuma(0, 1, 0)) + ','
            + number(RGBRegion.getLuma(0, 0, 1)) + "]}";
    }

    private static int staticInt(Class<?> type, String name) throws ReflectiveOperationException {
        Field field = type.getDeclaredField(name);
        field.setAccessible(true);
        return field.getInt(null);
    }

    private static void setField(Class<?> type, Object target, String name, Object value) throws ReflectiveOperationException {
        Field field = type.getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    private static SyntheticPanel panel(BufferedImage image, double axisAngle) throws ReflectiveOperationException {
        Field field = Class.forName("sun.misc.Unsafe").getDeclaredField("theUnsafe");
        field.setAccessible(true);
        Object unsafe = field.get(null);
        Method allocate = unsafe.getClass().getMethod("allocateInstance", Class.class);
        SyntheticPanel panel = (SyntheticPanel) allocate.invoke(unsafe, SyntheticPanel.class);
        panel.syntheticVideo = video(image);
        panel.syntheticCoords = new AxisAngle(axisAngle);
        panel.syntheticImageWidth = image.getWidth();
        return panel;
    }

    /** A Video whose only behavior is the frame image; any other call fails the oracle. */
    private static Video video(BufferedImage image) {
        return (Video) Proxy.newProxyInstance(Video.class.getClassLoader(), new Class<?>[] {Video.class}, (proxy, method, args) -> {
            return switch (method.getName()) {
                case "getImage" -> image;
                case "isVisible" -> Boolean.TRUE;
                case "getTypeName" -> "Synthetic"; // not VideoType.TYPE_IMAGE, so no ImageVideo RGB-size checks
                case "hashCode" -> System.identityHashCode(proxy);
                case "equals" -> proxy == args[0];
                case "toString" -> "SyntheticVideo";
                default -> throw new UnsupportedOperationException("Unexpected Video call " + method);
            };
        });
    }

    private interface Measurement {
        Object run() throws Exception;
    }

    private record Outcome(Object value, String error, String diagnostic) {}

    /** Runs a measurement, recording Tracker's printed stack traces and uncaught exceptions as diagnostics. */
    private static Outcome measure(Measurement measurement) {
        PrintStream original = System.err;
        ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        Object value = null;
        String error = null;
        System.setErr(new PrintStream(buffer, true, StandardCharsets.UTF_8));
        try {
            value = measurement.run();
        } catch (Exception thrown) {
            error = thrown.getClass().getName();
        } finally {
            System.setErr(original);
        }
        String printed = buffer.toString(StandardCharsets.UTF_8);
        String diagnostic = printed.contains("ArrayIndexOutOfBoundsException") ? "array_index_out_of_bounds"
            : printed.contains("LineProfileStep image failed") ? "image_failed" : printed.isEmpty() ? null : "stderr";
        return new Outcome(value, error, diagnostic);
    }

    private static String region(RegionSpec spec, Map<String, BufferedImage> images) throws ReflectiveOperationException {
        BufferedImage image = images.get(spec.frame());
        SyntheticPanel panel = panel(image, 0);
        RGBRegion region = new RGBRegion();
        setField(RGBRegion.class, region, "fixedPosition", Boolean.FALSE); // measure this step's own position
        setField(RGBRegion.class, region, "shapeType", spec.shape());
        RGBStep step = new RGBStep(region, 0, spec.x(), spec.y(), spec.width(), spec.height());
        if (spec.vertices() != null) {
            Method set = RGBStep.class.getDeclaredMethod("setPolygonVertices", double[][].class);
            set.setAccessible(true);
            set.invoke(step, (Object) spec.vertices());
        }
        // The region exactly as getRGBData builds it: Tracker's Java2D shape, translated to the position.
        Method shapeAt = RGBStep.class.getDeclaredMethod("getRGBShape", TPoint.class);
        shapeAt.setAccessible(true);
        Shape shape = (Shape) shapeAt.invoke(step, step.getPosition());
        Rectangle bounds = shape.getBounds();
        long mask = 0xcbf29ce484222325L;
        int included = 0;
        for (int y = bounds.y; y < bounds.y + bounds.height; y++) {
            for (int x = bounds.x; x < bounds.x + bounds.width; x++) {
                if (shape.contains(x + 0.5, y + 0.5)) {
                    included++;
                    for (int value : new int[] {x, y}) {
                        for (int shift = 0; shift < 32; shift += 8) {
                            mask ^= value >>> shift & 0xff;
                            mask *= 0x100000001b3L;
                        }
                    }
                }
            }
        }
        Outcome outcome = measure(() -> step.getRGBData(panel));
        StringBuilder json = new StringBuilder();
        json.append("{\"id\":").append(string(spec.id())).append(",\"frame\":").append(string(spec.frame()))
            .append(",\"shape\":").append(string(spec.shape() == SHAPE_ELLIPSE ? "ellipse" : spec.shape() == SHAPE_RECTANGLE ? "rectangle" : "polygon"))
            .append(",\"size\":[").append(spec.width()).append(',').append(spec.height()).append(']')
            .append(",\"position\":[").append(number(spec.x())).append(',').append(number(spec.y())).append(']')
            .append(",\"vertices\":");
        if (spec.vertices() == null) {
            json.append("null");
        } else {
            json.append('[');
            for (int i = 0; i < spec.vertices().length; i++) {
                if (i > 0) json.append(',');
                json.append(numbers(spec.vertices()[i]));
            }
            json.append(']');
        }
        json.append(",\"bounds\":[").append(bounds.x).append(',').append(bounds.y).append(',').append(bounds.width).append(',')
            .append(bounds.height).append("],\"mask_pixels\":").append(included)
            .append(",\"mask_fnv1a64\":").append(string(String.format("%016x", mask)));
        appendOutcome(json, outcome);
        double[] data = (double[]) outcome.value();
        if (data == null) return json.append(",\"result\":null}").toString();
        return json.append(",\"result\":{\"R\":").append(number(data[0])).append(",\"G\":").append(number(data[1]))
            .append(",\"B\":").append(number(data[2])).append(",\"luma\":").append(number(data[3]))
            .append(",\"pixels\":").append(number(data[4])).append(",\"Rsd\":").append(number(data[5]))
            .append(",\"Gsd\":").append(number(data[6])).append(",\"Bsd\":").append(number(data[7])).append("}}").toString();
    }

    private static String profile(ProfileSpec spec, Map<String, BufferedImage> images) throws ReflectiveOperationException {
        SyntheticPanel panel = panel(images.get(spec.frame()), spec.axisAngle());
        LineProfile line = new LineProfile();
        line.setActive(); // steps find their track through the active-track registry
        setField(LineProfile.class, line, "spread", spec.spread());
        setField(LineProfile.class, line, "isHorizontal", spec.alwaysHorizontal());
        LineProfileStep step = new LineProfileStep(line, 0, spec.x0(), spec.y0(), spec.x1(), spec.y1());
        Outcome outcome = measure(() -> step.getProfileData(panel));
        StringBuilder json = new StringBuilder();
        json.append("{\"id\":").append(string(spec.id())).append(",\"frame\":").append(string(spec.frame()))
            .append(",\"always_horizontal\":").append(spec.alwaysHorizontal()).append(",\"axis_angle\":").append(number(spec.axisAngle()))
            .append(",\"ends\":[").append(numbers(new double[] {spec.x0(), spec.y0()})).append(',')
            .append(numbers(new double[] {spec.x1(), spec.y1()})).append("],\"spread\":").append(spec.spread());
        appendOutcome(json, outcome);
        double[][] values = (double[][]) outcome.value();
        if (values == null) return json.append(",\"result\":null}").toString();
        // Tracker's rows: x, y, R, G, B, luma, pixels, n.
        String[] names = {"x", "y", "R", "G", "B", "luma", "pixels", "n"};
        json.append(",\"result\":{");
        for (int row = 0; row < names.length; row++) {
            if (row > 0) json.append(',');
            json.append(string(names[row])).append(':').append(numbers(values[row]));
        }
        return json.append("}}").toString();
    }

    private static void appendOutcome(StringBuilder json, Outcome outcome) {
        json.append(",\"tracker_error\":").append(outcome.error() == null ? "null" : string(outcome.error()))
            .append(",\"tracker_diagnostic\":").append(outcome.diagnostic() == null ? "null" : string(outcome.diagnostic()));
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
