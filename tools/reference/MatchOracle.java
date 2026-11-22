import java.awt.AlphaComposite;
import java.awt.Graphics2D;
import java.awt.Rectangle;
import java.awt.Shape;
import java.awt.geom.AffineTransform;
import java.awt.geom.Ellipse2D;
import java.awt.geom.Rectangle2D;
import java.awt.geom.RectangularShape;
import java.awt.image.BufferedImage;
import java.awt.image.DataBufferInt;
import java.lang.reflect.Field;
import java.math.BigDecimal;
import java.math.MathContext;
import java.math.RoundingMode;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import org.opensourcephysics.media.core.TPoint;
import org.opensourcephysics.media.core.TemplateMatcher;

/** Runs the actual OSP TemplateMatcher of the pinned Tracker 6.3.5 JAR on deterministic synthetic scenes.
 *
 * Scenes are rendered with integer arithmetic and IEEE +, -, *, /, floor and abs only, so that
 * tests/science/template_matcher_reference_test.cpp reproduces every frame bit for bit (it checks the
 * recorded frame hashes). Templates are built and driven the way AutoTracker does: a mask-bounds crop of a
 * TYPE_INT_RGB frame, the mask translated to the bounds origin, setTemplate before each search and
 * buildTemplate(crop at Math.round(match), evolve, tether) after each good match. The sequence driver's
 * search placement is this oracle's own (see docs/template-matching.md), not AutoTracker's look-ahead.
 */
public final class MatchOracle {
    private static final int SUBSAMPLES = 4;
    private static final int GOOD_MATCH = 4; // AutoTracker.goodMatch default

    // ---------------------------------------------------------------------------------------------
    // Scenes: a textured background and one textured, optionally spinning blob on a known trajectory.

    record Scene(int width, int height, int[] background, int texture, double[] center, double[] velocity, double[] acceleration,
                 double radius, double growth, double[] amplitude, double[] drift, double spin, double[] pattern, int noise,
                 long seed, int[] occluder, int[] occluderColor) {

        /** The blob center at frame k, the independent truth for every match. */
        double[] centerAt(int k) {
            double t = k;
            return new double[] {center[0] + velocity[0] * t + 0.5 * acceleration[0] * t * t,
                center[1] + velocity[1] * t + 0.5 * acceleration[1] * t * t};
        }

        String json() {
            return "{\"width\":" + width + ",\"height\":" + height + ",\"background\":" + ints(background) + ",\"texture\":" + texture
                + ",\"center\":" + numbers(center) + ",\"velocity\":" + numbers(velocity) + ",\"acceleration\":" + numbers(acceleration)
                + ",\"radius\":" + number(radius) + ",\"growth\":" + number(growth) + ",\"amplitude\":" + numbers(amplitude)
                + ",\"drift\":" + numbers(drift) + ",\"spin\":" + number(spin) + ",\"pattern\":" + numbers(pattern)
                + ",\"noise\":" + noise + ",\"seed\":" + seed + ",\"occluder\":" + (occluder == null ? "null" : ints(occluder))
                + ",\"occluder_color\":" + (occluderColor == null ? "null" : ints(occluderColor)) + "}";
        }
    }

    private static final double[] PATTERN = {0.11, 0.06, 0.04, 0.12};
    // The blob is larger than the default mask so that templates are mostly object, not static background.
    private static final double BLOB = 12.0;
    private static final int[] BACKGROUND = {70, 90, 60};
    private static final int CLUTTER = 2;
    private static final double[] ZERO = {0.0, 0.0};
    private static final double[] NO_DRIFT = {0.0, 0.0, 0.0};

    private static Scene scene(int width, int height, double cx, double cy, double vx, double vy, double radius, double[] amplitude,
                               int noise, long seed) {
        return new Scene(width, height, BACKGROUND, CLUTTER, new double[] {cx, cy}, new double[] {vx, vy}, ZERO, radius, 0.0,
            amplitude, NO_DRIFT, 0.0, PATTERN, noise, seed, null, null);
    }

    private static double tri(double s) {
        return Math.abs(2 * (s - Math.floor(s + 0.5)));
    }

    private static long splitMix(long[] state) {
        state[0] += 0x9E3779B97F4A7C15L;
        long z = state[0];
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }

    /** Packed 0x00RRGGBB pixels of frame k (row-major), as a TYPE_INT_RGB raster holds them.
     *
     * The blob is an opaque disc over a static, cluttered background: each of 4 x 4 subsamples per pixel
     * shows either the background (base + clutter) or the blob (base + amplitude x shading, with a radial
     * gradient and a spinning triangle-wave pattern); the pixel is their rounded mean. Noise and the
     * occluder come last.
     */
    static int[] render(Scene s, int k) {
        int w = s.width, h = s.height;
        int[] pixels = new int[w * h];
        double t = k;
        double[] c = s.centerAt(k);
        double cx = c[0], cy = c[1];
        double radius = s.radius + s.growth * t;
        double limit = radius * radius;
        double spin = s.spin * t;
        double cos = (1 - spin * spin) / (1 + spin * spin);
        double sin = 2 * spin / (1 + spin * spin);
        double[] amplitude = new double[3];
        for (int i = 0; i < 3; i++) amplitude[i] = s.amplitude[i] + s.drift[i] * t;
        long[] state = {s.seed + k * 0x632BE59BD9B4E019L};
        boolean occluded = s.occluder != null && k >= s.occluder[4] && k <= s.occluder[5];
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int tex = ((x * 7 + y * 13) % 11) - 5;
                int[] base = new int[3];
                for (int channel = 0; channel < 3; channel++) base[channel] = s.background[channel] + tex * s.texture;
                if (!(x + 1 < cx - radius || x > cx + radius || y + 1 < cy - radius || y > cy + radius)) {
                    double[] sum = new double[3];
                    for (int j = 0; j < SUBSAMPLES; j++) {
                        for (int i = 0; i < SUBSAMPLES; i++) {
                            double u = x + (i + 0.5) / SUBSAMPLES - cx;
                            double v = y + (j + 0.5) / SUBSAMPLES - cy;
                            double r2 = u * u + v * v;
                            if (r2 < limit) {
                                double q = 1 - r2 / limit;
                                double ur = cos * u + sin * v;
                                double vr = cos * v - sin * u;
                                double pattern = tri(ur * s.pattern[0] + vr * s.pattern[1]) * tri(ur * s.pattern[2] - vr * s.pattern[3]);
                                double shade = (0.5 + 0.5 * q) * (0.35 + 0.65 * pattern);
                                for (int channel = 0; channel < 3; channel++) sum[channel] += s.background[channel] + amplitude[channel] * shade;
                            } else {
                                for (int channel = 0; channel < 3; channel++) sum[channel] += base[channel];
                            }
                        }
                    }
                    for (int channel = 0; channel < 3; channel++) {
                        base[channel] = (int) Math.floor(sum[channel] / (SUBSAMPLES * SUBSAMPLES) + 0.5);
                    }
                }
                boolean covered = occluded && x >= s.occluder[0] && x < s.occluder[0] + s.occluder[2] && y >= s.occluder[1]
                    && y < s.occluder[1] + s.occluder[3];
                int pixel = 0;
                for (int channel = 0; channel < 3; channel++) {
                    int value = covered ? s.occluderColor[channel] : base[channel];
                    if (s.noise > 0) value += (int) Long.remainderUnsigned(splitMix(state), 2 * s.noise + 1) - s.noise;
                    value = Math.max(0, Math.min(255, value));
                    pixel = (pixel << 8) | value;
                }
                pixels[y * w + x] = pixel;
            }
        }
        return pixels;
    }

    static BufferedImage frame(Scene s, int k) {
        BufferedImage image = new BufferedImage(s.width, s.height, BufferedImage.TYPE_INT_RGB);
        int[] data = pixels(image);
        System.arraycopy(render(s, k), 0, data, 0, data.length);
        return image;
    }

    // ---------------------------------------------------------------------------------------------
    // AutoTracker's use of TemplateMatcher.

    record Mask(boolean ellipse, double x, double y, double width, double height) {
        RectangularShape shape() {
            return ellipse ? new Ellipse2D.Double(x, y, width, height) : new Rectangle2D.Double(x, y, width, height);
        }

        double centerX() {
            return shape().getCenterX();
        }

        double centerY() {
            return shape().getCenterY();
        }

        String json() {
            return "{\"shape\":\"" + (ellipse ? "ellipse" : "rectangle") + "\",\"frame\":" + numbers(new double[] {x, y, width, height}) + "}";
        }
    }

    /** AutoTracker.newVideoImage. */
    static BufferedImage crop(BufferedImage frame, Rectangle rect) {
        BufferedImage image = new BufferedImage(rect.width, rect.height, BufferedImage.TYPE_INT_RGB);
        Graphics2D g = image.createGraphics();
        g.drawImage(frame, -rect.x, -rect.y, null);
        g.dispose();
        return image;
    }

    /** AutoTracker.createTemplateMatcher. */
    static TemplateMatcher create(BufferedImage frame, Mask mask) {
        RectangularShape shape = mask.shape();
        Rectangle rect = shape.getBounds();
        AffineTransform transform = new AffineTransform();
        transform.setToTranslation(-rect.x, -rect.y);
        return new TemplateMatcher(crop(frame, rect), transform.createTransformedShape(shape));
    }

    /** AutoTracker.buildEvolvedTemplateImage after a good match at p. */
    static void evolve(TemplateMatcher matcher, BufferedImage frame, Mask mask, TPoint p, int evolve, int tether) {
        Rectangle rect = mask.shape().getBounds();
        rect.x = (int) Math.round(p.getX());
        rect.y = (int) Math.round(p.getY());
        matcher.buildTemplate(crop(frame, rect), evolve, tether);
    }

    static int[] pixels(BufferedImage image) {
        return ((DataBufferInt) image.getRaster().getDataBuffer()).getData();
    }

    static int field(TemplateMatcher matcher, String name) throws ReflectiveOperationException {
        Field field = TemplateMatcher.class.getDeclaredField(name);
        field.setAccessible(true);
        return field.getInt(matcher);
    }

    /** FNV-1a 64 over each pixel's four little-endian bytes. */
    static String hash(int[] pixels) {
        long h = 0xcbf29ce484222325L;
        for (int pixel : pixels) {
            for (int b = 0; b < 4; b++) {
                h ^= (pixel >>> (8 * b)) & 0xff;
                h *= 0x100000001b3L;
            }
        }
        return "\"" + String.format("%016x", h) + "\"";
    }

    static String templateJson(TemplateMatcher matcher) throws ReflectiveOperationException {
        BufferedImage template = matcher.getTemplate();
        return "{\"size\":" + ints(new int[] {template.getWidth(), template.getHeight()}) + ",\"trim\":"
            + ints(new int[] {field(matcher, "trimLeft"), field(matcher, "trimTop")}) + ",\"hash\":" + hash(pixels(template))
            + ",\"working\":" + hash(matcher.getWorkingPixels()) + ",\"alphas\":" + ints(matcher.getAlphas()) + "}";
    }

    /** One search exactly as AutoTracker.findMatchTarget makes it; the rectangle is mutated by Java. */
    static String search(TemplateMatcher matcher, BufferedImage frame, Rectangle rect, double[] line) {
        matcher.setTemplate(matcher.getTemplate()); // FrameData.getTemplateToMatch
        StringBuilder json = new StringBuilder("{\"rect\":").append(rect(rect));
        int[][] points = null;
        if (line != null) {
            points = matcher.getSearchPoints(rect, line[0], line[1], line[2], 0);
            json.append(",\"line_points\":").append(points(points));
        }
        try {
            TPoint p = line != null ? matcher.getMatchLocation(frame, rect, points, line[0], line[1], line[2])
                : matcher.getMatchLocation(frame, rect, null, 0, 0, 0);
            double[] widthAndHeight = matcher.getMatchWidthAndHeight();
            json.append(",\"after\":").append(rect(rect)).append(",\"point\":")
                .append(p == null ? "null" : numbers(new double[] {p.getX(), p.getY()})).append(",\"width\":")
                .append(number(widthAndHeight[0])).append(",\"height\":").append(number(widthAndHeight[1]));
        } catch (RuntimeException e) {
            json.append(",\"exception\":").append(string(e.getClass().getSimpleName()));
        }
        return json.append('}').toString();
    }

    // ---------------------------------------------------------------------------------------------
    // Cases.

    static String composite() {
        // TemplateMatcher.buildTemplate draws with AlphaComposite(SRC_OVER, 1.0f * alpha / 255) onto TYPE_INT_ARGB.
        List<String> rows = new ArrayList<>();
        for (int alpha = 1; alpha <= 255; alpha++) {
            StringBuilder row = new StringBuilder("{\"alpha\":").append(alpha);
            for (boolean opaque : new boolean[] {true, false}) {
                BufferedImage source = new BufferedImage(256, 256, BufferedImage.TYPE_INT_ARGB);
                BufferedImage target = new BufferedImage(256, 256, BufferedImage.TYPE_INT_ARGB);
                int[] src = pixels(source), dst = pixels(target);
                for (int y = 0; y < 256; y++) {
                    for (int x = 0; x < 256; x++) {
                        int sa = opaque ? 255 : (x * 11 + y * 3) & 0xff;
                        int da = opaque ? 255 : (x * 7 + y * 13) & 0xff;
                        src[y * 256 + x] = (sa << 24) | (x << 16) | (y << 8) | ((x ^ y) & 0xff);
                        dst[y * 256 + x] = (da << 24) | (y << 16) | (x << 8) | ((x + y) & 0xff);
                    }
                }
                Graphics2D g = target.createGraphics();
                g.setComposite(AlphaComposite.getInstance(AlphaComposite.SRC_OVER, 1.0f * alpha / 255));
                g.drawImage(source, 0, 0, null);
                g.dispose();
                row.append(opaque ? ",\"opaque\":" : ",\"translucent\":").append(hash(dst));
            }
            rows.add(row.append('}').toString());
        }
        return String.join(",\n    ", rows);
    }

    /** AutoTracker.refreshKeyFrame(checkSize) for a center and the default 9-pixel corner offset. */
    static Mask keyFrameMask(boolean ellipse, double cx, double cy, double size) {
        TPoint center = new TPoint(cx, cy);
        TPoint corner = new TPoint(cx + size, cy + size);
        double sin = center.sin(corner);
        double cos = center.cos(corner);
        double d = Math.max(4, center.distance(corner));
        double dx = d * cos;
        double dy = -d * sin;
        RectangularShape shape = ellipse ? new Ellipse2D.Double() : new Rectangle2D.Double();
        shape.setFrameFromCenter(cx, cy, cx + dx, cy + dy);
        return new Mask(ellipse, shape.getX(), shape.getY(), shape.getWidth(), shape.getHeight());
    }

    static final Scene MASK_SCENE = new Scene(64, 48, BACKGROUND, CLUTTER, new double[] {30.5, 22.5}, ZERO, ZERO, 20.0, 0.0,
        new double[] {120, -40, 60}, NO_DRIFT, 0.0, PATTERN, 9, 3, null, null);

    static Object[][] masks() {
        return new Object[][] {
            {"ellipse-9", new Mask(true, 10, 10, 9, 9)},
            {"ellipse-key-default", keyFrameMask(true, 40.37, 30.81, 9)},
            {"ellipse-fractional", new Mask(true, 3.125, 5.5, 7.5, 11.25)},
            {"ellipse-min", new Mask(true, 20, 12, 8, 8)},
            {"ellipse-3", new Mask(true, 6, 6, 3, 3)},
            {"ellipse-empty", new Mask(true, 5, 5, 2, 2)},
            {"ellipse-elongated", new Mask(true, 12.25, 30.5, 30, 6)},
            {"ellipse-negative", new Mask(true, -3.5, -2.25, 12, 10)},
            {"ellipse-far", new Mask(true, 1000.75, 2000.5, 13.5, 13.5)},
            {"ellipse-max", new Mask(true, 0.4, 0.6, 99, 99)},
            {"rectangle-9", new Mask(false, 10, 10, 9, 9)},
            {"rectangle-key-default", keyFrameMask(false, 40.37, 30.81, 9)},
            {"rectangle-fractional", new Mask(false, 2.5, 3.75, 10.5, 7.25)},
            {"rectangle-even", new Mask(false, 7, 9, 11, 8)},
        };
    }

    static String maskCase(String id, Mask mask) throws ReflectiveOperationException {
        RectangularShape shape = mask.shape();
        Rectangle bounds = shape.getBounds();
        AffineTransform transform = new AffineTransform();
        transform.setToTranslation(-bounds.x, -bounds.y);
        Shape translated = transform.createTransformedShape(shape);
        // Path2D containment on a quarter-pixel grid one pixel beyond the bounds, hashed as 0/1 ints.
        List<Integer> inside = new ArrayList<>();
        for (int j = -4; j <= 4 * bounds.height + 4; j++) {
            for (int i = -4; i <= 4 * bounds.width + 4; i++) inside.add(translated.contains(i / 4.0, j / 4.0) ? 1 : 0);
        }
        int[] grid = inside.stream().mapToInt(Integer::intValue).toArray();
        // A fixed textured frame: masks reaching beyond it (negative or far origins) crop black pixels.
        BufferedImage frame = frame(MASK_SCENE, 0);
        StringBuilder json = new StringBuilder("{\"id\":").append(string(id)).append(",\"scene\":").append(MASK_SCENE.json())
            .append(",\"mask\":").append(mask.json()).append(",\"bounds\":").append(rect(bounds)).append(",\"contains_grid\":").append(hash(grid));
        try {
            TemplateMatcher matcher = create(frame, mask);
            BufferedImage template = matcher.getTemplate();
            int[] data = pixels(template);
            List<String> rows = new ArrayList<>();
            for (int y = 0; y < template.getHeight(); y++) {
                StringBuilder row = new StringBuilder();
                for (int x = 0; x < template.getWidth(); x++) row.append((data[y * template.getWidth() + x] >>> 24) == 0 ? '0' : '1');
                rows.add(string(row.toString()));
            }
            json.append(",\"template\":").append(templateJson(matcher)).append(",\"rows\":[").append(String.join(",", rows)).append(']');
            // The match preview (getMatchImage) of an exact key-frame match, for masks inside the frame:
            // how many included template pixels it shows differently from the matched pixels.
            String preview = "null";
            if (bounds.x >= 0 && bounds.y >= 0 && bounds.x + bounds.width < frame.getWidth() && bounds.y + bounds.height < frame.getHeight()) {
                int centerX = bounds.x + field(matcher, "trimLeft") + template.getWidth() / 2;
                int centerY = bounds.y + field(matcher, "trimTop") + template.getHeight() / 2;
                matcher.getMatchLocation(frame, new Rectangle(centerX - 2, centerY - 2, 5, 5), null, 0, 0, 0);
                int[] shown = pixels(matcher.getMatchImage());
                int differing = 0;
                for (int i = 0; i < data.length; i++) if ((data[i] >>> 24) != 0 && (data[i] & 0xffffff) != (shown[i] & 0xffffff)) differing++;
                preview = Integer.toString(differing);
            }
            json.append(",\"preview_mismatches\":").append(preview);
        } catch (RuntimeException e) {
            json.append(",\"exception\":").append(string(e.getClass().getSimpleName()));
        }
        return json.append('}').toString();
    }

    static String evolution() throws ReflectiveOperationException {
        Scene scene = new Scene(64, 48, new int[] {40, 120, 200}, 4, new double[] {30.3, 22.6}, new double[] {1.1, -0.7}, ZERO, BLOB,
            0.2, new double[] {150, -60, 40}, new double[] {-3, 5, 2}, 0.05, PATTERN, 6, 77, null, null);
        Mask mask = new Mask(true, 21.55, 13.85, 17.5, 17.5);
        TemplateMatcher matcher = create(frame(scene, 0), mask);
        StringBuilder json = new StringBuilder("{\"scene\":").append(scene.json()).append(",\"mask\":").append(mask.json())
            .append(",\"created\":").append(templateJson(matcher)).append(",\"steps\":[");
        // {frame, origin x, origin y, alphaInput, alphaOriginal}: defaults (20%, 5%), extremes, clamping,
        // the both-zero no-op and crops reaching outside the frame.
        int[][] steps = {{1, 22, 13, 51, 12}, {2, 23, 12, 255, 0}, {3, 25, 11, 0, 128}, {4, 26, 10, 254, 1}, {5, 50, 40, 1, 255},
            {6, -6, -9, 0, 0}, {6, -6, -9, -5, 0}, {7, 28, 9, 300, 300}, {8, 29, 8, 51, 12}, {9, 31, 7, 128, 128}};
        for (int i = 0; i < steps.length; i++) {
            int[] step = steps[i];
            Rectangle rect = mask.shape().getBounds();
            rect.x = step[1];
            rect.y = step[2];
            BufferedImage built = matcher.buildTemplate(crop(frame(scene, step[0]), rect), step[3], step[4]);
            if (i > 0) json.append(',');
            json.append("{\"frame\":").append(step[0]).append(",\"origin\":").append(ints(new int[] {step[1], step[2]}))
                .append(",\"alphas\":").append(ints(new int[] {step[3], step[4]})).append(",\"built\":")
                .append(built != null).append(",\"template\":").append(templateJson(matcher)).append('}');
        }
        return json.append("]}").toString();
    }

    record MatchSpec(String id, Scene scene, Mask mask, int target, int[] search, double[] line, int[] evolve) {}

    static List<MatchSpec> matchSpecs() {
        List<MatchSpec> specs = new ArrayList<>();
        double[] bright = {150, -70, 90};
        Scene still = scene(80, 60, 37.4, 28.7, 0, 0, BLOB, bright, 0, 0);
        Mask key = keyFrameMask(true, 37.4, 28.7, 9);
        int cx = 37, cy = 29;
        specs.add(new MatchSpec("exact", still, key, 0, new int[] {cx - 20, cy - 20, 40, 40}, null, null));
        specs.add(new MatchSpec("outside-right", still, key, 0, new int[] {200, 10, 20, 20}, null, null));
        specs.add(new MatchSpec("outside-below", still, key, 0, new int[] {10, 59, 20, 20}, null, null));
        specs.add(new MatchSpec("partly-outside", still, key, 0, new int[] {-15, -12, 50, 45}, null, null));
        specs.add(new MatchSpec("oversized", still, key, 0, new int[] {-100, -100, 400, 300}, null, null));
        Scene flat = new Scene(40, 30, new int[] {90, 90, 90}, 0, new double[] {20, 15}, ZERO, ZERO, 1.0, 0.0, new double[] {0, 0, 0},
            NO_DRIFT, 0.0, PATTERN, 0, 0, null, null);
        specs.add(new MatchSpec("uniform", flat, new Mask(true, 14, 9, 12, 12), 0, new int[] {8, 6, 20, 16}, null, null));
        specs.add(new MatchSpec("template-larger-than-frame", scene(10, 8, 5, 4, 0, 0, 3, bright, 0, 0), new Mask(true, 0.5, 0.5, 18, 18), 0,
            new int[] {0, 0, 10, 8}, null, null));
        specs.add(new MatchSpec("one-pixel-template", scene(40, 30, 20.2, 14.6, 0.4, 0.3, 5, bright, 0, 0), new Mask(true, 18.5, 13, 3, 3), 1,
            new int[] {12, 8, 16, 14}, null, null));
        // Subpixel shifts of a still textured blob: known offsets between key frame and target.
        double[][] shifts = {{0.25, 0.0}, {0.5, 0.5}, {0.75, -0.25}, {-0.375, 0.625}, {1.2, -2.7}, {-3.45, 1.05}};
        for (double[] shift : shifts) {
            Scene moved = new Scene(80, 60, BACKGROUND, CLUTTER, new double[] {37.4, 28.7}, shift, ZERO, BLOB, 0.0, bright, NO_DRIFT,
                0.0, PATTERN, 0, 0, null, null);
            specs.add(new MatchSpec("shift(" + shift[0] + "," + shift[1] + ")", moved, key, 1, new int[] {cx - 6, cy - 6, 13, 13}, null, null));
        }
        // Targets beyond each edge of a small search area (the reference's boundary fit).
        double[][] beyond = {{4.4, 0.3}, {-4.4, 0.3}, {0.3, 4.4}, {0.3, -4.4}, {-2.6, 1.3}, {1.3, -2.6}};
        for (double[] shift : beyond) {
            Scene moved = new Scene(80, 60, BACKGROUND, CLUTTER, new double[] {37.4, 28.7}, shift, ZERO, BLOB, 0.0, bright, NO_DRIFT,
                0.0, PATTERN, 0, 0, null, null);
            specs.add(new MatchSpec("beyond(" + shift[0] + "," + shift[1] + ")", moved, key, 1, new int[] {cx - 2, cy - 2, 5, 5}, null, null));
        }
        // Targets just beyond the top and left edges, where Tracker's missing or wrapped neighbor also
        // changes the peak width and with it whether the match is accepted.
        Mask edgeMask = new Mask(true, 28.5, 19.75, 18, 18);
        for (double[] shift : new double[][] {{0.0, -3.5}, {-3.4, 0.2}, {0.4, -3.0}}) {
            Scene moved = new Scene(80, 60, BACKGROUND, CLUTTER, new double[] {37.5, 28.75}, shift, ZERO, BLOB, 0.0, bright, NO_DRIFT,
                0.0, PATTERN, 0, 0, null, null);
            specs.add(new MatchSpec("beyond-edge(" + shift[0] + "," + shift[1] + ")", moved, edgeMask, 1, new int[] {35, 26, 5, 5}, null,
                null));
        }
        // Even-sized trimmed template (rectangle mask, integer frame) and an evolved template.
        Mask rectangle = new Mask(false, 30, 22, 11, 14);
        Scene drift = scene(80, 60, 35.6, 29.2, 0.8, -0.45, 9.0, bright, 4, 99);
        specs.add(new MatchSpec("even-rectangle", drift, rectangle, 2, new int[] {28, 20, 20, 20}, null, null));
        specs.add(new MatchSpec("evolved", drift, key, 3, new int[] {28, 22, 20, 20}, null, new int[] {51, 12}));
        specs.add(new MatchSpec("low-contrast", scene(80, 60, 37.4, 28.7, 0.6, 0.35, BLOB, new double[] {9, -6, 7}, 2, 5), key, 2,
            new int[] {cx - 8, cy - 8, 17, 17}, null, null));
        // One-dimensional searches along an x-axis (AutoTracker's 1D mode).
        // {angle, x0, y0, distance along the axis, cos angle, sin angle (6 decimals)}
        double[][] lines = {{0.0, 37.4, 28.7, 2.35, 1, 0}, {Math.PI, 37.4, 28.7, -2.35, -1, 0},
            {0.3, 37.4, 28.7, 2.35, 0.955336, 0.295520}, {2.0, 37.4, 28.7, 1.6, -0.416147, 0.909297},
            {Math.PI / 2, 37.4, 28.7, -2.2, 0, 1}, {-Math.PI / 4, 37.4, 28.7, 1.8, 0.707107, -0.707107}};
        for (double[] line : lines) {
            double angle = line[0];
            double along = line[3];
            double[] velocity = {along * line[4], -along * line[5]};
            Scene moved = new Scene(80, 60, BACKGROUND, CLUTTER, new double[] {37.4, 28.7}, velocity, ZERO, BLOB, 0.0, bright,
                NO_DRIFT, 0.0, PATTERN, 0, 0, null, null);
            specs.add(new MatchSpec("line(" + angle + ")", moved, key, 1, new int[] {cx - 10, cy - 10, 21, 21},
                new double[] {line[1], line[2], angle}, null));
        }
        // Line cases near the frame edge, missing the area, and with no line pixel inside the trimmed area.
        Mask edgeKey = keyFrameMask(true, 9.6, 30.2, 9);
        for (double[] direction : new double[][] {{0.0, 1, 0}, {0.2, 0.980067, 0.198669}}) {
            double angle = direction[0];
            Scene edge = new Scene(80, 60, BACKGROUND, CLUTTER, new double[] {9.6, 30.2},
                new double[] {1.7 * direction[1], -1.7 * direction[2]}, ZERO, BLOB, 0.0, bright, NO_DRIFT, 0.0, PATTERN, 0, 0, null, null);
            specs.add(new MatchSpec("line-trimmed(" + angle + ")", edge, edgeKey, 1, new int[] {0, 20, 24, 21},
                new double[] {9.6, 30.2, angle}, null));
        }
        // Lattice-aligned axes through a pixel corner: grid crossings coincide, so the points depend on
        // the last bits of Math.tan.
        Mask latticeKey = keyFrameMask(true, 37.0, 29.0, 9);
        double[][] lattice = {{-Math.PI / 4, 1.9, 0.707107, -0.707107}, {Math.PI / 4, -1.9, 0.707107, 0.707107},
            {Math.atan(0.5), 2.2, 0.894427, 0.447214}};
        for (double[] axis : lattice) {
            Scene moved = new Scene(80, 60, BACKGROUND, CLUTTER, new double[] {37.0, 29.0}, new double[] {axis[1] * axis[2], -axis[1] * axis[3]},
                ZERO, BLOB, 0.0, bright, NO_DRIFT, 0.0, PATTERN, 0, 0, null, null);
            specs.add(new MatchSpec("line-lattice(" + axis[0] + ")", moved, latticeKey, 1, new int[] {27, 19, 21, 21},
                new double[] {37.0, 29.0, axis[0]}, null));
        }
        specs.add(new MatchSpec("line-misses-area", still, key, 0, new int[] {cx - 10, cy - 10, 21, 21}, new double[] {0, 5, 0.0}, null));
        specs.add(new MatchSpec("line-outside-trimmed-area", still, key, 0, new int[] {60, 20, 20, 20}, new double[] {75, 0, Math.PI / 2},
            null));
        return specs;
    }

    static String matchCase(MatchSpec spec) throws ReflectiveOperationException {
        TemplateMatcher matcher = create(frame(spec.scene, 0), spec.mask);
        BufferedImage target = frame(spec.scene, spec.target);
        if (spec.evolve != null) {
            for (int k = 1; k < spec.target; k++) {
                BufferedImage image = frame(spec.scene, k);
                double[] c = spec.scene.centerAt(k);
                double[] c0 = spec.scene.centerAt(0);
                Rectangle bounds = spec.mask.shape().getBounds();
                TPoint p = new TPoint(bounds.x + (c[0] - c0[0]), bounds.y + (c[1] - c0[1]));
                evolve(matcher, image, spec.mask, p, spec.evolve[0], spec.evolve[1]);
            }
        }
        Rectangle rect = new Rectangle(spec.search[0], spec.search[1], spec.search[2], spec.search[3]);
        StringBuilder json = new StringBuilder("{\"id\":").append(string(spec.id)).append(",\"scene\":").append(spec.scene.json())
            .append(",\"mask\":").append(spec.mask.json()).append(",\"target\":").append(spec.target)
            .append(",\"evolve\":").append(spec.evolve == null ? "null" : ints(spec.evolve))
            .append(",\"line\":").append(spec.line == null ? "null" : numbers(spec.line)).append(",\"template\":")
            .append(templateJson(matcher)).append(",\"result\":").append(search(matcher, target, rect, spec.line));
        return json.append('}').toString();
    }

    // ---------------------------------------------------------------------------------------------
    // Math.sin/cos/tan, which getSearchPoints and getMatchLocation use. HotSpot x86-64 intrinsics
    // are correctly rounded for all but a few arguments; MoTrack's java_math is correctly rounded.

    static final int TRIG_FIRST = -20000, TRIG_LAST = 20000;
    static final double TRIG_STEP = 0.000157;
    static final MathContext TRIG_PRECISION = new MathContext(50, RoundingMode.HALF_EVEN);
    static final BigDecimal PI = new BigDecimal("3.14159265358979323846264338327950288419716939937510582097494459230781640628620899863");
    static final BigDecimal HALF_PI = PI.divide(BigDecimal.valueOf(2), TRIG_PRECISION);

    /** sin, cos and tan of the exact double `angle` to about 45 digits. */
    static BigDecimal[] exactTrig(double angle) {
        BigDecimal x = new BigDecimal(angle);
        BigDecimal k = x.divide(HALF_PI, TRIG_PRECISION).setScale(0, RoundingMode.HALF_EVEN);
        BigDecimal r = x.subtract(k.multiply(HALF_PI, TRIG_PRECISION), TRIG_PRECISION);
        BigDecimal r2 = r.multiply(r, TRIG_PRECISION);
        BigDecimal sin = r, cos = BigDecimal.ONE, sinTerm = r, cosTerm = BigDecimal.ONE;
        for (int n = 1; n <= 24; n++) {
            sinTerm = sinTerm.multiply(r2, TRIG_PRECISION).divide(BigDecimal.valueOf((2L * n) * (2L * n + 1)), TRIG_PRECISION).negate();
            cosTerm = cosTerm.multiply(r2, TRIG_PRECISION).divide(BigDecimal.valueOf((2L * n - 1) * (2L * n)), TRIG_PRECISION).negate();
            sin = sin.add(sinTerm, TRIG_PRECISION);
            cos = cos.add(cosTerm, TRIG_PRECISION);
        }
        BigDecimal s, c;
        switch (Math.floorMod(k.intValueExact(), 4)) {
            case 0 -> { s = sin; c = cos; }
            case 1 -> { s = cos; c = sin.negate(); }
            case 2 -> { s = sin.negate(); c = cos.negate(); }
            default -> { s = cos.negate(); c = sin; }
        }
        return new BigDecimal[] {s, c, s.divide(c, TRIG_PRECISION)};
    }

    static String bits(double value) {
        return "\"" + String.format("%016x", Double.doubleToRawLongBits(value)) + "\"";
    }

    static String trig() {
        String[] names = {"sin", "cos", "tan"};
        long[] hashes = {0xcbf29ce484222325L, 0xcbf29ce484222325L, 0xcbf29ce484222325L};
        List<String> exceptions = new ArrayList<>();
        for (int k = TRIG_FIRST; k <= TRIG_LAST; k++) {
            double angle = k * TRIG_STEP;
            double[] java = {Math.sin(angle), Math.cos(angle), Math.tan(angle)};
            BigDecimal[] exact = angle == 0 ? null : exactTrig(angle);
            for (int f = 0; f < 3; f++) {
                long value = Double.doubleToRawLongBits(java[f]);
                for (int b = 0; b < 8; b++) {
                    hashes[f] ^= (value >>> (8 * b)) & 0xff;
                    hashes[f] *= 0x100000001b3L;
                }
                double correct = exact == null ? java[f] : exact[f].doubleValue();
                if (Double.doubleToRawLongBits(correct) != value) {
                    exceptions.add("{\"k\":" + k + ",\"function\":\"" + names[f] + "\",\"java\":" + bits(java[f]) + ",\"correct\":"
                        + bits(correct) + "}");
                }
            }
        }
        return "{\"first\":" + TRIG_FIRST + ",\"last\":" + TRIG_LAST + ",\"step\":" + number(TRIG_STEP) + ",\"hashes\":{\"sin\":\""
            + String.format("%016x", hashes[0]) + "\",\"cos\":\"" + String.format("%016x", hashes[1]) + "\",\"tan\":\""
            + String.format("%016x", hashes[2]) + "\"},\"not_correctly_rounded\":[" + String.join(",", exceptions) + "]}";
    }

    static String searchPointCases() {
        Object[][] specs = {
            {new int[] {10, 20, 21, 15}, new double[] {5, 27.5, 0.0}}, {new int[] {10, 20, 21, 15}, new double[] {5, 27.5, Math.PI}},
            {new int[] {10, 20, 21, 15}, new double[] {20.5, 3, Math.PI / 2}}, {new int[] {10, 20, 21, 15}, new double[] {20.5, 3, -Math.PI / 2}},
            {new int[] {10, 20, 21, 15}, new double[] {12.3, 30.1, 0.3}}, {new int[] {10, 20, 21, 15}, new double[] {12.3, 30.1, 2.0}},
            {new int[] {10, 20, 21, 15}, new double[] {10, 20, -Math.PI / 4}}, {new int[] {10, 20, 20, 20}, new double[] {10, 40, Math.PI / 4}},
            {new int[] {10, 20, 21, 15}, new double[] {0, 27.25, 1e-11}}, {new int[] {10, 20, 21, 15}, new double[] {15.75, 0, Math.PI / 2 - 1e-12}},
            {new int[] {10, 20, 21, 15}, new double[] {0, 0, 0.0}}, {new int[] {10, 20, 21, 15}, new double[] {31, 20, -1.0}},
            {new int[] {-7, -5, 12, 9}, new double[] {-3.3, -2.2, 0.7}}, {new int[] {-7, -5, 12, 9}, new double[] {-7, -1.5, 0.0}},
            {new int[] {4, 4, 0, 6}, new double[] {4, 5.5, 0.4}}, {new int[] {4, 4, 6, 0}, new double[] {2, 4, 0.0}},
            {new int[] {0, 0, 60, 40}, new double[] {30.0, 20.0, 1.234}}, {new int[] {0, 0, 60, 40}, new double[] {13.0, 7.0, -2.5}},
            // A platform tan one ulp off Java's changes this point count (independent review of #239).
            {new int[] {10, 20, 21, 15}, new double[] {11, 32, -17490 * 0.000157}},
            // Lattice-aligned axes from the rectangle's corners.
            {new int[] {10, 20, 21, 15}, new double[] {10, 20, -Math.atan(0.5)}},
            {new int[] {10, 20, 21, 15}, new double[] {31, 35, 3 * Math.PI / 4}},
        };
        List<String> rows = new ArrayList<>();
        TemplateMatcher matcher = new TemplateMatcher(new BufferedImage(3, 3, BufferedImage.TYPE_INT_RGB), null);
        for (Object[] spec : specs) {
            int[] r = (int[]) spec[0];
            double[] line = (double[]) spec[1];
            StringBuilder row = new StringBuilder("{\"rect\":").append(ints(r)).append(",\"line\":").append(numbers(line));
            try {
                int[][] points = matcher.getSearchPoints(new Rectangle(r[0], r[1], r[2], r[3]), line[0], line[1], line[2], 0);
                row.append(",\"points\":").append(points(points));
            } catch (RuntimeException e) {
                row.append(",\"exception\":").append(string(e.getClass().getSimpleName()));
            }
            rows.add(row.append('}').toString());
        }
        return String.join(",\n    ", rows);
    }

    record SequenceSpec(String id, Scene scene, int frames, Mask mask, double halfSearch, int[] evolve, double angle, double tolerance) {}

    static List<SequenceSpec> sequenceSpecs() {
        List<SequenceSpec> specs = new ArrayList<>();
        double[] bright = {150, -70, 90};
        Scene clean = new Scene(96, 72, BACKGROUND, CLUTTER, new double[] {20.3, 50.6}, new double[] {1.37, -0.61},
            new double[] {0.02, -0.015}, BLOB, 0.0, bright, NO_DRIFT, 0.0, PATTERN, 0, 0, null, null);
        Mask key = keyFrameMask(true, 20.3, 50.6, 9);
        specs.add(new SequenceSpec("clean", clean, 30, key, 10, new int[] {51, 12}, Double.NaN, 0.4));
        specs.add(new SequenceSpec("clean-fixed-template", clean, 30, key, 10, new int[] {0, 0}, Double.NaN, 0.15));
        Scene noisy = new Scene(96, 72, BACKGROUND, CLUTTER, new double[] {20.3, 50.6}, new double[] {1.37, -0.61},
            new double[] {0.02, -0.015}, BLOB, 0.0, bright, NO_DRIFT, 0.0, PATTERN, 12, 12345, null, null);
        specs.add(new SequenceSpec("noisy", noisy, 30, key, 10, new int[] {51, 12}, Double.NaN, 0.4));
        Scene occluded = new Scene(96, 72, BACKGROUND, CLUTTER, new double[] {20.3, 50.6}, new double[] {1.37, -0.61}, ZERO, BLOB,
            0.0, bright, NO_DRIFT, 0.0, PATTERN, 4, 2024, new int[] {26, 38, 22, 24, 6, 9}, new int[] {30, 30, 30});
        specs.add(new SequenceSpec("occlusion", occluded, 24, key, 12, new int[] {51, 12}, Double.NaN, 0.3));
        Scene faint = new Scene(96, 72, BACKGROUND, 1, new double[] {30.7, 35.2}, new double[] {0.83, 0.41}, ZERO, BLOB, 0.0,
            new double[] {34, -22, 26}, NO_DRIFT, 0.0, PATTERN, 3, 31, null, null);
        specs.add(new SequenceSpec("low-contrast", faint, 25, keyFrameMask(true, 30.7, 35.2, 9), 8, new int[] {51, 12}, Double.NaN, 0.5));
        Scene evolving = new Scene(96, 72, new int[] {60, 80, 100}, 3, new double[] {25.2, 36.1}, new double[] {1.05, 0.22}, ZERO, BLOB,
            0.06, new double[] {150, -70, 90}, new double[] {-1.5, 1.0, -1.2}, 0.012, PATTERN, 3, 8, null, null);
        Mask evolvingKey = keyFrameMask(true, 25.2, 36.1, 9);
        specs.add(new SequenceSpec("evolving", evolving, 30, evolvingKey, 8, new int[] {51, 12}, Double.NaN, 0.75));
        specs.add(new SequenceSpec("evolving-fixed-template", evolving, 30, evolvingKey, 8, new int[] {0, 0}, Double.NaN, 0.15));
        Scene fast = new Scene(96, 72, BACKGROUND, CLUTTER, new double[] {14.1, 12.7}, new double[] {2.65, 1.9}, ZERO, BLOB, 0.0,
            bright, NO_DRIFT, 0.0, PATTERN, 5, 777, null, null);
        specs.add(new SequenceSpec("fast", fast, 24, keyFrameMask(true, 14.1, 12.7, 9), 6, new int[] {51, 12}, Double.NaN, 0.6));
        specs.add(new SequenceSpec("rectangle", clean, 30, keyFrameMask(false, 20.3, 50.6, 9), 10, new int[] {51, 12}, Double.NaN, 0.35));
        Scene exiting = new Scene(96, 72, BACKGROUND, CLUTTER, new double[] {70.4, 40.3}, new double[] {1.85, 0.2}, ZERO, BLOB, 0.0,
            bright, NO_DRIFT, 0.0, PATTERN, 2, 4, null, null);
        specs.add(new SequenceSpec("edge-exit", exiting, 20, keyFrameMask(true, 70.4, 40.3, 9), 10, new int[] {51, 12}, Double.NaN, 0.3));
        double angle = 0.3;
        Scene alongLine = new Scene(96, 72, BACKGROUND, CLUTTER, new double[] {20.3, 50.6},
            new double[] {1.41 * 0.955336, -1.41 * 0.295520}, ZERO, BLOB, 0.0, bright, NO_DRIFT, 0.0, PATTERN, 2, 64, null,
            null);
        specs.add(new SequenceSpec("line-tilted", alongLine, 30, key, 10, new int[] {0, 0}, angle, 0.3));
        Scene horizontal = new Scene(96, 72, BACKGROUND, CLUTTER, new double[] {20.3, 50.6}, new double[] {1.73, 0}, ZERO, BLOB, 0.0,
            bright, NO_DRIFT, 0.0, PATTERN, 2, 65, null, null);
        specs.add(new SequenceSpec("line-horizontal", horizontal, 30, key, 10, new int[] {51, 12}, 0.0, 0.3));
        return specs;
    }

    /** A fixed driver: search around the velocity-predicted center of the latest good matches, evolve after good ones. */
    static String sequence(SequenceSpec spec) throws ReflectiveOperationException {
        Scene scene = spec.scene;
        Mask mask = spec.mask;
        Rectangle bounds = mask.shape().getBounds();
        TemplateMatcher matcher = create(frame(scene, 0), mask);
        StringBuilder json = new StringBuilder("{\"id\":").append(string(spec.id)).append(",\"scene\":").append(scene.json())
            .append(",\"frames\":").append(spec.frames).append(",\"mask\":").append(mask.json()).append(",\"half_search\":")
            .append(number(spec.halfSearch)).append(",\"evolve\":").append(ints(spec.evolve)).append(",\"angle\":")
            .append(Double.isNaN(spec.angle) ? "null" : number(spec.angle)).append(",\"tolerance\":").append(number(spec.tolerance))
            .append(",\"steps\":[");
        double maskX = mask.centerX(), maskY = mask.centerY();
        int lastFrame = -1, previousFrame = -1;
        double lastX = maskX, lastY = maskY, previousX = 0, previousY = 0;
        double[] origin = scene.centerAt(0);
        for (int k = 0; k < spec.frames; k++) {
            double px = lastX, py = lastY;
            if (previousFrame >= 0) {
                double vx = (lastX - previousX) / (lastFrame - previousFrame);
                double vy = (lastY - previousY) / (lastFrame - previousFrame);
                px = lastX + vx * (k - lastFrame);
                py = lastY + vy * (k - lastFrame);
            }
            Rectangle rect = new Rectangle2D.Double(px - spec.halfSearch, py - spec.halfSearch, 2 * spec.halfSearch, 2 * spec.halfSearch)
                .getBounds();
            String searched = rect(rect);
            BufferedImage image = frame(scene, k);
            double[] line = null;
            if (!Double.isNaN(spec.angle)) line = new double[] {origin[0], origin[1], spec.angle};
            matcher.setTemplate(matcher.getTemplate());
            TPoint p;
            int[][] points = null;
            if (line != null) {
                points = matcher.getSearchPoints(rect, line[0], line[1], line[2], 0);
                p = matcher.getMatchLocation(image, rect, points, line[0], line[1], line[2]);
            } else {
                p = matcher.getMatchLocation(image, rect, null, 0, 0, 0);
            }
            double[] wh = matcher.getMatchWidthAndHeight();
            boolean good = p != null && (Double.isInfinite(wh[1]) || wh[1] >= GOOD_MATCH);
            if (k > 0) json.append(',');
            json.append("{\"k\":").append(k).append(",\"frame_hash\":").append(hash(pixels(image))).append(",\"rect\":")
                .append(searched).append(",\"after\":").append(rect(rect)).append(",\"point\":").append(p == null ? "null" : numbers(new double[] {p.getX(), p.getY()})).append(",\"width\":")
                .append(number(wh[0])).append(",\"height\":").append(number(wh[1])).append(",\"good\":").append(good);
            if (good) {
                double cx = p.getX() + maskX - bounds.x;
                double cy = p.getY() + maskY - bounds.y;
                previousFrame = lastFrame;
                previousX = lastX;
                previousY = lastY;
                lastFrame = k;
                lastX = cx;
                lastY = cy;
                if (spec.evolve[0] != 0 || spec.evolve[1] != 0) {
                    evolve(matcher, image, mask, p, spec.evolve[0], spec.evolve[1]);
                    json.append(",\"template\":").append(hash(pixels(matcher.getTemplate())));
                }
            }
            json.append('}');
        }
        return json.append("]}").toString();
    }

    // ---------------------------------------------------------------------------------------------

    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("usage: MatchOracle <output.json>");
        StringBuilder json = new StringBuilder("{\n");
        json.append("  \"schema_version\": 1,\n  \"reference_version\": \"6.3.5\",\n");
        json.append("  \"composite\": [\n    ").append(composite()).append("\n  ],\n");
        List<String> masks = new ArrayList<>();
        for (Object[] mask : masks()) masks.add(maskCase((String) mask[0], (Mask) mask[1]));
        json.append("  \"masks\": [\n    ").append(String.join(",\n    ", masks)).append("\n  ],\n");
        json.append("  \"evolution\": ").append(evolution()).append(",\n");
        json.append("  \"trig\": ").append(trig()).append(",\n");
        json.append("  \"search_points\": [\n    ").append(searchPointCases()).append("\n  ],\n");
        List<String> matches = new ArrayList<>();
        for (MatchSpec spec : matchSpecs()) matches.add(matchCase(spec));
        json.append("  \"matches\": [\n    ").append(String.join(",\n    ", matches)).append("\n  ],\n");
        List<String> sequences = new ArrayList<>();
        for (SequenceSpec spec : sequenceSpecs()) sequences.add(sequence(spec));
        json.append("  \"sequences\": [\n    ").append(String.join(",\n    ", sequences)).append("\n  ]\n}\n");
        Files.writeString(Path.of(args[0]), json.toString(), StandardCharsets.UTF_8);
    }

    // ---------------------------------------------------------------------------------------------
    // JSON helpers.

    static String rect(Rectangle r) {
        return ints(new int[] {r.x, r.y, r.width, r.height});
    }

    static String points(int[][] points) {
        if (points == null) return "null";
        StringBuilder json = new StringBuilder("[");
        for (int i = 0; i < points.length; i++) {
            if (i > 0) json.append(',');
            json.append('[').append(points[i][0]).append(',').append(points[i][1]).append(']');
        }
        return json.append(']').toString();
    }

    static String ints(int[] values) {
        StringBuilder json = new StringBuilder("[");
        for (int i = 0; i < values.length; i++) {
            if (i > 0) json.append(',');
            json.append(values[i]);
        }
        return json.append(']').toString();
    }

    static String numbers(double[] values) {
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

    static String string(String value) {
        StringBuilder json = new StringBuilder("\"");
        for (char c : value.toCharArray()) {
            if (c == '"' || c == '\\') json.append('\\').append(c);
            else if (c < 0x20) json.append(String.format("\\u%04x", (int) c));
            else json.append(c);
        }
        return json.append('"').toString();
    }
}
