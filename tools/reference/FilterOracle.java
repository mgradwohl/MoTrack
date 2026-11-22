import java.awt.Graphics2D;
import java.awt.image.BufferedImage;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.TreeSet;
import org.opensourcephysics.controls.XMLControl;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.BaselineFilter;
import org.opensourcephysics.media.core.BrightnessFilter;
import org.opensourcephysics.media.core.DeinterlaceFilter;
import org.opensourcephysics.media.core.Filter;
import org.opensourcephysics.media.core.FilterStack;
import org.opensourcephysics.media.core.GhostFilter;
import org.opensourcephysics.media.core.GrayScaleFilter;
import org.opensourcephysics.media.core.PerspectiveFilter;
import org.opensourcephysics.media.core.RadialDistortionFilter;
import org.opensourcephysics.media.core.ResizeFilter;
import org.opensourcephysics.media.core.StrobeFilter;
import org.opensourcephysics.media.core.SumFilter;

/** Runs the actual OSP video filters of the pinned Tracker 6.3.5 JAR on deterministic synthetic frames.
 *
 * Every case drives a FilterStack the way Tracker does: VideoAdapter keeps one TYPE_INT_RGB buffer per
 * video and redraws each frame into it, so the first filter always sees the same image object; the
 * stack is evaluated whenever the filtered image is invalid; TrackerPanel calls SumFilter.addNextImage
 * on every step-number change; PerspectiveFilter refreshes its corners on every video-frame change.
 * Initial parameters are applied through each filter's XML loader (the file-load path), later changes
 * through the setters its inspector calls. The frame generators are integer-only and are repeated in
 * tests/science/video_filter_test.cpp. See docs/video-filters.md for the case matrix.
 */
public final class FilterOracle {
    private static final String CORE = "org.opensourcephysics.media.core.";
    /** TrackerPanel's filter registry in 6.3.5 (BarrelPincushionFilter is commented out there). */
    private static final List<String> REGISTRY = List.of("DeinterlaceFilter", "GhostFilter", "StrobeFilter", "DarkGhostFilter",
        "NegativeFilter", "GrayScaleFilter", "LogFilter", "BrightnessFilter", "BaselineFilter", "SumFilter", "ResizeFilter",
        "RotateFilter", "PerspectiveFilter", "RadialDistortionFilter");
    private static final int FULL_IMAGE_PIXELS = 48; // results this small also store every pixel
    private static final Map<String, Frame> UNIQUE_FRAMES = new LinkedHashMap<>();

    // ---------------------------------------------------------------------------------------------
    // Synthetic frames: integer-only, so Java and C++ generate identical pixels.

    record Frame(String pattern, int width, int height, int seed, int t) {
        Map<String, Object> json() {
            return map("pattern", pattern, "width", width, "height", height, "seed", seed, "t", t);
        }
    }

    private static int xorshift(int state) {
        state ^= state << 13;
        state ^= state >>> 17;
        state ^= state << 5;
        return state;
    }

    private static int seedState(int seed, int t) {
        int state = seed * 0x9E3779B9 ^ (t + 1) * 0x85EBCA6B;
        return state == 0 ? 1 : state;
    }

    private static int rgb(int r, int g, int b) {
        return (r << 16) | (g << 8) | b;
    }

    /** Pixels as Java ints: 0x00RRGGBB, or 0xAARRGGBB for the "argb" pattern. */
    static int[] generate(Frame frame) {
        int w = frame.width(), h = frame.height(), t = frame.t();
        int[] pixels = new int[w * h];
        int state = seedState(frame.seed(), frame.pattern().equals("blob") || frame.pattern().equals("darkblob") ? 0 : t);
        int cx = 4 + 3 * t, cy = 3 + 2 * t;
        for (int y = 0, i = 0; y < h; y++) {
            for (int x = 0; x < w; x++, i++) {
                state = xorshift(state);
                int noise = state;
                boolean disk = (x - cx) * (x - cx) + (y - cy) * (y - cy) <= 16;
                pixels[i] = switch (frame.pattern()) {
                    case "gradient" -> rgb(w > 1 ? x * 255 / (w - 1) : 128, h > 1 ? y * 255 / (h - 1) : 128, (x * 7 + y * 13 + t * 29) & 0xff);
                    case "noise" -> noise & 0xffffff;
                    case "checker" -> new int[] {0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0x00ffff, 0xff00ff, 0xffffff, 0x000000}
                        [(x / 3 + (y / 3) * 3 + t) & 7];
                    case "blob" -> disk ? rgb(235, 215, 130)
                        : rgb(30 + (noise & 15), 35 + ((noise >>> 4) & 15), 40 + ((noise >>> 8) & 15));
                    case "darkblob" -> disk ? rgb(25, 45, 70)
                        : rgb(215 + (noise & 15), 220 + ((noise >>> 4) & 15), 225 + ((noise >>> 8) & 15));
                    case "argb" -> {
                        int phase = (x + y) % 7;
                        int alpha = phase == 0 ? 0 : phase == 1 ? 255 : (x * 37 + y * 61 + frame.seed()) & 0xff;
                        yield (alpha << 24) | (noise & 0xffffff);
                    }
                    default -> throw new IllegalArgumentException("Unknown pattern " + frame.pattern());
                };
            }
        }
        return pixels;
    }

    static BufferedImage image(Frame frame, int type) {
        BufferedImage image = new BufferedImage(frame.width(), frame.height(), type);
        image.getRaster().setDataElements(0, 0, frame.width(), frame.height(), generate(frame));
        return image;
    }

    // ---------------------------------------------------------------------------------------------
    // Cases

    static final class Case {
        final String id;
        final String description;
        final List<Map<String, Object>> filters = new ArrayList<>();
        final List<Frame> frames = new ArrayList<>();
        final List<Map<String, Object>> ops = new ArrayList<>();
        final List<Map<String, Object>> probes = new ArrayList<>();

        Case(String id, String description) {
            this.id = id;
            this.description = description;
        }

        Case filter(String className, Object... params) {
            filters.add(map("class", CORE + className, "params", map(params)));
            return this;
        }

        Case frame(String pattern, int w, int h, int seed, int t) {
            frames.add(new Frame(pattern, w, h, seed, t));
            return this;
        }

        Case frames(String pattern, int w, int h, int seed, int count) {
            for (int t = 0; t < count; t++) frame(pattern, w, h, seed, t);
            return this;
        }

        Case apply(int... frameIndices) {
            for (int frame : frameIndices) ops.add(map("op", "apply", "frame", frame));
            return this;
        }

        Case applyAt(int frame, int videoFrame) {
            ops.add(map("op", "apply", "frame", frame, "video_frame", videoFrame));
            return this;
        }

        /** A step-number change followed by an evaluation (TrackerPanel's STEPNUMBER handler, then repaint). */
        Case stepApply(int... frameIndices) {
            for (int frame : frameIndices) {
                ops.add(map("op", "step"));
                ops.add(map("op", "apply", "frame", frame));
            }
            return this;
        }

        Case step() {
            ops.add(map("op", "step"));
            return this;
        }

        Case set(int filter, String name, Object value) {
            ops.add(map("op", "set", "filter", filter, "name", name, "value", value));
            return this;
        }

        Case enable(int filter, boolean value) {
            ops.add(map("op", "enable", "filter", filter, "value", value));
            return this;
        }

        Case clear(int filter) {
            ops.add(map("op", "clear", "filter", filter));
            return this;
        }

        Case baseline(int filter, int frame) {
            ops.add(map("op", "baseline", "filter", filter, "frame", frame));
            return this;
        }

        Case replaceSource() {
            ops.add(map("op", "replace_source"));
            return this;
        }

        /** Records a filter field after the run, e.g. a parameter the filter adjusts by itself. */
        Case probe(int filter, String name) {
            probes.add(map("filter", filter, "name", name));
            return this;
        }
    }

    private static double[][] quad(double... xy) {
        return new double[][] {{xy[0], xy[1]}, {xy[2], xy[3]}, {xy[4], xy[5]}, {xy[6], xy[7]}};
    }

    private static Map<String, Object> keyed(Object... frameAndQuad) {
        Map<String, Object> keys = new LinkedHashMap<>();
        for (int i = 0; i < frameAndQuad.length; i += 2) keys.put(String.valueOf(frameAndQuad[i]), frameAndQuad[i + 1]);
        return keys;
    }

    private static List<Case> cases() {
        List<Case> cases = new ArrayList<>();
        final int w = 37, h = 23;

        // Point filters.
        for (String pattern : List.of("gradient", "noise", "checker")) {
            cases.add(new Case("negative-" + pattern, "255 - c per channel").filter("NegativeFilter").frame(pattern, w, h, 1, 0).apply(0));
        }
        cases.add(new Case("negative-1x1", "single pixel").filter("NegativeFilter").frame("noise", 1, 1, 3, 0).apply(0));
        Object[][] weights = {{0.30, 0.59, 0.11}, {1.0 / 3, 1.0 / 3, 1.0 / 3}, {0.2, 0.5, 0.9}, {0.0, 0.0, 0.0}, {-1.0, 1.0, 1.0},
            {1.5, 0.0, 0.0}};
        String[] weightNames = {"video", "flat", "custom", "zero", "negative", "overflow"};
        for (int i = 0; i < weights.length; i++) {
            double[] v = {(Double) weights[i][0], (Double) weights[i][1], (Double) weights[i][2]};
            cases.add(new Case("grayscale-" + weightNames[i], "gray = (int)(wr r + wg g + wb b) / (wr + wg + wb)")
                .filter("GrayScaleFilter", "weights", v).frame("noise", w, h, 2, 0).frame("gradient", w, h, 0, 0).apply(0, 1));
        }
        cases.add(new Case("grayscale-set-weights", "inspector weight change").filter("GrayScaleFilter").frame("noise", w, h, 2, 0)
            .apply(0).set(0, "weights", new double[] {0.1, 0.1, 0.8}).apply(0));
        double[][] brightness = {{0, 50}, {64, 50}, {128, 50}, {-104, 50}, {-100, 20}, {0, 0}, {128, 100}, {-128, 75.5}, {30, 99.9},
            {0, 100}, {500, 150}, {-20, 1}, {7, 35}, {-50, 62}};
        for (double[] bc : brightness) {
            cases.add(new Case("brightness-" + (int) bc[0] + "-" + bc[1], "slope (c + offset1) + offset2, truncated and clamped")
                .filter("BrightnessFilter", "brightness", (int) bc[0], "contrast", bc[1]).frame("gradient", w, h, 0, 0)
                .frame("noise", w, h, 4, 0).apply(0, 1));
        }
        cases.add(new Case("brightness-clear", "inspector edits, then Reset").filter("BrightnessFilter").frame("noise", w, h, 4, 0)
            .apply(0).set(0, "brightness", 40).apply(0).set(0, "contrast", 70.0).apply(0).clear(0).apply(0));
        int[] levels = {0, 1, 25, 100, -40, -100, 250, -1000};
        for (int level : levels) {
            for (boolean gray : new boolean[] {false, true}) {
                cases.add(new Case("log-" + level + (gray ? "-gray" : "-hsb"), "highlight/shadow lookup")
                    .filter("LogFilter", "level", level, "grayscale", gray).frame("noise", w, h, 5, 0).frame("checker", w, h, 0, 0)
                    .apply(0, 1));
            }
        }
        cases.add(new Case("log-set-level", "inspector level/grayscale changes").filter("LogFilter").frame("noise", w, h, 5, 0)
            .apply(0).set(0, "level", 60).apply(0).set(0, "grayscale", true).apply(0).set(0, "level", -30).apply(0));
        for (int height : new int[] {23, 24, 1, 2}) {
            for (String field : new String[] {"even", "odd"}) {
                cases.add(new Case("deinterlace-" + field + "-" + height, "duplicate one field's rows")
                    .filter("DeinterlaceFilter", "field", field).frame("noise", 9, height, 6, 0).apply(0));
            }
        }
        cases.add(new Case("deinterlace-set", "switch fields").filter("DeinterlaceFilter").frame("noise", w, h, 6, 0).apply(0)
            .set(0, "field", "odd").apply(0));

        // Temporal filters.
        for (double fade : new double[] {0.05, 0.3, 1.0, 0.0}) {
            cases.add(new Case("ghost-" + fade, "bright ghost trail").filter("GhostFilter", "fade", fade).frames("blob", w, h, 7, 6)
                .apply(0, 1, 2, 3, 4, 5));
            cases.add(new Case("darkghost-" + fade, "dark ghost trail").filter("DarkGhostFilter", "fade", fade)
                .frames("darkblob", w, h, 7, 6).apply(0, 1, 2, 3, 4, 5));
        }
        cases.add(new Case("ghost-reevaluate-seek", "state advances per evaluation, also on repeats and backward seeks")
            .filter("GhostFilter", "fade", 0.2).frames("blob", w, h, 7, 6).apply(0, 1, 2, 2, 2, 5, 1, 0, 3));
        cases.add(new Case("ghost-clear-enable", "clear and enable changes restart from the next frame")
            .filter("GhostFilter", "fade", 0.1).frames("blob", w, h, 7, 6).apply(0, 1, 2).clear(0).apply(3, 4).enable(0, false)
            .apply(5).enable(0, true).apply(0, 1).set(0, "fade", 0.5).apply(2));
        cases.add(new Case("darkghost-fade-limits", "fade is |fade| limited to 1").filter("DarkGhostFilter", "fade", -0.25)
            .frames("darkblob", w, h, 7, 4).apply(0, 1).set(0, "fade", 3.0).apply(2, 3));
        for (double fade : new double[] {0.0, 0.2}) {
            for (boolean bright : new boolean[] {false, true}) {
                String pattern = bright ? "blob" : "darkblob";
                cases.add(new Case("strobe-" + (bright ? "bright-" : "dark-") + fade, "strobe trail")
                    .filter("StrobeFilter", "fade", fade, "bright_trails", bright).frames(pattern, w, h, 8, 6).apply(0, 1, 2, 3, 4, 5));
            }
        }
        cases.add(new Case("strobe-mode-switch", "bright-trail switch clears; repeats fade further")
            .filter("StrobeFilter", "fade", 0.1).frames("blob", w, h, 8, 6).apply(0, 1, 1, 2).set(0, "bright_trails", true).apply(3, 4, 4)
            .clear(0).apply(5).enable(0, false).apply(0).enable(0, true).apply(1));
        cases.add(new Case("sum-steps", "sums one image per step change").filter("SumFilter").frames("noise", w, h, 9, 5)
            .apply(0).stepApply(1, 2).apply(2).stepApply(3).set(0, "mean", true).apply(3).stepApply(4));
        cases.add(new Case("sum-brightness", "fraction of the sum").filter("SumFilter").frames("noise", w, h, 9, 4)
            .set(0, "brightness", 0.5).apply(0).stepApply(1, 2).set(0, "brightness", -0.25).apply(2).set(0, "mean", true).apply(2)
            .set(0, "mean", false).stepApply(3));
        cases.add(new Case("sum-clear-disable", "clear restarts from the current input; disabled steps still count")
            .filter("SumFilter").frames("noise", w, h, 9, 5).apply(0).stepApply(1).clear(0).apply(1).stepApply(2)
            .enable(0, false).step().apply(3).enable(0, true).apply(3).step().step().apply(4));
        cases.add(new Case("sum-seek-back", "a backward step change also adds the image").filter("SumFilter").frames("blob", w, h, 9, 4)
            .set(0, "mean", true).apply(0).stepApply(1, 2, 1, 0));
        cases.add(new Case("baseline-none", "no baseline copies the input").filter("BaselineFilter").frame("noise", w, h, 10, 0).apply(0));
        cases.add(new Case("baseline-same", "subtract a same-size baseline").filter("BaselineFilter").frame("noise", w, h, 10, 0)
            .frame("gradient", w, h, 0, 0).baseline(0, 1).apply(0).clear(0).apply(0));
        cases.add(new Case("baseline-larger", "a larger baseline contributes its top-left region").filter("BaselineFilter")
            .frame("noise", w, h, 10, 0).frame("noise", 45, 30, 11, 0).apply(0).baseline(0, 1).apply(0));

        // Geometric filters.
        double[][] factors = {{1, 1}, {0.5, 0.5}, {2, 2}, {2.5, 0.37}, {1.00005, 1}, {1.000001, 0.9999999}, {0.3, 3}, {10, 10},
            {-0.75, 1.25}};
        for (double[] f : factors) {
            cases.add(new Case("resize-" + f[0] + "x" + f[1], "Java2D nearest-neighbour ScaledBlit")
                .filter("ResizeFilter", "width_factor", f[0], "height_factor", f[1]).frame("noise", f[0] == 10 ? 5 : w, f[0] == 10 ? 4 : h, 12, 0)
                .apply(0));
        }
        cases.add(new Case("resize-dv-720x480", "720x480 input at unit factors becomes 640 wide").filter("ResizeFilter")
            .frame("gradient", 720, 480, 0, 0).apply(0).probe(0, "width_factor").probe(0, "height_factor"));
        cases.add(new Case("resize-tiles-1920x1080", "large outputs take ScaledBlit's tiled path")
            .filter("ResizeFilter", "width_factor", 2.0, "height_factor", 1.9).frame("gradient", 1920, 1080, 0, 0).apply(0));
        cases.add(new Case("resize-set", "factor edits reinitialize").filter("ResizeFilter").frame("noise", w, h, 12, 0).apply(0)
            .set(0, "width_factor", 0.6).apply(0).set(0, "height_factor", 1.7).apply(0));
        for (String rotation : new String[] {"None", "CCW", "CW", "180"}) {
            for (boolean reverse : new boolean[] {false, true}) {
                cases.add(new Case("rotate-" + rotation + (reverse ? "-reverse" : ""), "quarter turns after an optional mirror")
                    .filter("RotateFilter", "rotation", rotation, "reverse", reverse).frame("gradient", w, h, 0, 0).frame("noise", 5, 3, 13, 0)
                    .apply(0, 1));
            }
        }
        cases.add(new Case("rotate-set", "inspector rotation and reverse changes").filter("RotateFilter").frame("noise", w, h, 13, 0)
            .apply(0).set(0, "rotation", "CW").apply(0).set(0, "reverse", true).apply(0).set(0, "rotation", "None").apply(0));
        cases.add(new Case("perspective-default", "default corners: in == out").filter("PerspectiveFilter").frame("noise", w, h, 14, 0)
            .apply(0));
        cases.add(new Case("perspective-trapezoid", "trapezoid to rectangle").filter("PerspectiveFilter", "fixed_in", true,
            "fixed_out", true, "in_corners", keyed(0, quad(8.5, 4.25, 30, 6, 34.75, 20, 2, 18)),
            "out_corners", keyed(0, quad(3, 2, 33, 2, 33, 20, 3, 20))).frame("gradient", w, h, 0, 0).frame("noise", w, h, 14, 0).apply(0, 1));
        cases.add(new Case("perspective-outside", "corners beyond the image and a rotated output")
            .filter("PerspectiveFilter", "fixed_in", true, "fixed_out", true, "in_corners", keyed(0, quad(-5, -3, 41, 0, 39, 27, -2, 25)),
                "out_corners", keyed(0, quad(30, 2, 34, 20, 6, 21, 2, 3))).frame("checker", w, h, 0, 0).apply(0));
        cases.add(new Case("perspective-degenerate", "collinear input corners make a singular matrix")
            .filter("PerspectiveFilter", "fixed_in", true, "fixed_out", true, "in_corners", keyed(0, quad(1, 1, 10, 10, 20, 20, 30, 30)),
                "out_corners", keyed(0, quad(3, 2, 33, 2, 33, 20, 3, 20))).frame("noise", w, h, 14, 0).apply(0));
        cases.add(new Case("perspective-keyframes", "unfixed input corners step at key frames")
            .filter("PerspectiveFilter", "fixed_in", false, "fixed_out", true,
                "in_corners", keyed(0, quad(8, 4, 30, 6, 34, 20, 2, 18), 3, quad(6, 3, 31, 2, 33, 21, 4, 19), 7, quad(5, 5, 25, 4, 30, 18, 2, 20)),
                "out_corners", keyed(0, quad(3, 2, 33, 2, 33, 20, 3, 20)))
            .frame("gradient", w, h, 0, 0).applyAt(0, 0).applyAt(0, 2).applyAt(0, 3).applyAt(0, 6).applyAt(0, 9).applyAt(0, 1));
        cases.add(new Case("perspective-small", "1-, 2- and 3-pixel images take the edge path").filter("PerspectiveFilter",
            "fixed_in", true, "fixed_out", true, "in_corners", keyed(0, quad(0.25, 0.5, 2.5, 0, 2.75, 2.5, 0, 2)),
            "out_corners", keyed(0, quad(0, 0, 3, 0, 3, 3, 0, 3))).frame("noise", 3, 3, 15, 0).frame("noise", 2, 2, 15, 0)
            .frame("noise", 1, 1, 15, 0).apply(0, 1, 2));
        String[] types = {"Rectilinear", "Equisolid", "Equidistant", "Stereographic", "Orthographic"};
        double[][] radial = {{0, 0, 0.75, 90}, {1, 0, 0.75, 120}, {2, 3, 0.5, 100}, {4, 0, 0.75, 170}, {0, 1, 0.9, 60}, {3, 4, 0.3, 150},
            {0, 2, 1.0, 10}, {4, 2, 0.2, 179.9}, {1, 1, 0.75, 90}, {0, 1, 0.75, 170}, {0, 3, 0.5, 150}, {0, 4, 0.75, 120}};
        for (double[] r : radial) {
            String in = types[(int) r[0]], out = types[(int) r[1]];
            cases.add(new Case("radial-" + in + "-" + out + "-" + r[2] + "-" + r[3], "radial projection change")
                .filter("RadialDistortionFilter", "input_type", in, "output_type", out, "fixed_radius", r[2], "input_fov", Math.toRadians(r[3]))
                .frame("checker", 64, 48, 0, 0).frame("noise", w, h, 16, 0).apply(0, 1).probe(0, "fixed_radius").probe(0, "output_fov")
                .probe(0, "lower_radius_limit"));
        }
        cases.add(new Case("radial-set", "inspector edits invalidate the transform").filter("RadialDistortionFilter")
            .frame("checker", 64, 48, 0, 0).apply(0).set(0, "input_type", "Equisolid").apply(0).set(0, "fixed_radius", 0.4).apply(0)
            .set(0, "input_fov", Math.toRadians(150)).apply(0).set(0, "output_type", "Stereographic").apply(0).set(0, "fixed_radius", 0.05)
            .apply(0).probe(0, "fixed_radius"));
        cases.add(new Case("radial-small", "edge and single-pixel images").filter("RadialDistortionFilter", "input_type", "Equidistant")
            .frame("noise", 3, 2, 17, 0).frame("noise", 1, 1, 17, 0).apply(0, 1));

        // Stacks.
        cases.add(new Case("stack-disabled", "an all-disabled stack returns the source").filter("NegativeFilter").filter("GhostFilter")
            .frames("blob", w, h, 7, 3).apply(0).enable(0, false).enable(1, false).apply(1).enable(1, true).apply(2));
        cases.add(new Case("stack-brightness-ghost", "disabling an upstream filter re-initializes the ghost")
            .filter("BrightnessFilter", "brightness", 20, "contrast", 60.0).filter("GhostFilter", "fade", 0.1).frames("blob", w, h, 7, 6)
            .apply(0, 1, 2).enable(0, false).apply(3).enable(0, true).apply(4).set(0, "brightness", -10).apply(5));
        cases.add(new Case("stack-resize-strobe", "an upstream resize change re-initializes the strobe")
            .filter("ResizeFilter", "width_factor", 0.8, "height_factor", 0.8).filter("StrobeFilter", "fade", 0.1, "bright_trails", true)
            .frames("blob", w, h, 8, 6).apply(0, 1, 2).set(0, "width_factor", 1.2).apply(3, 4).set(0, "height_factor", 1.2).apply(5));
        cases.add(new Case("stack-ghost-strobe-sum", "clearing a ghost re-initializes everything after it")
            .filter("GhostFilter", "fade", 0.1).filter("StrobeFilter", "fade", 0.05, "bright_trails", true).filter("SumFilter")
            .frames("blob", w, h, 7, 6).apply(0).stepApply(1, 2).clear(0).stepApply(3).set(2, "mean", true).stepApply(4, 5));
        cases.add(new Case("stack-geometry", "rotate, perspective and radial distortion in sequence").filter("RotateFilter", "rotation", "CW")
            .filter("PerspectiveFilter", "fixed_in", true, "fixed_out", true, "in_corners", keyed(0, quad(2, 3, 20, 4, 21, 33, 1, 35)),
                "out_corners", keyed(0, quad(1, 1, 22, 1, 22, 36, 1, 36)))
            .filter("RadialDistortionFilter", "input_type", "Equisolid").filter("NegativeFilter").frame("gradient", w, h, 0, 0).apply(0));
        cases.add(new Case("stack-replace-source", "a new source buffer re-initializes temporal filters").filter("GhostFilter", "fade", 0.1)
            .filter("SumFilter").frames("blob", w, h, 7, 4).apply(0).stepApply(1).replaceSource().stepApply(2, 3));
        cases.add(new Case("stack-size-change", "a different frame size re-initializes every filter").filter("StrobeFilter", "fade", 0.2)
            .filter("RotateFilter", "rotation", "CCW").frames("blob", w, h, 8, 3).frame("noise", 20, 11, 18, 0).apply(0, 1, 3, 2));
        return cases;
    }

    // ---------------------------------------------------------------------------------------------
    // Running a case through FilterStack

    private static Filter create(Map<String, Object> spec) throws ReflectiveOperationException {
        Filter filter = (Filter) Class.forName((String) spec.get("class")).getDeclaredConstructor().newInstance();
        @SuppressWarnings("unchecked")
        Map<String, Object> params = (Map<String, Object>) spec.get("params");
        if (!params.isEmpty()) {
            XMLControlElement control = new XMLControlElement(filter.getClass());
            for (Map.Entry<String, Object> param : params.entrySet()) {
                control.setValue(param.getKey(), xmlValue(param.getValue()));
            }
            control.loadObject(filter);
        }
        return filter;
    }

    /** Perspective key frames are saved as double[frame][corner][xy] with null for frames that are not keys. */
    private static Object xmlValue(Object value) {
        if (value instanceof Map<?, ?> keys) {
            int last = keys.keySet().stream().mapToInt(key -> Integer.parseInt((String) key)).max().orElse(-1);
            double[][][] data = new double[last + 1][][];
            keys.forEach((key, corners) -> data[Integer.parseInt((String) key)] = (double[][]) corners);
            return data;
        }
        return value;
    }

    private static Object invoke(Object target, String name, Class<?> type, Object value) throws ReflectiveOperationException {
        Method method = target.getClass().getDeclaredMethod(name, type);
        method.setAccessible(true);
        return method.invoke(target, value);
    }

    private static Object field(Object target, String name) throws ReflectiveOperationException {
        Class<?> type = target.getClass();
        while (type != null) {
            try {
                Field field = type.getDeclaredField(name);
                field.setAccessible(true);
                return field.get(target);
            } catch (NoSuchFieldException missing) {
                type = type.getSuperclass();
            }
        }
        throw new NoSuchFieldException(name);
    }

    private static void setField(Object target, String name, Object value) throws ReflectiveOperationException {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    private static int rotationType(String name) {
        return switch (name) {
            case "None" -> -1;
            case "CCW" -> 0;
            case "CW" -> 1;
            case "180" -> 2;
            default -> throw new IllegalArgumentException(name);
        };
    }

    /** The setter each filter's inspector calls for a changed field. */
    private static void set(Filter filter, String name, Object value) throws ReflectiveOperationException {
        String key = filter.getClass().getSimpleName() + "." + name;
        switch (key) {
            case "BrightnessFilter.brightness" -> ((BrightnessFilter) filter).setBrightness((Integer) value);
            case "BrightnessFilter.contrast" -> ((BrightnessFilter) filter).setContrast((Double) value);
            case "GrayScaleFilter.weights" -> {
                double[] w = (double[]) value;
                ((GrayScaleFilter) filter).setWeights(w[0], w[1], w[2]);
            }
            case "LogFilter.level" -> invoke(filter, "setLevel", int.class, value);
            case "LogFilter.grayscale" -> invoke(filter, "setGrayscale", boolean.class, value);
            case "DeinterlaceFilter.field" -> ((DeinterlaceFilter) filter).setOdd("odd".equals(value));
            case "GhostFilter.fade", "DarkGhostFilter.fade" -> ((GhostFilter) filter).setFade((Double) value);
            case "StrobeFilter.fade" -> ((StrobeFilter) filter).setFade((Double) value);
            case "StrobeFilter.bright_trails" -> ((StrobeFilter) filter).setBrightTrails((Boolean) value);
            case "SumFilter.brightness" -> ((SumFilter) filter).setBrightness((Double) value);
            case "SumFilter.mean" -> ((SumFilter) filter).setMean((Boolean) value);
            case "ResizeFilter.width_factor" -> ((ResizeFilter) filter).setWidthFactor((Double) value);
            case "ResizeFilter.height_factor" -> ((ResizeFilter) filter).setHeightFactor((Double) value);
            case "RotateFilter.rotation" -> {
                Method method = filter.getClass().getDeclaredMethod("setRotationType", int.class);
                method.setAccessible(true);
                method.invoke(filter, rotationType((String) value));
            }
            case "RotateFilter.reverse" -> setField(filter, "reverse", value);
            case "RadialDistortionFilter.fixed_radius" -> ((RadialDistortionFilter) filter).setFixedRadius((Double) value);
            case "RadialDistortionFilter.input_fov" -> ((RadialDistortionFilter) filter).setSourceFOV((Double) value);
            case "RadialDistortionFilter.input_type" -> ((RadialDistortionFilter) filter).setSourceProjectionType((String) value);
            case "RadialDistortionFilter.output_type" -> ((RadialDistortionFilter) filter).setOutputProjectionType((String) value);
            default -> throw new IllegalArgumentException("No setter for " + key);
        }
    }

    private static Object probe(Filter filter, String name) throws ReflectiveOperationException {
        return switch (filter.getClass().getSimpleName() + "." + name) {
            case "ResizeFilter.width_factor" -> ((ResizeFilter) filter).getWidthFactor();
            case "ResizeFilter.height_factor" -> ((ResizeFilter) filter).getHeightFactor();
            case "RadialDistortionFilter.fixed_radius" -> ((RadialDistortionFilter) filter).getFixedRadius();
            case "RadialDistortionFilter.output_fov" -> field(filter, "outputFOV");
            case "RadialDistortionFilter.lower_radius_limit" -> field(filter, "lowerRadiusLimit");
            default -> throw new IllegalArgumentException("No probe " + name);
        };
    }

    private static Map<String, Object> run(Case c) throws Exception {
        List<Filter> filters = new ArrayList<>();
        FilterStack stack = new FilterStack();
        for (Map<String, Object> spec : c.filters) {
            Filter filter = create(spec);
            filters.add(filter);
            stack.addFilter(filter);
        }
        Method refreshCorners = PerspectiveFilter.class.getDeclaredMethod("refreshCorners", int.class);
        refreshCorners.setAccessible(true);
        BufferedImage buffer = null; // VideoAdapter.bufferedImage
        List<Object> results = new ArrayList<>();
        for (Map<String, Object> op : c.ops) {
            switch ((String) op.get("op")) {
                case "apply" -> {
                    Frame frame = c.frames.get((Integer) op.get("frame"));
                    if (buffer == null || buffer.getWidth() != frame.width() || buffer.getHeight() != frame.height()) {
                        buffer = new BufferedImage(frame.width(), frame.height(), BufferedImage.TYPE_INT_RGB);
                    }
                    buffer.getRaster().setDataElements(0, 0, frame.width(), frame.height(), generate(frame));
                    int videoFrame = (Integer) op.getOrDefault("video_frame", 0);
                    for (Filter filter : filters) {
                        // VideoAdapter fires PROPERTY_VIDEO_NEXTFRAME only after the filter has key frames.
                        if (filter instanceof PerspectiveFilter && !((TreeSet<?>) field(filter, "inKeyFrames")).isEmpty()
                                && !((TreeSet<?>) field(filter, "outKeyFrames")).isEmpty()) {
                            refreshCorners.invoke(filter, videoFrame);
                        }
                    }
                    results.add(describe(stack.getFilteredImage(buffer)));
                }
                case "step" -> {
                    for (Filter filter : filters) {
                        if (filter instanceof SumFilter sum) sum.addNextImage();
                    }
                }
                case "set" -> set(filters.get((Integer) op.get("filter")), (String) op.get("name"), op.get("value"));
                case "enable" -> filters.get((Integer) op.get("filter")).setEnabled((Boolean) op.get("value"));
                case "clear" -> filters.get((Integer) op.get("filter")).clear();
                case "baseline" -> {
                    Frame frame = c.frames.get((Integer) op.get("frame"));
                    if (!((BaselineFilter) filters.get((Integer) op.get("filter"))).setBaselineImage(image(frame, BufferedImage.TYPE_INT_RGB))) {
                        throw new IllegalStateException("baseline rejected");
                    }
                }
                case "replace_source" -> buffer = null;
                default -> throw new IllegalArgumentException("Unknown op " + op.get("op"));
            }
        }
        List<Object> probes = new ArrayList<>();
        for (Map<String, Object> p : c.probes) {
            Object value = probe(filters.get((Integer) p.get("filter")), (String) p.get("name"));
            probes.add(map("filter", p.get("filter"), "name", p.get("name"), "value", value));
        }
        List<Object> frames = new ArrayList<>();
        for (Frame frame : c.frames) {
            frames.add(frame.json());
            UNIQUE_FRAMES.putIfAbsent(frame.toString(), frame);
        }
        Map<String, Object> json = map("id", c.id, "description", c.description, "filters", c.filters, "frames", frames, "ops", c.ops,
            "results", results);
        if (!probes.isEmpty()) json.put("probes", probes);
        return json;
    }

    private static Map<String, Object> describe(BufferedImage image) throws Exception {
        int w = image.getWidth(), h = image.getHeight();
        int[] pixels = new int[w * h];
        image.getRaster().getDataElements(0, 0, w, h, pixels);
        Map<String, Object> result = map("width", w, "height", h, "sha256", sha256(pixels, w, h));
        if (w * h <= FULL_IMAGE_PIXELS) {
            StringBuilder rgb = new StringBuilder();
            for (int pixel : pixels) rgb.append(String.format("%06x", pixel & 0xffffff));
            result.put("rgb", rgb.toString());
        }
        return result;
    }

    /** SHA-256 of the visible pixels: R, G, B bytes per pixel in row-major order (the alpha byte is ignored). */
    static String sha256(int[] pixels, int w, int h) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        byte[] bytes = new byte[w * h * 3];
        for (int i = 0; i < w * h; i++) {
            bytes[3 * i] = (byte) (pixels[i] >> 16);
            bytes[3 * i + 1] = (byte) (pixels[i] >> 8);
            bytes[3 * i + 2] = (byte) pixels[i];
        }
        return hex(digest.digest(bytes));
    }

    private static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder();
        for (byte b : bytes) text.append(String.format("%02x", b & 0xff));
        return text.toString();
    }

    // ---------------------------------------------------------------------------------------------
    // Supporting probes

    /** drawImage(TYPE_INT_ARGB onto TYPE_INT_RGB): VideoAdapter's frame buffer and Filter.initializeSource. */
    private static List<Object> composites() throws Exception {
        List<Object> list = new ArrayList<>();
        Frame source = new Frame("argb", 23, 17, 19, 0);
        for (Frame background : new Frame[] {null, new Frame("gradient", 23, 17, 0, 0)}) {
            BufferedImage destination = background == null ? new BufferedImage(23, 17, BufferedImage.TYPE_INT_RGB)
                : image(background, BufferedImage.TYPE_INT_RGB);
            Graphics2D graphics = destination.createGraphics();
            graphics.drawImage(image(source, BufferedImage.TYPE_INT_ARGB), 0, 0, null);
            graphics.dispose();
            Map<String, Object> result = describe(destination);
            result.put("source", source.json());
            result.put("background", background == null ? null : background.json());
            list.add(result);
        }
        // A filter handed a non-RGB image converts it the same way before filtering.
        FilterStack stack = new FilterStack();
        stack.addFilter((Filter) Class.forName(CORE + "NegativeFilter").getDeclaredConstructor().newInstance());
        Map<String, Object> negative = describe(stack.getFilteredImage(image(source, BufferedImage.TYPE_INT_ARGB)));
        negative.put("source", source.json());
        negative.put("filter", CORE + "NegativeFilter");
        list.add(negative);
        return list;
    }

    /** Bits of the transcendental values whose last ulp decides truncated pixel values. */
    private static Map<String, Object> math() throws Exception {
        List<Object> trig = new ArrayList<>();
        TreeSet<Double> contrasts = new TreeSet<>();
        for (int c = 0; c <= 100; c++) contrasts.add((double) c);
        contrasts.addAll(List.of(75.5, 99.9, 150.0, 70.0, 60.0, 1.0, -37.5, 1000.25, 123456.5));
        for (double contrast : contrasts) {
            double theta = Math.PI * contrast / 200;
            trig.add(map("contrast", contrast, "sin", bits(Math.sin(theta)), "cos", bits(Math.cos(theta))));
        }
        Map<String, Object> lookups = new TreeMap<>((a, b) -> Integer.compare(Integer.parseInt(a), Integer.parseInt(b)));
        List<Integer> levels = new ArrayList<>();
        for (int level = -100; level <= 100; level++) levels.add(level);
        levels.addAll(List.of(250, -250, 1000, -1000, 100000, Integer.MIN_VALUE, Integer.MAX_VALUE - 1));
        for (int level : levels) {
            Filter log = (Filter) Class.forName(CORE + "LogFilter").getDeclaredConstructor().newInstance();
            invoke(log, "setLevel", int.class, level);
            int[] lookup = (int[]) field(log, "lookup");
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            for (int value : lookup) digest.update(new byte[] {(byte) (value >> 24), (byte) (value >> 16), (byte) (value >> 8), (byte) value});
            lookups.put(String.valueOf(level), hex(digest.digest()).substring(0, 16));
        }
        return map("brightness_trig", trig, "log_lookup_sha256_prefix", lookups);
    }

    private static String bits(double value) {
        return String.format("%016x", Double.doubleToRawLongBits(value));
    }

    /** Java exceptions that MoTrack replaces with defined results (docs/video-filters.md). */
    private static List<Object> failures() throws Exception {
        List<Object> list = new ArrayList<>();
        // A resize whose output has a zero dimension: new BufferedImage(0, h) throws.
        Filter resize = create(map("class", CORE + "ResizeFilter", "params", map("width_factor", 0.01)));
        list.add(map("id", "resize-zero-width", "exception", evaluate(resize, new Frame("noise", 50, 40, 20, 0), null)));
        // Perspective key frames without key 0 (edited XML): frames before the first key have no corners.
        Filter perspective = create(map("class", CORE + "PerspectiveFilter", "params", map("fixed_in", false, "fixed_out", false,
            "in_corners", keyed(5, quad(8, 4, 30, 6, 34, 20, 2, 18)), "out_corners", keyed(5, quad(3, 2, 33, 2, 33, 20, 3, 20)))));
        list.add(map("id", "perspective-no-key-0", "exception", evaluate(perspective, new Frame("noise", 37, 23, 14, 0), 0)));
        return list;
    }

    /** Evaluates one filter like the cases do and names the exception it throws, or "none". */
    private static String evaluate(Filter filter, Frame frame, Integer videoFrame) throws Exception {
        FilterStack stack = new FilterStack();
        stack.addFilter(filter);
        try {
            if (videoFrame != null) {
                Method refreshCorners = PerspectiveFilter.class.getDeclaredMethod("refreshCorners", int.class);
                refreshCorners.setAccessible(true);
                try {
                    refreshCorners.invoke(filter, videoFrame);
                } catch (java.lang.reflect.InvocationTargetException error) {
                    throw (Exception) error.getCause();
                }
            }
            stack.getFilteredImage(image(frame, BufferedImage.TYPE_INT_RGB));
            return "none";
        } catch (RuntimeException error) {
            return error.getClass().getSimpleName();
        }
    }

    /** What each Loader writes and whether a disabled filter reopens disabled. */
    private static List<Object> persistence() throws Exception {
        List<Object> list = new ArrayList<>();
        for (String name : REGISTRY) {
            Filter filter = (Filter) Class.forName(CORE + name).getDeclaredConstructor().newInstance();
            if (!Filter.class.isAssignableFrom(filter.getClass())) throw new IllegalStateException(name);
            filter.setEnabled(false);
            XMLControl saved = new XMLControlElement(filter);
            List<String> properties = new ArrayList<>(saved.getPropertyNamesRaw());
            properties.sort(null);
            String reloaded;
            try {
                Filter copy = (Filter) new XMLControlElement(saved.toXML()).loadObject(null);
                reloaded = copy == null ? "null" : copy instanceof PerspectiveFilter p ? (p.isSuperEnabled() ? "enabled" : "disabled")
                    : (copy.isEnabled() ? "enabled" : "disabled");
            } catch (RuntimeException error) {
                reloaded = error.getClass().getSimpleName();
            }
            list.add(map("class", CORE + name, "saved_properties", properties, "disabled_reloads_as", reloaded));
        }
        return list;
    }

    // ---------------------------------------------------------------------------------------------
    // Minimal JSON writer (insertion-ordered maps, finite numbers only)

    static Map<String, Object> map(Object... keyValues) {
        Map<String, Object> map = new LinkedHashMap<>();
        for (int i = 0; i < keyValues.length; i += 2) map.put((String) keyValues[i], keyValues[i + 1]);
        return map;
    }

    private static void json(StringBuilder out, Object value) {
        if (value == null) {
            out.append("null");
        } else if (value instanceof String s) {
            out.append('"');
            for (char ch : s.toCharArray()) {
                if (ch == '"' || ch == '\\') out.append('\\').append(ch);
                else if (ch < 0x20) out.append(String.format("\\u%04x", (int) ch));
                else out.append(ch);
            }
            out.append('"');
        } else if (value instanceof Boolean || value instanceof Integer || value instanceof Long) {
            out.append(value);
        } else if (value instanceof Double d) {
            if (!Double.isFinite(d)) throw new IllegalArgumentException("non-finite JSON number");
            out.append(d == Math.rint(d) && Math.abs(d) < 1e15 ? String.valueOf(d) : Double.toString(d));
        } else if (value instanceof Map<?, ?> m) {
            out.append('{');
            boolean first = true;
            for (Map.Entry<?, ?> entry : m.entrySet()) {
                if (!first) out.append(',');
                first = false;
                json(out, (String) entry.getKey());
                out.append(':');
                json(out, entry.getValue());
            }
            out.append('}');
        } else if (value instanceof List<?> l) {
            out.append('[');
            for (int i = 0; i < l.size(); i++) {
                if (i > 0) out.append(',');
                json(out, l.get(i));
            }
            out.append(']');
        } else if (value instanceof double[] d) {
            List<Object> list = new ArrayList<>();
            for (double x : d) list.add(x);
            json(out, list);
        } else if (value instanceof double[][] d) {
            json(out, Arrays.asList((Object[]) d));
        } else {
            throw new IllegalArgumentException("Cannot write " + value.getClass());
        }
    }

    public static void main(String[] args) {
        try {
            if (args.length != 1) throw new IllegalArgumentException("Usage: FilterOracle output.json");
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            List<Object> registry = new ArrayList<>();
            for (String name : REGISTRY) registry.add(CORE + name);
            List<Object> results = new ArrayList<>();
            for (Case c : cases()) results.add(run(c));
            // Every generated frame once, with the hash that checks the C++ generator.
            List<Object> frames = new ArrayList<>();
            for (Frame frame : UNIQUE_FRAMES.values()) {
                Map<String, Object> json = frame.json();
                json.put("sha256", sha256(generate(frame), frame.width(), frame.height()));
                frames.add(json);
            }
            Map<String, Object> document = map("schema_version", 1, "reference_version", OSPRuntime.VERSION, "registry", registry,
                "math", math(), "frames", frames, "composites", composites(), "persistence", persistence(), "failures", failures(),
                "cases", results);
            StringBuilder out = new StringBuilder();
            json(out, document);
            Files.writeString(Path.of(args[0]), out.append('\n'), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
